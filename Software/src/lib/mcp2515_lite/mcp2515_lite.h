#pragma once

#include <SPI.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mcp2515_iram_spi.h"
#include "mcp2515_rx_ring.h"

/* MCP2515_Lite: Minimal MCP2515 library for Arduino+FreeRTOS.

Features:
 - Non-blocking send/receive APIs using preallocated FreeRTOS queues
 - High-priority background task performing blocking SPI transactions
 - Hardware triple-buffered transmit and double-buffered receive
 - Pause/resume functionality (suspends rx/tx and ACKs)
 - On-the-fly CAN bus speed changes
 - As few SPI transactions as possible for minimum overhead

*/

// A high priority will avoid drops during message bursts
#define MCP2515_LITE_TASK_PRIORITY 10
// Minimal stack size (+1500 if you want to do any printf debugging!)
#define MCP2515_LITE_TASK_STACK_SIZE 1100
// Queue depths (in messages)
#define MCP2515_LITE_TX_QUEUE_DEPTH 25
#define MCP2515_LITE_RX_QUEUE_DEPTH 25
// Poll this often if there are no interrupts
#define MCP2515_LITE_POLL_TIMEOUT_MS 1000
// How long to give the chip to actually adopt a requested operating mode: it
// finishes the transmission in progress first, so a mode change is not
// instantaneous. 5 x 2 ms covers a full frame at the slowest bitrate this
// driver is used at, with room to spare.
#define MCP2515_LITE_MODE_CHANGE_ATTEMPTS 5
#define MCP2515_LITE_MODE_CHANGE_POLL_MS 2

/* And this often while the interrupt drains.
 *
 * The interrupt then answers receive entirely, and does not wake the task for
 * it - so this poll is the ONLY backstop left for everything else: an ERRIF the
 * chip cannot raise on a receive-only pin, a transmit buffer that freed with
 * nothing sending to notice, and the remote case of a pin the interrupt masked
 * with no task transaction following to re-arm it. At 1000 ms each of those is
 * a second of blindness on a driver whose whole point is not losing a
 * millisecond. Each wake costs two SPI transactions, and ten of them a second
 * is about three percent of the per-frame wake this replaced - a bus hold small
 * enough not to reintroduce the contention that wake was removed for.
 */
#define MCP2515_LITE_ISR_DRAIN_POLL_TIMEOUT_MS 100
// Frames the interrupt drain can hold while no task is running to take them.
// A flash erase parks every task for tens of milliseconds, so the depth is what
// decides whether frames survive one; 64 covers 128 ms at 500 frames/s and
// costs 1 KB of DRAM.
#define MCP2515_LITE_ISR_RING_DEPTH 64
// How many times one interrupt re-reads CANINTF before handing back. The chip
// holds two receive buffers, so a healthy drain finishes in one pass; the bound
// is what stops a stuck flag from turning the interrupt into a spin.
#define MCP2515_LITE_ISR_DRAIN_PASSES 4

// This has the same layout as CAN_frame (with only 8 data bytes)
typedef struct {
  union {
    bool fd;
    uint8_t flags;
  };
  bool ext;
  uint8_t dlc;
  uint32_t id;
  uint8_t data[8];
} MCP2515_Lite_Frame;

typedef struct {
  uint32_t bitrate;
  uint32_t f_osc;
} MCP2515_Lite_Speed;

class MCP2515_Lite {
 public:
  // Requires an initialized SPIClass (e.g. SPI or SPI2) passed by reference
  MCP2515_Lite(SPIClass& spi, uint8_t cs, uint8_t int_pin);
  ~MCP2515_Lite();

  uint32_t autodetectOscillatorFrequency();

  bool begin(const MCP2515_Lite_Speed& speed, bool loopback = false, bool skip_task_start = false);

  // Non-blocking: pushes message to the TX queue
  bool sendFrame(const MCP2515_Lite_Frame& msg);

  // Non-blocking: pops message from the RX queue
  bool receiveFrame(MCP2515_Lite_Frame& msg);

  // Non-blocking: signals the task to change CAN bus speed. The task enacts
  // it later and verifies it; ask speedChangeFailed() for the verdict, since
  // by then this caller is long gone.
  void changeSpeed(const MCP2515_Lite_Speed& new_speed);

  /* Ask for received frames to be drained inside the interrupt rather than by
   * the driver task.
   *
   * Only the caller can know this is safe: the drain owns the SPI bus at
   * moments no task can be asked about, so the chip must be the only device
   * on `spi_bus`. Call before begin(); begin() decides whether it can be
   * honoured and isrDrainActive() reports what actually happened.
   */
  void useIsrDrain(uint8_t spi_bus);
  inline bool isrDrainActive() const { return _isr_drain_enabled; }

  // Interrupt-drain counters, all zero while the drain is inactive.
  inline uint32_t isrFramesDrained() const { return _isr_frames; }
  inline uint32_t isrFramesDropped() const { return _isr_ring.dropped(); }
  inline uint32_t isrBusDeferrals() const { return _isr_bus_deferrals; }
  inline uint32_t isrBusTimeouts() const { return _iram_spi.timeouts(); }

  // True once if the last speed change the task enacted did not take: the
  // bitrate was unreachable from this oscillator, or the chip did not report
  // the mode that was asked for. Consumed on read, like hasErrors(), so the
  // caller that reads it owns reporting it. Until this change no status existed
  // anywhere in this chain - the failure was unreportable rather than merely
  // unreported.
  inline bool speedChangeFailed() {
    auto ret = _speed_change_failed;
    _speed_change_failed = false;
    return ret;
  }

  // Non-blocking: pauses all communication (and stops acknowledging messages)
  void pause(bool paused);

  inline bool hasErrors() {
    auto ret = _errors;
    _errors = false;
    return ret;
  }

  // Interrupt drain. Requested by useIsrDrain(), enabled by begin()
  // only once the register-level SPI is bound and the interrupt is installed
  // with ESP_INTR_FLAG_IRAM.
  Mcp2515IramSpi _iram_spi;
  Mcp2515RxRing<MCP2515_Lite_Frame, MCP2515_LITE_ISR_RING_DEPTH> _isr_ring;
  bool _isr_drain_requested = false;
  uint8_t _isr_spi_bus = 0;
  bool _isr_interrupt_installed = false;
  bool _isr_service_owned = false;
  volatile bool _isr_drain_enabled = false;
  volatile uint32_t _isr_frames = 0;
  volatile uint32_t _isr_bus_deferrals = 0;
  uint32_t _isr_dropped_reported = 0;

  // Who owns the SPI bus. Two flags rather than a lock: a lock taken in the
  // interrupt while a task holds it would spin for the whole of the flash
  // window this drain exists to keep working through. Only the task ever
  // waits.
  volatile bool _task_wants_bus = false;
  volatile bool _isr_owns_bus = false;
  uint32_t _task_bus_depth = 0;

  // Set when the interrupt had to leave frames on the chip and masked its own
  // pin to avoid re-entering on the level it did not clear. The task
  // clears it when it releases the bus, which is the moment the reason is
  // gone. The core is the one the interrupt runs on; the mask is per core.
  volatile bool _isr_pin_masked = false;
  volatile uint32_t _isr_pin_core = 0;

  // Background task for handling sequential blocking transfers

 private:
  SPIClass& _spi;
  uint8_t _cs;
  uint8_t _int_pin;

  QueueHandle_t _tx_queue;
  QueueHandle_t _rx_queue;

  MCP2515_Lite_Speed _next_speed;
  volatile bool _speed_change_pending = false;
  volatile bool _speed_change_failed = false;
  volatile bool _pause_requested = false;
  volatile bool _paused = false;
  volatile bool _rx_overflow = false;
  volatile bool _errors = false;

  // Background task for handling sequential blocking transfers
  TaskHandle_t _can_task_handle = nullptr;
  static void canTask(void* pvParameters);

  // Internal SPI helpers
  void spiTransactionBlocking(const uint8_t* tx_data, uint8_t* rx_data, size_t length);
  void writeRegister(uint8_t reg, uint8_t value);
  uint8_t readRegister(uint8_t reg);
  void modifyRegister(uint8_t reg, uint8_t mask, uint8_t data);

  bool reset();
  bool applySpeedConfig(const MCP2515_Lite_Speed& speed);
  bool enterMode(uint8_t reqop);

  // ISR handler for the CAN interrupt pin
  static void IRAM_ATTR mcp2515_isr_handler(void* arg);

  // SPI bus ownership between the driver task and the interrupt. IRAM_ATTR
  // is on the definitions rather than here: the attribute names a section per
  // occurrence, and repeating it on the declaration makes the two disagree.
  void busAcquireTask();
  void busReleaseTask();
  bool busTryAcquireIsr();
  void busReleaseIsr();

  // Mask the level-triggered interrupt pin from inside the interrupt, when it
  // could not drain and would otherwise be re-entered on the same level for
  // as long as the reason lasts. busReleaseTask() re-arms it.
  void maskIsrPin();

  /* Read every frame the chip is holding into the ring. Runs in the interrupt
     * and nowhere else, so the ring has one producer.
     *
     * False means it gave up on a transfer that never completed and the chip is
     * still holding frames - the caller must mask the pin, or the level it did
     * not clear brings it straight back.
     */
  bool drainRx();

  // Install the interrupt with ESP_INTR_FLAG_IRAM. Returns false if the GPIO
  // interrupt service is already installed by someone else, since then its
  // allocation flags are not ours to know.
  bool installIsrDrainInterrupt();

  // ISR handler for the CAN interrupt pin

  // Undo whichever of the two interrupt paths begin() took.
  void detachIsrPin();
};
