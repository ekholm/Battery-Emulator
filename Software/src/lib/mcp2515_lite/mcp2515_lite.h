#pragma once

#include <SPI.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

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

  // True once if the last speed change the task enacted did not take: the
  // bitrate was unreachable from this oscillator, or the chip did not report
  // the mode that was asked for. Consumed on read, like hasErrors(), so the
  // caller that reads it owns reporting it. Previously no status existed
  // anywhere in this chain - the failure was unreportable rather than merely
  // unreported.
  inline bool speedChangeFailed() {
    auto ret = _speed_change_failed;
    _speed_change_failed = false;
    return ret;
  }

  // True once if the last speed change the task enacted DID take. The mirror
  // of speedChangeFailed(), and it exists so a caller that takes the interface
  // out of service on a failure has something to bring it back with.
  // Without it the gate is a one-way door: the caller stops polling the very
  // path the verdict arrives on, and a later good change is never seen.
  //
  // The pair is mutually exclusive by construction: enacting a change
  // retires the opposite verdict, so at most one of these two can be true and
  // it is always the most recent answer. A caller may therefore read them in
  // any order without acting on a stale one.
  inline bool speedChangeSucceeded() {
    auto ret = _speed_change_succeeded;
    _speed_change_succeeded = false;
    return ret;
  }

  // Non-blocking: pauses all communication (and stops acknowledging messages)
  void pause(bool paused);

  inline bool hasErrors() {
    auto ret = _errors;
    _errors = false;
    return ret;
  }

 private:
  SPIClass& _spi;
  uint8_t _cs;
  uint8_t _int_pin;

  QueueHandle_t _tx_queue;
  QueueHandle_t _rx_queue;

  MCP2515_Lite_Speed _next_speed;
  volatile bool _speed_change_pending = false;
  volatile bool _speed_change_failed = false;
  volatile bool _speed_change_succeeded = false;
  // The mode begin() put the chip in, remembered so a later speed change
  // returns it to THAT mode. Without this a driver started in LOOPBACK came
  // back in NORMAL and a test session silently became live on the bus.
  bool _loopback = false;
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
};
