#pragma once

#include "CANMessage.h"

// Emulated stand-in for the vendored CANFDMessage. Only the fields the firmware
// reads or writes are modelled; data[] keeps the real 64-byte length because
// comm_can.cpp bounds its memcpy with sizeof() on it.
class CANFDMessage {
 public:
  typedef enum : uint8_t { CAN_REMOTE, CAN_DATA, CANFD_NO_BIT_RATE_SWITCH, CANFD_WITH_BIT_RATE_SWITCH } Type;

  uint32_t id = 0;
  bool ext = false;
  Type type = CANFD_WITH_BIT_RATE_SWITCH;
  uint8_t idx = 0;
  uint8_t len = 0;
  uint8_t data[64] = {};
};
