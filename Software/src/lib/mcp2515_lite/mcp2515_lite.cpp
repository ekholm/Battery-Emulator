#include "mcp2515_lite.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <esp_memory_utils.h>
#include <hal/gpio_ll.h>

#include "mcp2515_timing.h"
#include "mcp2515_tx_status.h"
#include "src/devboard/utils/logging.h"

// MCP2515 Opcodes and Registers
#define CMD_WRITE 0x02
#define CMD_READ 0x03
#define CMD_BIT_MODIFY 0x05
#define CMD_LOAD_TX_BUFFER 0x40
#define CMD_READ_RX_BUFFER 0x90
#define CMD_READ_STATUS 0xA0
#define CMD_RESET 0xC0

#define CANCTRL_REQOP_NORMAL 0x00
#define CANCTRL_REQOP_CONFIG 0x80
#define CANCTRL_REQOP_LOOPBACK 0x40

#define REG_CANCTRL 0x0F
#define REG_CANSTAT 0x0E
#define REG_CNF1 0x2A
#define REG_CNF2 0x29
#define REG_CNF3 0x28
#define REG_CANINTE 0x2B
#define REG_CANINTF 0x2C
#define REG_RXB0CTRL 0x60
#define REG_RXB1CTRL 0x70

#define STATUS_TX0IF 0x08
#define STATUS_TX1IF 0x20
#define STATUS_TX2IF 0x80

#define CANINTF_RX0IF 0x01
#define CANINTF_RX1IF 0x02
#define CANINTF_TX0IF 0x04
#define CANINTF_TX1IF 0x08
#define CANINTF_TX2IF 0x10
#define CANINTF_ERRIF 0x20

// CANINTE has CANINTF's layout: one enable per flag, same bit.
#define CANINTE_RX0IE 0x01
#define CANINTE_RX1IE 0x02
#define CANINTE_TX0IE 0x04

#define EFLG_TXBO 0x20
#define EFLG_EWARN 0x01

static inline void packExtendedId(uint8_t* buffer, uint32_t id);
static inline void packStandardId(uint8_t* buffer, uint32_t id);

MCP2515_Lite::MCP2515_Lite(SPIClass& spi, uint8_t cs, uint8_t int_pin) : _spi(spi), _cs(cs), _int_pin(int_pin) {

  // Initialize queues (this will allocate memory for storing the frames)
  _tx_queue = xQueueCreate(MCP2515_LITE_TX_QUEUE_DEPTH, sizeof(MCP2515_Lite_Frame));
  _rx_queue = xQueueCreate(MCP2515_LITE_RX_QUEUE_DEPTH, sizeof(MCP2515_Lite_Frame));
}

MCP2515_Lite::~MCP2515_Lite() {
  if (_can_task_handle) {
    vTaskDelete(_can_task_handle);
    _can_task_handle = nullptr;
  }
  if (_tx_queue) {
    vQueueDelete(_tx_queue);
    _tx_queue = nullptr;
  }
  if (_rx_queue) {
    vQueueDelete(_rx_queue);
    _rx_queue = nullptr;
  }
  detachIsrPin();
}

void MCP2515_Lite::resetIsrCounters() {
  _isr_frames = 0;
  _isr_bus_deferrals = 0;
  _isr_dropped_reported = 0;
  _isr_ring.resetDropped();
  _iram_spi.resetTimeouts();
}

void MCP2515_Lite::detachIsrPin() {
  detachInterrupt(digitalPinToInterrupt(_int_pin));
  _isr_interrupt_installed = false;
  // Removing the handler disables the pin, so a pending mask has nothing left
  // to re-arm; leaving the flag set would re-arm a pin nobody handles.
  _isr_pin_masked = false;
}

// The bit-timing arithmetic moved to mcp2515_timing.cpp, so that a
// host test can call it: it needs neither Arduino nor FreeRTOS, and being a
// file-static in this file was the only thing keeping it out of the test build.

uint32_t MCP2515_Lite::autodetectOscillatorFrequency() {
  // 7813 baud at 8MHz is 128us per bit
  if (!begin({7813, 8000000}, true, true)) {
    return 0;
  }

  // Set task handle to current task
  _can_task_handle = xTaskGetCurrentTaskHandle();

  // Send a test frame
  uint8_t cmd_frame[16];
  cmd_frame[0] = CMD_LOAD_TX_BUFFER | 0x00;  // Load TXB0
  packExtendedId(&cmd_frame[1], 0x12345678);
  cmd_frame[5] = 0x08;  // DLC
  spiTransactionBlocking(cmd_frame, nullptr, 14);

  // Set RTS to start transmission
  cmd_frame[0] = 0x81;  // RTS TXB0
  const uint32_t t1 = esp_timer_get_time() & 0xFFFFFFFF;
  spiTransactionBlocking(cmd_frame, nullptr, 1);

  // Wait for the frame to be received
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
  const uint32_t t2 = esp_timer_get_time() & 0xFFFFFFFF;

  uint32_t elapsed_us = (t2 - t1);
  DEBUG_PRINTF("MCP2515: autodetect=%uus\n", elapsed_us);

  _can_task_handle = nullptr;
  detachIsrPin();
  reset();

  return elapsed_us < 13500 ? 16000000 : 8000000;
}

bool MCP2515_Lite::begin(const MCP2515_Lite_Speed& speed, bool loopback, bool skip_task_start) {
  // 1. Set up GPIO pins (SCK/MOSI/MISO are already set up)

  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);

  pinMode(_int_pin, INPUT_PULLUP);
  /* One registration path for every caller. CONFIG_ARDUINO_ISR_IRAM=y
     * makes Arduino's GPIO service and its dispatcher IRAM-resident, so the
     * bespoke IDF-level service allocation this drain used to carry is gone:
     * ESP_INTR_FLAG_IRAM is a property of the interrupt SOURCE, and allocating
     * the service here bound every later attachInterrupt() caller to it
     * silently. What the flag cannot make safe is the callback itself - a
     * flash-resident handler on an IRAM-resident service is a cache-off fetch
     * in exactly the window the flag exists to keep serviced. That is the
     * boot-time half of the audit, below: a handler that is not IRAM-resident
     * is not registered at all, and the task drains on its notify backstop -
     * degraded but safe. The deep call chain (drainRx, the register-level SPI,
     * the ring) is audited per linked image by the notes repo's ISR IRAM audit
     * (its mcp2515 preset).
   */
  if (esp_ptr_in_iram(reinterpret_cast<const void*>(&MCP2515_Lite::mcp2515_isr_handler))) {
    attachInterruptArg(digitalPinToInterrupt(_int_pin), mcp2515_isr_handler, this, FALLING);
    _isr_interrupt_installed = _isr_drain_requested;
  } else {
    DEBUG_PRINTF("MCP2515: ISR handler is not IRAM-resident, not attaching the interrupt\n");
    _isr_interrupt_installed = false;
  }

  // 1. Reset and configure the MCP2515

  if (!reset()) {
    return false;
  }

  /* Enter config mode - and confirm it.
   *
   * This used to be a bare request, the same fire-and-forget shape that was
   * removed from the speed-change path one function down. It matters more
   * here than it looks: CNF1..3 are writable ONLY in configuration mode, so a
   * chip that did not reach CONFIG takes none of the timing that follows and
   * runs at whatever bitrate it was already using - while begin() returns
   * true and the interface reports itself up.
   */
  if (!enterMode(CANCTRL_REQOP_CONFIG)) {
    return false;
  }

  // Turn off masks/filters to receive everything into both buffers
  writeRegister(REG_RXB0CTRL, 0x64);  // enable rollover for double-buffered rx
  writeRegister(REG_RXB1CTRL, 0x60);

  /* Receive interrupts only. The mask names TX0IE because clearing
     * it is the point, not because it survives.
     *
     * Every source that can pull /INT low has to clear itself, or the level
     * trigger below never releases. READ RX BUFFER clears RXnIF on the chip
     * select's rising edge, so a receive flag is gone before the interrupt
     * returns; TXnIF and ERRIF need a register write, which only the task makes
     * - and the task is frozen for the whole of a flash write, which is the one
     * window this path exists for. Transmit liveness does not need the pin: the
     * task asks READ STATUS which buffers are free, and sendFrame() wakes it.
     */
  modifyRegister(REG_CANINTE, CANINTE_RX0IE | CANINTE_RX1IE | CANINTE_TX0IE, CANINTE_RX0IE | CANINTE_RX1IE);

  // Baudrate setup. An unreachable bitrate writes no timing registers at all,
  // which at boot is exactly as unusable as a chip that never left
  // CONFIG - so it fails init here rather than reporting an interface that is
  // silently running at the wrong speed.
  if (!applySpeedConfig(speed)) {
    return false;
  }

  // Remember the mode this driver was started in, so a later speed change can
  // put the chip back into THAT mode rather than assuming NORMAL.
  _loopback = loopback;

  // Leave config mode, and confirm the chip actually took the running mode.
  if (!enterMode(_loopback ? CANCTRL_REQOP_LOOPBACK : CANCTRL_REQOP_NORMAL)) {
    return false;
  }

  // Not for autodetection: it runs the chip in loopback at 7813 baud and only
  // wants the interrupt's timing, so a drain there would put its test frame
  // in the ring and hand it to the consumer later as if it were traffic.
  if (_isr_interrupt_installed && !skip_task_start) {
    // One more Arduino transaction, so the peripheral carries this driver's
    // settings at the moment the register-level service snapshots them.
    readRegister(REG_CANSTAT);
    if (_iram_spi.bind(_isr_spi_bus, _cs)) {
      _isr_drain_enabled = true;
      /* Level triggering, and only now.
             *
             * An edge is a one-shot: a frame that arrives while the interrupt
             * is deferring to the task pulls /INT low once, and if that fall is
             * not acted on nothing brings it back. Inside a flash window the
             * task cannot act on it, so the frames waited for the 1000 ms
             * backstop - a second of traffic, which is the loss this item
             * exists to remove. A level is not consumed by being read: it
             * states that the chip still holds frames, and it releases itself
             * when the drain reads them out.
             *
             * The interrupt has to be draining for that to hold, which is why
             * this waits for the register-level SPI to bind. Where the task
             * drains, the level would simply be re-entered until the task got
             * to run.
             */
      gpio_set_intr_type((gpio_num_t)_int_pin, GPIO_INTR_LOW_LEVEL);
    } else {
      DEBUG_PRINTF("MCP2515: no register-level SPI for bus %u, draining in the task\n", _isr_spi_bus);
    }
  }

  if (!skip_task_start) {
    // Start the background task
    xTaskCreate(canTask, "MCP2515_Lite", MCP2515_LITE_TASK_STACK_SIZE, this, MCP2515_LITE_TASK_PRIORITY,
                &_can_task_handle);
  }

  return true;
}

void MCP2515_Lite::useIsrDrain(uint8_t spi_bus) {
  _isr_drain_requested = true;
  _isr_spi_bus = spi_bus;
}

bool MCP2515_Lite::sendFrame(const MCP2515_Lite_Frame& msg) {
  if (xQueueSend(_tx_queue, &msg, 0) == pdTRUE) {
    // Notify the task that there's a new message in the queue
    if (_can_task_handle) {
      xTaskNotifyGive(_can_task_handle);
    }
    return true;
  }
  return false;
}

bool MCP2515_Lite::receiveFrame(MCP2515_Lite_Frame& msg) {
  if (_rx_overflow) {
    DEBUG_PRINTF("MCP2515 RX queue overflow!\n");
    _rx_overflow = false;
  }
  if (_isr_drain_enabled) {
    // Reported from here rather than from the drain: the drain may be
    // running with the flash cache off, where a printf is a crash.
    const uint32_t dropped = _isr_ring.dropped();
    if (dropped != _isr_dropped_reported) {
      DEBUG_PRINTF("MCP2515 ISR ring overflow, %u frames lost!\n", dropped);
      _isr_dropped_reported = dropped;
    }
    return _isr_ring.pop(msg);
  }
  // Grab a message from the RX queue if available
  return (xQueueReceive(_rx_queue, &msg, 0) == pdTRUE);
}

void MCP2515_Lite::changeSpeed(const MCP2515_Lite_Speed& new_speed) {
  _next_speed = new_speed;
  _speed_change_pending = true;
  // Wake the task to enact the speed change
  xTaskNotifyGive(_can_task_handle);
}

void MCP2515_Lite::pause(bool paused) {
  _pause_requested = paused;
  // Wake the task to apply the pause state
  xTaskNotifyGive(_can_task_handle);
}

static const SPISettings spiSettings(10000000, MSBFIRST, SPI_MODE0);

void MCP2515_Lite::spiTransactionBlocking(const uint8_t* tx_data, uint8_t* rx_data, size_t length) {
  // This should send as a single transaction, yielding to FreeRTOS and
  // returning after completion.

  busAcquireTask();
  _spi.beginTransaction(spiSettings);
  digitalWrite(_cs, LOW);
  _spi.transferBytes(tx_data, rx_data, length);
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
  busReleaseTask();
}

/* Bus ownership between this task and the interrupt.
*
* Both sides announce themselves before looking at the other, which is what
* makes the pair exclusive without a lock: whichever announces second sees the
* first. The task is the only side that ever waits, and it waits at most one
* 14-byte transfer; the interrupt gives up instead, because the case it must
* survive - a flash write - is exactly the case where the task holding the bus
* is frozen and would never hand it back in time.
*/
void MCP2515_Lite::busAcquireTask() {
  if (_task_bus_depth++ > 0) {
    return;
  }
  _task_wants_bus = true;
  __sync_synchronize();
  while (_isr_owns_bus) {}
}

void MCP2515_Lite::busReleaseTask() {
  if (--_task_bus_depth > 0) {
    return;
  }
  __sync_synchronize();
  _task_wants_bus = false;
  /* Give the pin back to an interrupt that had to defer to this transaction
     *. /INT is still low, so enabling it re-enters the interrupt at
     * once and the frames leave the chip - which is what makes the level
     * trigger a retry rather than a lost edge, and why the task needs no drain
     * of its own.
     *
     * The flag is cleared BEFORE the pin is enabled: the interrupt that fires
     * the instant it comes back may have to mask again, and it must be able to
     * leave the flag set behind us.
     */
  if (_isr_pin_masked) {
    _isr_pin_masked = false;
    gpio_ll_intr_enable_on_core(&GPIO, _isr_pin_core, _int_pin);
  }
}

/* Hold the interrupt off until the task next releases the SPI bus.
 *
 * The pin is level triggered, so an interrupt that returns without having
 * drained returns to a pin that is still low, and is entered again immediately
 * - a storm that would starve the very task it is waiting for. Masking turns
 * "I could not drain" into "drain again the moment the reason is gone": every
 * task transaction ends in busReleaseTask(), the handler wakes the task after
 * masking, and the task takes the bus at least once per
 * MCP2515_LITE_POLL_TIMEOUT_MS even with nothing else to do - except while
 * paused, when it makes no transactions at all. A paused chip is in config
 * mode and receives nothing, so a mask then only delays frames the chip
 * already holds until the unpause transaction re-arms the pin.
 *
 * gpio_ll_intr_disable() is an always_inline register write, so this is legal
 * with the flash cache off. The core is read rather than assumed: the mask is
 * per core, and this runs on whichever one the GPIO interrupt was allocated on.
 */
void IRAM_ATTR MCP2515_Lite::maskIsrPin() {
  _isr_pin_core = xPortGetCoreID();
  gpio_ll_intr_disable(&GPIO, _int_pin);
  _isr_pin_masked = true;
}

bool IRAM_ATTR MCP2515_Lite::busTryAcquireIsr() {
  if (_task_wants_bus) {
    return false;
  }
  _isr_owns_bus = true;
  __sync_synchronize();
  if (_task_wants_bus) {
    // The task announced itself while we were announcing ourselves. It is
    // waiting on us, so back off rather than deadlock the pair.
    _isr_owns_bus = false;
    return false;
  }
  return true;
}

void IRAM_ATTR MCP2515_Lite::busReleaseIsr() {
  __sync_synchronize();
  _isr_owns_bus = false;
}

/* Read every frame the chip is holding into the ring.
*
* Runs in the interrupt and nowhere else - the task's copy is gone - so
* the ring has exactly one producer for as long as the drain is live.
*
* Returns false when it gave up on a transfer that never completed. The caller
* needs to know because the receive flags are then still set and the pin is
* still low: with the level trigger, returning without either draining or
* masking is an interrupt that fires again immediately and forever.
*
* Everything reachable from here must be resident when the flash cache is off:
* the register-level SPI service, the ring and the decode are all headers or
* IRAM_ATTR for that reason, and nothing here logs.
*/
bool IRAM_ATTR MCP2515_Lite::drainRx() {
  uint8_t cmd_frame[MCP2515_RXB_LENGTH + 1];
  uint8_t rx_frame[MCP2515_RXB_LENGTH + 1];
  MCP2515_Lite_Frame can_frame;

  for (uint32_t pass = 0; pass < MCP2515_LITE_ISR_DRAIN_PASSES; pass++) {
    cmd_frame[0] = CMD_READ;
    cmd_frame[1] = REG_CANINTF;
    cmd_frame[2] = 0x00;
    /* A transfer that did not complete leaves rx_frame holding whatever was
    * there, and acting on it is worse than not draining: a
    * fabricated CANINTF claims receive flags that are not set, and the
    * buffer reads below then publish frames that were never on the wire.
         * Give up instead, and say so: the chip still holds the frames and the
         * pin is still low, so the caller masks and the next interrupt after
         * the task's transaction tries again.
    */
    if (!_iram_spi.transfer(cmd_frame, rx_frame, 3)) {
      return false;
    }
    const uint8_t intf = rx_frame[2];

    if ((intf & (CANINTF_RX0IF | CANINTF_RX1IF)) == 0) {
      // Nothing received. Transmit completions and errors are the task's
      // work, and the interrupt notifies it for those. They no longer
      // reach the pin, so this is the chip's own "I am done".
      return true;
    }

    for (uint8_t buffer = 0; buffer < 2; buffer++) {
      if ((intf & (1 << buffer)) == 0) {
        continue;
      }
      // READ RX BUFFER clears RXnIF in hardware when chip select is
      // released, so the drain needs no follow-up write to clear it.
      cmd_frame[0] = CMD_READ_RX_BUFFER | (buffer * 4);
      if (!_iram_spi.transfer(cmd_frame, rx_frame, MCP2515_RXB_LENGTH + 1)) {
        return false;
      }
      mcp2515_decode_rx_buffer(&rx_frame[1], can_frame);
      if (_isr_ring.push(can_frame)) {
        _isr_frames = _isr_frames + 1;
      }
    }
  }
  /* The pass bound was reached with frames still arriving. Every pass read a
     * buffer out, so this is progress rather than a stuck flag, and the pin is
     * low because the chip has more - which the next interrupt takes.
     */
  return true;
}

void MCP2515_Lite::canTask(void* pvParameters) {
  MCP2515_Lite* self = static_cast<MCP2515_Lite*>(pvParameters);
  MCP2515_Lite_Frame can_frame;

  // Reusable buffers for SPI payloads
  uint8_t cmd_frame[16];
  uint8_t rx_frame[16];

  while (true) {
    // Sleep the task until ISR or `sendFrame` wakes us up. We also wake
    // after a timeout just in case we've missed an interrupt and there's
    // something pending to do - and sooner while the interrupt drains,
    // because then this poll is the only backstop left for everything that
    // is not receive.
    const uint32_t poll_timeout_ms =
        self->_isr_drain_enabled ? MCP2515_LITE_ISR_DRAIN_POLL_TIMEOUT_MS : MCP2515_LITE_POLL_TIMEOUT_MS;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_timeout_ms));

    // 1. Pause/unpause if requested

    if (self->_paused && !self->_pause_requested) {
      // Enter normal mode to resume communication
      self->modifyRegister(REG_CANCTRL, 0xE0, CANCTRL_REQOP_NORMAL);
      self->_paused = false;
    } else if (!self->_paused && self->_pause_requested) {
      // Enter configuration mode to pause
      self->modifyRegister(REG_CANCTRL, 0xE0, CANCTRL_REQOP_CONFIG);
      self->_paused = true;
      continue;
    } else if (self->_paused) {
      // We're paused, skip the rest
      continue;
    }

    // Keep looping doing RX/TX until there's no more work to do (the notify
    // interrupt is edge triggered so won't retrigger until we clear all
    // pending work).
    bool work_done;
    do {
      work_done = false;

      // 2. Read the status register to see which interrupts are active

      /* Which transmit buffers are free is the chip's answer, not a flag
             * history: READ STATUS carries all three TXREQ bits, and
             * asking each pass is what a driver that no longer sees transmit
             * interrupts has instead of a shadow that nothing refreshes.
             */
      cmd_frame[0] = CMD_READ_STATUS;
      self->spiTransactionBlocking(cmd_frame, rx_frame, 2);
      uint8_t tx_free_mask = mcp2515_tx_free_mask(rx_frame[1]);

      cmd_frame[0] = CMD_READ;
      cmd_frame[1] = REG_CANINTF;
      self->spiTransactionBlocking(cmd_frame, rx_frame, 4);
      const uint8_t intf = rx_frame[2];
      const uint8_t eflag = rx_frame[3];

      // 3. Process any RX interrupts sequentially (clears receive flags)

      // While the interrupt drains, receive is entirely its business and the ring
      // is the queue - two producers would race on the ring's head, and one of
      // them cannot be locked out.
      if (!self->_isr_drain_enabled) {
        for (int i = 0; i < 2; i++) {
          // Is there a frame in this slot to read?
          if (intf & (1 << i)) {
            cmd_frame[0] = CMD_READ_RX_BUFFER | (i * 4);  // 0x90 (RXB0) or 0x94 (RXB1)

            // Write 1 command byte + 13 payload read bytes
            self->spiTransactionBlocking(cmd_frame, rx_frame, 14);

            mcp2515_decode_rx_buffer(&rx_frame[1], can_frame);

            if (xQueueSend(self->_rx_queue, &can_frame, 0) != pdTRUE) {
              self->_rx_overflow = true;
            }
            work_done = true;
          }
        }
      }

      // 4. Check for errors, and clear the interrupt flags if needed

      // The transmit flags are acknowledged, not acted on: a completion
      // says a frame left, while TXREQ above says the buffer can be
      // loaded again, and only the second question is being asked here.
      int int_clear_mask = intf & (CANINTF_TX0IF | CANINTF_TX1IF | CANINTF_TX2IF);

      if (intf & CANINTF_ERRIF) {  // ERRIF
        if (eflag & (EFLG_TXBO | EFLG_EWARN)) {
          self->_errors = true;
        }

        int_clear_mask |= CANINTF_ERRIF;
      }

      if (int_clear_mask) {
        self->modifyRegister(REG_CANINTF, int_clear_mask, 0x00);
      }

      // 5. Perform a speed change if requested

      if (self->_speed_change_pending) {
        if (tx_free_mask != MCP2515_TX_ALL_FREE) {
          // There's still something being sent. If we're at the wrong
          // speed, it probably won't ever send, so wait long enough
          // to give a chance and then change speed anyway.
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Every step is now asked whether it worked. This used
        // to be three writes and no question: the caller is long gone
        // by the time the task runs, changeSpeed() returns void, and
        // nothing read the chip back - so an interface left at the old
        // bitrate, or in CONFIG mode and therefore off the bus
        // entirely, looked exactly like a speed change that worked.
        // The verdict is picked up by speedChangeFailed().
        bool changed = self->enterMode(CANCTRL_REQOP_CONFIG);
        changed = self->applySpeedConfig(self->_next_speed) && changed;
        // Back to the mode begin() started in, not an assumed NORMAL: a
        // driver opened in LOOPBACK used to become live on the bus at
        // the first speed change, silently.
        changed = self->enterMode(self->_loopback ? CANCTRL_REQOP_LOOPBACK : CANCTRL_REQOP_NORMAL) && changed;
        /* One verdict outstanding at a time, and it is the LATEST one.
         * Two independent read-and-clear latches are not two
         * pieces of information: they are one, spread over two bools,
         * and a caller that reads them in priority order can act on the
         * older of the pair. Concretely, with a change that SUCCEEDED
         * and a later one that FAILED both landing between two polls,
         * the caller shuts the gate on the failure, clears only that
         * latch, and then reopens the interface on the success left
         * behind - putting a chip at an unknown bitrate back on the bus,
         * which is the one outcome the gate exists to prevent.
         *
         * Setting each verdict therefore retires the other. That makes
         * "last verdict wins" a property of the driver rather than of
         * the caller's polling discipline, which is the same guarantee a
         * single tri-state field would give.
         */
        if (!changed) {
          self->_speed_change_succeeded = false;
          self->_speed_change_failed = true;
        } else {
          // Success is reported too, so a caller that took the
          // interface out of service on a failure can put it back.
          self->_speed_change_failed = false;
          self->_speed_change_succeeded = true;
        }
        self->_speed_change_pending = false;
      }

      // 6. Transmit any pending messages (if we have free buffers)

      uint8_t rts_mask = 0;
      if (tx_free_mask > 0) {
        // We have free tx buffers, do we have anything to send?
        for (int i = 0; i < MCP2515_TX_BUFFERS; i++) {
          if ((tx_free_mask & (1 << i)) && xQueueReceive(self->_tx_queue, &can_frame, 0)) {
            // This slot is no longer free
            tx_free_mask &= ~(1 << i);
            // Mark this slot as ready-to-send
            rts_mask |= (1 << i);

            cmd_frame[0] = CMD_LOAD_TX_BUFFER | (i * 2);  // 0x40 (TXB0), 0x42 (TXB1), or 0x44 (TXB2)

            if (can_frame.ext) {
              packExtendedId(&cmd_frame[1], can_frame.id);
            } else {
              packStandardId(&cmd_frame[1], can_frame.id);
            }
            // Limit to 8 bytes (in case someone tries to send a FD frame)
            uint8_t payload_len = can_frame.dlc > 8 ? 8 : can_frame.dlc;
            cmd_frame[5] = payload_len;
            memcpy(&cmd_frame[6], can_frame.data, payload_len);

            self->spiTransactionBlocking(cmd_frame, nullptr, 6 + payload_len);
          }
        }
      }

      // 7. Trigger sending if required

      if (rts_mask > 0) {
        cmd_frame[0] = 0x80 | rts_mask;
        self->spiTransactionBlocking(cmd_frame, nullptr, 1);
        work_done = true;
      }
    } while (work_done);

    /* Nothing re-checks the pin here any more. It is level
         * triggered, so a frame that arrived while the interrupt was deferring
         * is still holding /INT low, and the transactions above have each
         * ended in busReleaseTask() - which re-arms the pin and lets the
         * interrupt collect it. Doing it from this task was the weak version
         * of exactly that: task context is frozen for the whole flash window,
         * which is when the frames need collecting.
         */
  }
}

// SPI helper functions

void MCP2515_Lite::writeRegister(uint8_t reg, uint8_t value) {
  modifyRegister(reg, 0xFF, value);
}

uint8_t MCP2515_Lite::readRegister(uint8_t reg) {
  uint8_t cmd[] = {CMD_READ, reg, 0x00};
  uint8_t rx[3] = {0};
  spiTransactionBlocking(cmd, rx, 3);
  return rx[2];
}

void MCP2515_Lite::modifyRegister(uint8_t reg, uint8_t mask, uint8_t data) {
  uint8_t cmd[] = {CMD_BIT_MODIFY, reg, mask, data};
  spiTransactionBlocking(cmd, nullptr, 4);
}

bool MCP2515_Lite::reset() {
  const uint8_t cmd[] = {CMD_RESET};
  spiTransactionBlocking(cmd, nullptr, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  uint8_t CANSTAT = readRegister(REG_CANSTAT);
  if (CANSTAT != 0x80) {
    DEBUG_PRINTF("MCP2515 reset failed, CANSTAT=0x%02X\n", CANSTAT);
    return false;
  }
  return true;
}

// Returns false when no usable timing could be computed, in which case NOTHING
// is written and the chip keeps the timing it had. Two things now produce that,
// where previously only the first did:
//
//  - a degenerate argument, in practice an oscillator frequency of zero, which
//    is what autodetectOscillatorFrequency() returns when its probe begin()
//    fails and what comm_can.cpp stores unconditionally;
//  - a bitrate this oscillator cannot get within
//    MCP2515_TIMING_TOLERANCE_PERMILLE of.
//
// The second used to return TRUE. The prescaler was clamped, the least-wrong of
// the two TQ layouts was picked, and the chip was configured for whatever came
// out: 1000 kbit/s asked of an 8 MHz part yielded 500, 800 of 8 MHz yielded 500,
// 200 of 8 MHz yielded 166, 800 of 16 MHz yielded 1000 - every one of them
// reported as a successful init, and all four rates are selectable from the
// CAN_Speed enum. Review found it; the earlier comment had claimed the opposite.
bool MCP2515_Lite::applySpeedConfig(const MCP2515_Lite_Speed& speed) {
  uint8_t cnf[3];
  uint32_t achieved = 0;
  if (!mcp2515_calculate_timing(speed.f_osc, speed.bitrate, cnf, &achieved)) {
    DEBUG_PRINTF("MCP2515 has no timing for %u bit/s from a %u Hz oscillator (closest %u)\n", (unsigned)speed.bitrate,
                 (unsigned)speed.f_osc, (unsigned)achieved);
    return false;
  }
  writeRegister(REG_CNF1, cnf[0]);
  writeRegister(REG_CNF2, cnf[1]);
  writeRegister(REG_CNF3, cnf[2]);
  return true;
}

// Request an operating mode and confirm the chip actually took it.
//
// A bounded poll rather than one read: the MCP2515 finishes the transmission in
// progress before a mode change takes effect, so an immediate readback can be
// a false alarm - and a false alarm here would report a working interface as
// broken, which is a worse defect than the silence this closes (the lesson
// the native path's own mutation testing taught).
bool MCP2515_Lite::enterMode(uint8_t reqop) {
  modifyRegister(REG_CANCTRL, 0xE0, reqop);

  for (int attempt = 0; attempt < MCP2515_LITE_MODE_CHANGE_ATTEMPTS; attempt++) {
    if ((readRegister(REG_CANSTAT) & 0xE0) == reqop) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(MCP2515_LITE_MODE_CHANGE_POLL_MS));
  }

  DEBUG_PRINTF("MCP2515 refused mode 0x%02X, CANSTAT=0x%02X\n", reqop, readRegister(REG_CANSTAT));
  return false;
}

/* ISR called when MCP2515 signals an interrupt via the /INT pin.
 *
 * With the drain enabled the received frames leave the chip here, before the
 * task is involved at all - that is what survives a flash write, during which
 * every task on both cores is frozen but an IRAM interrupt still runs. It no
 * longer re-checks the pin either, because the pin is level triggered and holds
 * the request itself.
 *
 * And it is no longer woken on every frame. Once CANINTE is receive-only this
 * interrupt fires for exactly one reason, and the drain finishes that reason
 * before returning - the consumer takes the frames from the ring, not from the
 * task. Waking it anyway would make it read two registers over the SPI bus for
 * nothing, and that bus hold is what makes the NEXT interrupt defer: an RX
 * interrupt scheduling the task's TX work is what turns a quiet drain into the
 * contention the handover exists to survive. The task's own work arrives with
 * its own wakes - sendFrame(), changeSpeed(), pause() - and the poll timeout is
 * the backstop for errors, which never reached the pin in the first place.
 *
 * vTaskNotifyGiveFromISR() is itself IRAM-resident in this build
 * (CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is off), which is what makes it
 * legal to call from here at all.
 */
void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg) {
  MCP2515_Lite* instance = static_cast<MCP2515_Lite*>(arg);
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  // Without the drain the task has all of the work: the frames are still in
  // the chip and only it can read them out.
  bool wake_task = true;

  if (instance->_isr_drain_enabled) {
    wake_task = false;
    if (instance->busTryAcquireIsr()) {
      const bool drained = instance->drainRx();
      instance->busReleaseIsr();
      if (!drained) {
        instance->maskIsrPin();
        // The pin is off until a task transaction re-arms it, so the
        // task is the one thing that can undo this.
        wake_task = true;
      }
    } else {
      instance->_isr_bus_deferrals = instance->_isr_bus_deferrals + 1;
      instance->maskIsrPin();
      wake_task = true;
    }
  }

  // Notify task that there's an interrupt to handle
  if (wake_task && instance->_can_task_handle) {
    vTaskNotifyGiveFromISR(instance->_can_task_handle, &xHigherPriorityTaskWoken);
  }

  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// Utility functions

static inline void packExtendedId(uint8_t* buffer, uint32_t id) {
  buffer[0] = id >> 21;
  buffer[1] = (((id >> 13) & 0xE0) | 0x08 | ((id >> 16) & 0x03));
  buffer[2] = id >> 8;
  buffer[3] = id;
}

static inline void packStandardId(uint8_t* buffer, uint32_t id) {
  buffer[0] = id >> 3;
  buffer[1] = (id & 0x07) << 5;
}
