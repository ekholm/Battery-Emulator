#ifndef HARDWARESERIAL_H
#define HARDWARESERIAL_H

#include <stdint.h>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include "Print.h"
#include "Stream.h"

enum SerialConfig {
  SERIAL_5N1 = 0x8000010,
  SERIAL_6N1 = 0x8000014,
  SERIAL_7N1 = 0x8000018,
  SERIAL_8N1 = 0x800001c,
  SERIAL_5N2 = 0x8000030,
  SERIAL_6N2 = 0x8000034,
  SERIAL_7N2 = 0x8000038,
  SERIAL_8N2 = 0x800003c,
  SERIAL_5E1 = 0x8000012,
  SERIAL_6E1 = 0x8000016,
  SERIAL_7E1 = 0x800001a,
  SERIAL_8E1 = 0x800001e,
  SERIAL_5E2 = 0x8000032,
  SERIAL_6E2 = 0x8000036,
  SERIAL_7E2 = 0x800003a,
  SERIAL_8E2 = 0x800003e,
  SERIAL_5O1 = 0x8000013,
  SERIAL_6O1 = 0x8000017,
  SERIAL_7O1 = 0x800001b,
  SERIAL_8O1 = 0x800001f,
  SERIAL_5O2 = 0x8000033,
  SERIAL_6O2 = 0x8000037,
  SERIAL_7O2 = 0x800003b,
  SERIAL_8O2 = 0x800003f
};

class HardwareSerial : public Stream {
 public:
  /* An RX queue a test can fill, so a driver's real receive() can be driven
   * with real bytes instead of being reached around. Empty by default, which is
   * what every existing test sees: available() answers 0 and read() answers -1
   * exactly as before. Anything a test injects it must also clear - the Serial
   * objects are globals shared by the whole suite. */
  void inject_rx(std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) {
      rx_queue.push_back(b);
    }
  }
  void clear_rx() { rx_queue.clear(); }

  int available() override { return static_cast<int>(rx_queue.size()); }
  int read() override {
    if (rx_queue.empty()) {
      return -1;
    }
    const int out = rx_queue.front();
    rx_queue.pop_front();
    return out;
  }
  int peek() override { return rx_queue.empty() ? -1 : rx_queue.front(); }
  void flush() override {}                      // Implement flush from Print
  size_t write(uint8_t) override { return 0; }  // Implement write from Print

  // Your existing methods
  uint32_t baudRate() { return 9600; }
  void begin(unsigned long baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1, int8_t txPin = -1,
             bool invert = false, unsigned long timeout_ms = 20000UL, uint8_t rxfifo_full_thrhd = 120) {}
  void setTxBufferSize(uint16_t size) {}
  void setRxBufferSize(uint16_t size) {}
  bool setRxFIFOFull(uint8_t fifoBytes) { return false; }

  // Add the buffer write method
  size_t write(const uint8_t* buffer, size_t size) override {
    (void)buffer;
    (void)size;
    return 0;
  }

 private:
  std::deque<uint8_t> rx_queue;
};
extern HardwareSerial Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;

#endif
