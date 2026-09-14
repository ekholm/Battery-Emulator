#pragma once

#include <stdint.h>

#include "../pierremolinaro-ACAN2517FD/CANMessage.h"
#include "ACAN_ESP32_Settings.h"

// TWAI status-register bits comm_can.cpp tests after a receive. Values are the
// real ones from the ESP32 technical reference manual (TWAI_STATUS_REG).
#define TWAI_ERR_ST (1 << 2)
#define TWAI_BUS_OFF_ST (1 << 7)

// Emulated stand-in for the vendored ACAN_ESP32 driver. The real class reads and
// writes TWAI registers from inline methods, which is why it cannot be reused
// here: statusRegister() alone dereferences a hardware address.
class ACAN_ESP32 {
 public:
  uint32_t begin(const ACAN_ESP32_Settings& inSettings);
  void end();

  bool available() const;
  // The size of the driver's receive ring. receive_frame_can_native() uses it
  // as the bound on one drain pass, so a stub answering 0 would drain nothing
  // and every native receive test would pass on an empty loop. It answers what
  // the last begin() was configured with, as the real driver does, so the depth
  // comm_can.cpp sets is the depth the drain runs with.
  uint16_t driverReceiveBufferSize() const;
  bool receive(CANMessage& outMessage);
  bool tryToSend(const CANMessage& inMessage);
  uint32_t statusRegister() const;

  static ACAN_ESP32 can;
};
