#pragma once

#include <stdint.h>

/* The RX half of the MCP2515 path, as the interrupt service routine sees it.
 *
 * Header-only, and free of Arduino, FreeRTOS and ESP-IDF on purpose. The drain
 * drains received frames inside an IRAM interrupt so that they keep moving while
 * the flash cache is off, and a call into flash from there is a crash rather
 * than a slowdown - so the ISR's RX path may only contain code the compiler can
 * inline into it. Keeping that code here, in a header with no dependencies, is
 * what makes the property checkable by reading one file.
 *
 * The second reason is that the two pieces with real logic - the ring's
 * fullness arithmetic and the decode of a raw RX buffer - are then testable on
 * the host, which nothing inside an ISR ever is.
 *
 * MCP2515_ISR_INLINE is why "can be inlined" is not left as a hope.
 * `inline` is permission, not instruction: at -Os GCC emitted ONE out-of-line
 * copy of the decode into .flash.text and had the IRAM drain reach it with an
 * l32r/callx8, which is precisely the fetch this design exists to avoid - and a
 * source scan cannot see it, because where a header function ends up is the
 * compiler's decision rather than the source's. always_inline is a hard error
 * when it cannot be honoured, so the firmware build itself now enforces what
 * the comment above claims.
 */
#define MCP2515_ISR_INLINE inline __attribute__((always_inline))

// Byte offsets inside the 13 bytes an MCP2515 READ RX BUFFER (0x90/0x94)
// returns after the command byte.
#define MCP2515_RXB_SIDH 0
#define MCP2515_RXB_SIDL 1
#define MCP2515_RXB_DLC 4
#define MCP2515_RXB_DATA 5
#define MCP2515_RXB_LENGTH 13

// IDE bit in RXBnSIDL: set means the frame carries a 29-bit identifier.
#define MCP2515_RXB_SIDL_IDE 0x08

/* Frames the interrupt drain can hold while no task is running to take them.
 *
 * A flash write parks every task for tens of milliseconds, and frames go on
 * arriving through it, so the depth decides whether they survive. It is sized
 * like the other drivers' rings (communication/can/can_rx_ring_depth.h): 2,000
 * frames/s through a 94 ms stall needs 188. The ring indexes by mask, so the
 * depth must be a power of two, and 256 is the first one past 188. That is 128
 * ms of cover at 2,000 frames/s, for 256 x 16 B = 4,096 B of internal DRAM
 * (the old depth of 64 cost 1,024 B).
 */
#define MCP2515_LITE_ISR_RING_DEPTH 256

static MCP2515_ISR_INLINE uint32_t mcp2515_unpack_extended_id(const uint8_t* buffer) {
  return ((uint32_t)buffer[0] << 21) | ((uint32_t)(buffer[1] & 0xE0) << 13) | ((uint32_t)(buffer[1] & 0x03) << 16) |
         ((uint32_t)buffer[2] << 8) | buffer[3];
}

static MCP2515_ISR_INLINE uint32_t mcp2515_unpack_standard_id(const uint8_t* buffer) {
  return ((uint32_t)buffer[0] << 3) | (buffer[1] >> 5);
}

/* Decode the 13 bytes of one RX buffer into a frame.
 *
 * `rxb` points at RXBnSIDH, i.e. past the command byte the read was issued
 * with. Templated on the frame type only so that this header stays free of
 * mcp2515_lite.h (which pulls in SPI and FreeRTOS); the one caller in the
 * firmware and the tests both pass MCP2515_Lite_Frame's layout.
 */
template <typename Frame>
static MCP2515_ISR_INLINE void mcp2515_decode_rx_buffer(const uint8_t* rxb, Frame& out) {
  out.flags = 0;
  if (rxb[MCP2515_RXB_SIDL] & MCP2515_RXB_SIDL_IDE) {
    out.ext = true;
    out.id = mcp2515_unpack_extended_id(&rxb[MCP2515_RXB_SIDH]);
  } else {
    out.ext = false;
    out.id = mcp2515_unpack_standard_id(&rxb[MCP2515_RXB_SIDH]);
  }
  // The DLC field's low nibble can read up to 15 on a malformed frame; the chip
  // only ever fills 8 data bytes, so anything above that would read past them.
  const uint8_t dlc = rxb[MCP2515_RXB_DLC] & 0x0F;
  out.dlc = dlc > 8 ? 8 : dlc;
  for (uint8_t i = 0; i < out.dlc; i++) {
    out.data[i] = rxb[MCP2515_RXB_DATA + i];
  }
}

/* Single-producer single-consumer ring, sized in frames.
 *
 * The producer is the ISR, and only the ISR: the task's copy was removed
 * of the drain once the pin became level triggered, so there is one writer of
 * `_head` rather than two that take turns. The consumer is receiveFrame()'s
 * caller and the only writer of `_tail`. No lock: a lock taken in an ISR that a
 * frozen task holds is a spin through the whole flash window, which is the one
 * thing this path must not do.
 *
 * Indices are free-running and compared by difference, so a wrap of the 32-bit
 * counter is not a special case; `Capacity` being a power of two is what makes
 * the slot index a mask.
 */
template <typename Frame, uint32_t Capacity>
class Mcp2515RxRing {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

 public:
  // Producer side. Returns false and counts a drop when the ring is full.
  MCP2515_ISR_INLINE bool push(const Frame& frame) {
    const uint32_t head = _head;
    if ((uint32_t)(head - _tail) >= Capacity) {
      _dropped = _dropped + 1;
      return false;
    }
    _slots[head & (Capacity - 1)] = frame;
    // The slot must be visible before the index that publishes it.
    __sync_synchronize();
    _head = head + 1;
    return true;
  }

  // Consumer side.
  bool pop(Frame& frame) {
    const uint32_t tail = _tail;
    if (tail == _head) {
      return false;
    }
    frame = _slots[tail & (Capacity - 1)];
    // The read must complete before the slot is handed back to the producer.
    __sync_synchronize();
    _tail = tail + 1;
    return true;
  }

  uint32_t size() const { return (uint32_t)(_head - _tail); }
  uint32_t capacity() const { return Capacity; }
  uint32_t dropped() const { return _dropped; }
  void resetDropped() { _dropped = 0; }

 private:
  volatile uint32_t _head = 0;
  volatile uint32_t _tail = 0;
  volatile uint32_t _dropped = 0;
  Frame _slots[Capacity];
};
