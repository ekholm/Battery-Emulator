#pragma once

// Emulated stand-in for the vendored CANMessage (classic CAN, 8 data bytes).
// Both vendored CAN libraries ship their own copy behind this same guard, so
// whichever is included first wins - exactly as on target.
#ifndef GENERIC_CAN_MESSAGE_DEFINED
#define GENERIC_CAN_MESSAGE_DEFINED

#include <stdint.h>

class CANMessage {
 public:
  uint32_t id = 0;
  bool ext = false;
  bool rtr = false;
  uint8_t idx = 0;
  uint8_t len = 0;
  union {
    uint64_t data64;
    uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  };
};

#endif  // GENERIC_CAN_MESSAGE_DEFINED
