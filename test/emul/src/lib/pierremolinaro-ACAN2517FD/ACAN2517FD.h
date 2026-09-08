#pragma once

// Mirrors the define the vendored header makes unconditionally at
// ACAN2517FD.h:26. Nothing in this stub uses it - there are no
// interrupt-mask wrappers here to switch off - but this header is what the
// host build puts in front of the vendored one, and comm_can.cpp's CAN-FD
// teardown is written against that define being set. Dropping the mirror
// hides the substitution from anything that checks the assumption holds.
#define DISABLEMCP2517FDCOMPAT

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
