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
  int baud_rate() { return 9600; }
  bool* allows_contactor_closing;
};

#endif
