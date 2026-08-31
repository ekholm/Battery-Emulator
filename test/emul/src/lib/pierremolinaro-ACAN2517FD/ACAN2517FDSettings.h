#pragma once

#include <stdint.h>

#include "ACANFD_DataBitRateFactor.h"

// Emulated stand-in for the vendored ACAN2517FDSettings. The real class computes
// bit timings in its constructor; none of that is observable off-chip, so the
// emulation only stores what comm_can.cpp sets. The enumerators are copied from
// the real header, values included - hal.h hands MCP2517_CLKODIV() to mCLKOPin
// as a raw integer, so the ordinals are part of the contract.
class ACAN2517FDSettings {
 public:
  typedef enum : uint8_t {
    OSC_AUTODETECT,
    OSC_4MHz,
    OSC_4MHz_DIVIDED_BY_2,
    OSC_4MHz10xPLL,
    OSC_4MHz10xPLL_DIVIDED_BY_2,
    OSC_20MHz,
    OSC_20MHz_DIVIDED_BY_2,
    OSC_40MHz,
    OSC_40MHz_DIVIDED_BY_2
  } Oscillator;

  typedef enum : uint8_t { CLKO_DIVIDED_BY_1, CLKO_DIVIDED_BY_2, CLKO_DIVIDED_BY_4, CLKO_DIVIDED_BY_10, SOF } CLKOpin;

  typedef enum : uint8_t {
    NormalFD = 0,
    Sleep = 1,
    InternalLoopBack = 2,
    ListenOnly = 3,
    Configuration = 4,
    ExternalLoopBack = 5,
    Normal20B = 6,
    RestrictedOperation = 7
  } OperationMode;

  ACAN2517FDSettings(const Oscillator inOscillator, const uint32_t inBitRate, const DataBitRateFactor inFactor)
      : mOscillator(inOscillator), mArbitrationBitRate(inBitRate), mDataBitRateFactor(inFactor) {}

  Oscillator mOscillator;
  uint32_t mArbitrationBitRate;
  DataBitRateFactor mDataBitRateFactor;
  CLKOpin mCLKOPin = CLKO_DIVIDED_BY_10;
  OperationMode mRequestedMode = NormalFD;
};
