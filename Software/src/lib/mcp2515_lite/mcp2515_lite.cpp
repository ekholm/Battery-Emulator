#include "mcp2515_lite.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <hal/gpio_ll.h>

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

void MCP2515_Lite::detachIsrPin() {
  if (_isr_interrupt_installed) {
    gpio_isr_handler_remove((gpio_num_t)_int_pin);
    _isr_interrupt_installed = false;
  } else {
    detachInterrupt(digitalPinToInterrupt(_int_pin));
  }
  // Removing the handler disables the pin, so a pending mask has nothing left
  // to re-arm; leaving the flag set would re-arm a pin nobody handles.
  _isr_pin_masked = false;
}

static bool calculateMCP2515Config(uint32_t f_osc, uint32_t can_rate, uint8_t* cnf) {
  if (!cnf || can_rate == 0 || f_osc == 0) {
    return false;
  }

  // Calculate for TQ = 16 (will fail for 500kbit@8MHz)
  uint32_t div16 = 32 * can_rate;
  uint32_t brp16 = (f_osc + (div16 / 2)) / div16;  // Integer rounding
  if (brp16 < 1) {
    brp16 = 1;
  } else if (brp16 > 64) {
    brp16 = 64;
  }
  uint32_t rate16 = f_osc / (32 * brp16);
  uint32_t err16 = (rate16 > can_rate) ? (rate16 - can_rate) : (can_rate - rate16);

  // Calculate for TQ = 8 (lower resolution)
  uint32_t div8 = 16 * can_rate;
  uint32_t brp8 = (f_osc + (div8 / 2)) / div8;  // Integer rounding
  if (brp8 < 1) {
    brp8 = 1;
  } else if (brp8 > 64) {
    brp8 = 64;
  }
  uint32_t rate8 = f_osc / (16 * brp8);
  uint32_t err8 = (rate8 > can_rate) ? (rate8 - can_rate) : (can_rate - rate8);

  if (err8 < err16) {
    // TQ=8 has lower error, use that
    cnf[0] = (uint8_t)(brp8 - 1);
    cnf[1] = 0x8A;  // BTLMODE=1, SAM=0, PHSEG1=1, PRSEG=2
    cnf[2] = 0x01;  // PHSEG2=1
  } else {
    // otherwise use TQ=16
    cnf[0] = (uint8_t)(brp16 - 1);
    cnf[1] = 0xA5;  // BTLMODE=1, SAM=0, PHSEG1=4, PRSEG=5
    cnf[2] = 0x03;  // PHSEG2=3
  }

  return true;
}

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
  /* Autodetection calls begin() first, and it must take the SAME path: the
   * GPIO interrupt service is installed once for the whole system, so an
   * attachInterrupt() here would install it WITHOUT ESP_INTR_FLAG_IRAM and
   * the real begin() would then find it installed and decline the drain
   * for good. On the boards where MCP2515_FREQ() is 0 - the devkit and the
   * 3LB - that is every boot.
   */
  _isr_interrupt_installed = _isr_drain_requested && installIsrDrainInterrupt();
  if (!_isr_interrupt_installed) {
    attachInterruptArg(digitalPinToInterrupt(_int_pin), mcp2515_isr_handler, this, FALLING);
  }

  // 1. Reset and configure the MCP2515

  if (!reset()) {
    return false;
  }

  // Enter config mpde
  modifyRegister(REG_CANCTRL, 0xE0, CANCTRL_REQOP_CONFIG);

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

  // Baudrate setup
  applySpeedConfig(speed);

  // Leave config mode and enter normal mode
  modifyRegister(REG_CANCTRL, 0xE0, loopback ? CANCTRL_REQOP_LOOPBACK : CANCTRL_REQOP_NORMAL);

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

/* Arduino's attachInterrupt() installs the GPIO interrupt service without
 * ESP_INTR_FLAG_IRAM (CONFIG_ARDUINO_ISR_IRAM is off in this build), and a
 * service allocated that way is masked for the whole of a flash write - which
 * is the one window the drain exists to keep working through. So install the
 * service here with the flag instead, and register through the IDF rather than
 * through Arduino's dispatcher, which is itself flash-resident.
 */
bool MCP2515_Lite::installIsrDrainInterrupt() {
  if (!_isr_service_owned) {
    const esp_err_t installed = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (installed == ESP_ERR_INVALID_STATE) {
      // Someone installed the service first and its allocation flags are
      // not ours to know. Claiming the drain would claim a window it may
      // not have.
      DEBUG_PRINTF("MCP2515: GPIO interrupt service already installed, draining in the task\n");
      return false;
    }
    if (installed != ESP_OK) {
      DEBUG_PRINTF("MCP2515: GPIO interrupt service install failed (0x%x)\n", installed);
      return false;
    }
    // The service is a singleton: begin() runs twice when the oscillator is
    // autodetected, and the second install would report it already there.
    _isr_service_owned = true;
  }
  /* An edge until the drain is actually live: autodetection installs the
     * interrupt too, and binding the register-level SPI can still fail, and a
     * level nobody drains is a level nobody clears. begin() switches the pin to
     * GPIO_INTR_LOW_LEVEL at the point where there is a drain behind it.
     */
  if (gpio_set_intr_type((gpio_num_t)_int_pin, GPIO_INTR_NEGEDGE) != ESP_OK) {
    return false;
  }
  if (gpio_isr_handler_add((gpio_num_t)_int_pin, mcp2515_isr_handler, this) != ESP_OK) {
    return false;
  }
  return true;
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
 * task transaction ends in busReleaseTask(), and the task takes the bus at
 * least once per MCP2515_LITE_POLL_TIMEOUT_MS even with nothing else to do.
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
    // something pending to do.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MCP2515_LITE_POLL_TIMEOUT_MS));

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
        changed = self->enterMode(CANCTRL_REQOP_NORMAL) && changed;
        if (!changed) {
          self->_speed_change_failed = true;
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

// Returns false when no timing could be computed at all, in which case NOTHING
// is written and the chip keeps the timing it had. On the live path that means
// one thing: an oscillator frequency of zero, which is what
// autodetectOscillatorFrequency() returns when its probe begin() fails and what
// comm_can.cpp stores unconditionally.
//
// It does NOT mean "this bitrate is unreachable from this oscillator"; a later
// review corrected this comment for claiming it did. calculateMCP2515Config() rejects
// only degenerate arguments (null buffer, zero rate, zero oscillator); for a
// rate the oscillator cannot produce it clamps the prescaler, picks whichever
// of its two TQ layouts is least wrong, and returns true. Measured with its own
// body: 1000 kbit/s asked of an 8 MHz part yields 500 kbit/s, 800 asked of 8 MHz
// yields 500, 1000 of 20 MHz yields 1250, 500 of 12 MHz yields 375 - every one
// of them reported as success. That silent failure is real, it is in the enum's
// range (CAN_SPEED_800KBPS and CAN_SPEED_1000KBPS both exist) and it is still
// open: the remaining gap is tracked separately.
bool MCP2515_Lite::applySpeedConfig(const MCP2515_Lite_Speed& speed) {
  uint8_t cnf[3];
  if (!calculateMCP2515Config(speed.f_osc, speed.bitrate, cnf)) {
    DEBUG_PRINTF("MCP2515 has no timing for %u bit/s from a %u Hz oscillator\n", (unsigned)speed.bitrate,
                 (unsigned)speed.f_osc);
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
 * every task on both cores is frozen but an IRAM interrupt still runs. The task
 * is still notified, because transmit completions, errors and speed changes are
 * its work; it no longer re-checks the pin, because the pin is level triggered
 * and holds the request itself.
 *
 * vTaskNotifyGiveFromISR() is itself IRAM-resident in this build
 * (CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is off), which is what makes it
 * legal to call from here at all.
 */
void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg) {
  MCP2515_Lite* instance = static_cast<MCP2515_Lite*>(arg);
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  if (instance->_isr_drain_enabled) {
    if (instance->busTryAcquireIsr()) {
      const bool drained = instance->drainRx();
      instance->busReleaseIsr();
      if (!drained) {
        instance->maskIsrPin();
      }
    } else {
      instance->_isr_bus_deferrals = instance->_isr_bus_deferrals + 1;
      instance->maskIsrPin();
    }
  }

  // Notify task that there's an interrupt to handle
  if (instance->_can_task_handle) {
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
