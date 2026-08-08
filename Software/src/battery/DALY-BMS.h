#ifndef DALY_BMS_H
#define DALY_BMS_H

#include "../datalayer/datalayer.h"
#include "RS485Battery.h"

class DalyBms : public RS485Battery {
 public:
  DalyBms() {
    datalayer_battery = &datalayer.batteries[0];
    allows_contactor_closing = &datalayer.system.status.battery_link[0].allows_contactor_closing;
  }
  void setup();
  void update_values();
  void transmit_rs485(unsigned long currentMillis);
  void receive();
  static constexpr const char* Name = "DALY RS485";

 private:
  // Writes this instance's parsed values; was a free function mutating module globals.
  void decode_packet(uint8_t command, uint8_t data[8]);
  // Parsed pack values, the RS485 command cursor and the receive buffer. These
  // were file-scope and function-local statics, so a second DalyBms - or a
  // second test case - inherited the previous one's half-assembled frame and
  // stale readings. recv_buff is the sharp one: a partial frame left behind
  // corrupts the next header check.
  uint32_t lastPacket = 0;
  int16_t temperature_min_dC = 0;
  int16_t temperature_max_dC = 0;
  int16_t current_dA = 0;
  uint16_t voltage_dV = 0;
  uint32_t remaining_capacity_mAh = 0;
  uint16_t cellvoltages_mV[48] = {0};
  uint16_t cellvoltage_min_mV = 3700;
  uint16_t cellvoltage_max_mV = 3700;
  uint16_t cell_count = 0;
  uint16_t SOC = 0;
  bool has_fault = false;
  uint8_t nextCommand = 0x90;
  uint8_t recv_buff[13] = {0};
  uint8_t recv_len = 0;
  int baud_rate() { return 9600; }
  bool* allows_contactor_closing;
};

#endif
