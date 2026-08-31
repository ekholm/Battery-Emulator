#pragma once

#include <stdint.h>

// The real Arduino SPI.h pulls this in, and the CAN drivers rely on it.
#include <Arduino.h>

// Arbitrary bus indices
#define HSPI 1
#define VSPI 2
#define FSPI 3

// Enough of Arduino's SPIClass for the CAN add-on drivers to be constructed and
// handed a bus. Nothing here talks to hardware: the emulated MCP2515/MCP2518FD
// drivers (emul/can_drivers.cpp) never issue a transaction, they only need a
// reference to hold.
class SPIClass {
 public:
  explicit SPIClass(uint8_t bus = VSPI) : bus_(bus) {}

  void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1) {
    sck_ = sck;
    miso_ = miso;
    mosi_ = mosi;
    ss_ = ss;
    begun_ = true;
  }

  void end() { begun_ = false; }

  uint8_t bus() const { return bus_; }
  bool begun() const { return begun_; }

 private:
  uint8_t bus_;
  int8_t sck_ = -1;
  int8_t miso_ = -1;
  int8_t mosi_ = -1;
  int8_t ss_ = -1;
  bool begun_ = false;
};
