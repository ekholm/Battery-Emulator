#pragma once

#include <SPI.h>
#include <stdint.h>

// Emulated stand-in for the vendored MCP2515_Lite driver. The frame layout is
// the real one and must stay that way: communication/can/utils.h static_asserts
// it against CAN_frame and memcpys between the two.
typedef struct {
  union {
    bool fd;
    uint8_t flags;
  };
  bool ext;
  uint8_t dlc;
  uint32_t id;
  uint8_t data[8];
} MCP2515_Lite_Frame;

typedef struct {
  uint32_t bitrate;
  uint32_t f_osc;
} MCP2515_Lite_Speed;

class MCP2515_Lite {
 public:
  MCP2515_Lite(SPIClass& spi, uint8_t cs, uint8_t int_pin);
  ~MCP2515_Lite();

  uint32_t autodetectOscillatorFrequency();

  bool begin(const MCP2515_Lite_Speed& speed, bool loopback = false, bool skip_task_start = false);

  bool sendFrame(const MCP2515_Lite_Frame& msg);
  bool receiveFrame(MCP2515_Lite_Frame& msg);

  // Asks for the interrupt drain on a bus. The real driver only records the
  // request here - begin() decides whether it can be honoured - and on the host
  // there is no interrupt to install, so isrDrainActive() is always false and
  // frames arrive the way every other emulated chip delivers them.
  void useIsrDrain(uint8_t spi_bus);
  bool isrDrainActive() const;

  void changeSpeed(const MCP2515_Lite_Speed& new_speed);
  // Consumed on read, like hasErrors(): a speed change that did not take is
  // reported once. The real driver applies the change from its task and
  // verifies it afterwards; with no task here, changeSpeed() applies it inline
  // and a chip set to fail (emul_can::set_begin_error) fails it.
  bool speedChangeFailed();
  void pause(bool paused);

  bool hasErrors();

 private:
  SPIClass& _spi;
  uint8_t _cs;
  uint8_t _int_pin;
};
