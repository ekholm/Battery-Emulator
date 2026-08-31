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
  bool receive(CANMessage& outMessage);
  bool tryToSend(const CANMessage& inMessage);
  uint32_t statusRegister() const;

  static ACAN_ESP32 can;
};
