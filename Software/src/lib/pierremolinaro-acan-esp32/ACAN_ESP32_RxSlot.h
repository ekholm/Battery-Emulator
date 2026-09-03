//------------------------------------------------------------------------------
//   Reading one slot of the TWAI receive FIFO.
//
//   Split out of ACAN_ESP32 so it can run on the host: the controller's
//   registers are a base address plus a byte offset, and nothing about reading
//   a slot needs an ESP header, so this file has none and the unit test points
//   it at a plain array instead of the peripheral.
//
//   The register offsets and bit positions below are the same ones
//   ACAN_ESP32's accessors use; they are repeated here rather than shared
//   because ACAN_ESP32.h cannot be included from a host build.
//------------------------------------------------------------------------------

#pragma once

//------------------------------------------------------------------------------
//   Include files
//------------------------------------------------------------------------------

#include <stdint.h>

#include "ACAN_ESP32_CANMessage.h"

//------------------------------------------------------------------------------

namespace ACAN_ESP32_RxSlot {

//--- Byte offsets from the TWAI register base
static const uint32_t kCommandOffset   = 0x004 ;
static const uint32_t kStatusOffset    = 0x008 ;
static const uint32_t kFrameInfoOffset = 0x040 ;
static const uint32_t kIdOffset        = 0x044 ;
static const uint32_t kDataSFFOffset   = 0x04C ;
static const uint32_t kDataEFFOffset   = 0x054 ;

//--- SR.8 Miss Status: the controller sets it on the slot standing in for a
//    frame it could not store, and clears it on the slots that hold real ones.
//    The classic ESP32's status register stops at SR.7 and the bit is reserved
//    there, which is why the test is gated on the target rather than
//    unconditional.
static const uint32_t kStatusMiss = 1U << 8 ;

//--- CMR.2 Release Receive Buffer
static const uint32_t kReleaseBuffer = 0x04 ;

//--- FRAME_INFO bits
static const uint32_t kFrameFormatEFF = 0x80 ;
static const uint32_t kFrameRTR       = 0x40 ;

//------------------------------------------------------------------------------

enum class Outcome {
  frame,             // outFrame holds a received frame
  overrunPlaceholder // the slot stood in for a frame lost to a data overrun
} ;

//------------------------------------------------------------------------------
//   Reads the slot at the head of the receive FIFO and releases it, either way.
//
//   On silicon that reports Miss Status a slot whose SR.8 is set holds no
//   frame: the controller records the loss in place, and the frames queued
//   behind it are still good. Reading such a slot as if it held data is what
//   delivers one stale copy of the previous frame per frame dropped, followed
//   by a gap of exactly as many - so the caller must not deliver a slot this
//   reports as a placeholder, and must keep reading the ones behind it.
//------------------------------------------------------------------------------

//   Always inlined, deliberately: the only caller on the device is the
//   driver's IRAM_ATTR receive path, and a helper the compiler chose to emit as
//   its own function would land in flash - the cache-disabled hazard the ISR
//   IRAM audit exists to catch. Inlined, there is no symbol to place.
//------------------------------------------------------------------------------

inline __attribute__((always_inline))
Outcome read (volatile uint32_t * const inRegisterBase,
              const bool inHasRxStatus,
              CANMessage & outFrame) {

  const auto reg = [inRegisterBase] (const uint32_t inByteOffset) -> volatile uint32_t & {
    return inRegisterBase [inByteOffset >> 2] ;
  } ;

  if (inHasRxStatus && ((reg (kStatusOffset) & kStatusMiss) != 0)) {
    reg (kCommandOffset) = kReleaseBuffer ;
    return Outcome::overrunPlaceholder ;
  }

  const uint32_t frameInfo = reg (kFrameInfoOffset) ;

  outFrame.len = frameInfo & 0xF ;
  if (outFrame.len > 8) {
    outFrame.len = 8 ;
  }
  outFrame.rtr = (frameInfo & kFrameRTR) != 0 ;
  outFrame.ext = (frameInfo & kFrameFormatEFF) != 0 ;

  if (!outFrame.ext) { //--- Standard Frame
    outFrame.id  = uint32_t (reg (kIdOffset)) << 3 ;
    outFrame.id |= uint32_t (reg (kIdOffset + 4)) >> 5 ;

    for (uint8_t i=0 ; i<outFrame.len ; i++) {
      outFrame.data [i] = uint8_t (reg (kDataSFFOffset + 4 * i)) ;
    }
  }else{ //--- Extended Frame
    outFrame.id  = uint32_t (reg (kIdOffset))      << 21 ;
    outFrame.id |= uint32_t (reg (kIdOffset +  4)) << 13 ;
    outFrame.id |= uint32_t (reg (kIdOffset +  8)) <<  5 ;
    outFrame.id |= uint32_t (reg (kIdOffset + 12)) >>  3 ;
    for (uint8_t i=0 ; i<outFrame.len ; i++) {
      outFrame.data [i] = uint8_t (reg (kDataEFFOffset + 4 * i)) ;
    }
  }

  reg (kCommandOffset) = kReleaseBuffer ;
  return Outcome::frame ;
}

//------------------------------------------------------------------------------
//   Interrupt dispatch
//------------------------------------------------------------------------------

//--- TWAI_INT_RAW bits, as ACAN_ESP32.h names them
static const uint32_t kInterruptRx      = 0x01 ;
static const uint32_t kInterruptOverrun = 0x08 ;

//--- What one interrupt asks of the driver.
struct Dispatch {
  bool handleOverrun ; // acknowledge the overrun, and on classic silicon drain
  bool readSlot ;      // read the slot at the head of the FIFO
} ;

//------------------------------------------------------------------------------
//   The Miss-Status split changed this decision, which is why it is here and
//   not inline in the ISR.
//
//   On the classic ESP32 an overrun is answered by draining the whole FIFO,
//   and reading it in the same interrupt is the corruption the drain avoids -
//   so the read is skipped. That also costs nothing: the drain empties the
//   FIFO, so the Receive Interrupt de-asserts with it.
//
//   On Miss-Status silicon nothing is drained, and there the read has to be
//   reachable in the same interrupt. Nothing STALLS if it is not - the Receive
//   Interrupt is level-triggered on a non-empty FIFO and is the one interrupt
//   reading the interrupt register does not clear, so the handler is simply
//   re-entered and reads the slot on the second pass. What this saves is that
//   second pass: one redundant interrupt per overrun, taken with the whole
//   receive path in a critical section.
//------------------------------------------------------------------------------

inline __attribute__((always_inline))
Dispatch plan (const uint32_t inInterruptRaw, const bool inHasRxStatus) {
  const bool overrun = (inInterruptRaw & kInterruptOverrun) != 0 ;
  const bool rx = (inInterruptRaw & kInterruptRx) != 0 ;
  Dispatch dispatch ;
  dispatch.handleOverrun = overrun ;
  dispatch.readSlot = rx && (inHasRxStatus || !overrun) ;
  return dispatch ;
}

//------------------------------------------------------------------------------

} // namespace ACAN_ESP32_RxSlot

//------------------------------------------------------------------------------
