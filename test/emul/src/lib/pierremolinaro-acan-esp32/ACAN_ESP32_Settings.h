#pragma once

#include <stdint.h>

#include "soc/gpio_num.h"

// Emulated stand-in for the vendored ACAN_ESP32_Settings. The real constructor
// solves for the bit timings and reports how close it got; the emulation answers
// with a nominally exact configuration so init_CAN()'s log block has something
// coherent to print.
class ACAN_ESP32_Settings {
 public:
  typedef enum : uint8_t { NormalMode, ListenOnlyMode, LoopBackMode } CANMode;

  explicit ACAN_ESP32_Settings(const uint32_t inDesiredBitRate, const uint32_t inTolerancePPM = 1000)
      : mDesiredBitRate(inDesiredBitRate) {}

  uint32_t actualBitRate() const { return mDesiredBitRate; }
  bool exactBitRate() const { return true; }
  uint32_t samplePointFromBitStart() const { return 75; }

  uint32_t mDesiredBitRate;
  CANMode mRequestedCANMode = NormalMode;
  gpio_num_t mTxPin = GPIO_NUM_NC;
  gpio_num_t mRxPin = GPIO_NUM_NC;
  uint16_t mBitRatePrescaler = 4;
  uint8_t mTimeSegment1 = 15;
  uint8_t mTimeSegment2 = 4;
  uint8_t mRJW = 3;
  bool mTripleSampling = false;
  // Read by begin() and answered back by driverReceiveBufferSize(), as the real
  // driver sizes its ring from it. The default is the library's own.
  uint16_t mDriverReceiveBufferSize = 32;
};
