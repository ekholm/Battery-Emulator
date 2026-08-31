#pragma once

#include <SPI.h>
#include <stdint.h>

#include "ACAN2517FDSettings.h"
#include "CANFDMessage.h"
#include "CANMessage.h"

// Emulated stand-in for the vendored ACAN2517FD driver. Both FD add-ons are
// instances of this class; emul_can tells them apart by construction order.
class ACAN2517FD {
 public:
  ACAN2517FD(const uint8_t inCS, SPIClass& inSPI, const uint8_t inINT);
  ~ACAN2517FD();

  uint32_t begin(const ACAN2517FDSettings& inSettings, void (*inInterruptServiceRoutine)(void));
  bool end();

  bool tryToSend(const CANFDMessage& inMessage);
  bool receive(CANFDMessage& outMessage);
  bool available();
  bool hasCanErrors();

  void poll();
  void isr();

 private:
  int chip_;
  uint8_t cs_;
  uint8_t int_;
};
