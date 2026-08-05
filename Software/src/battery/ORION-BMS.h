#ifndef ORION_BMS_H
#define ORION_BMS_H

#include "../datalayer/datalayer.h"
#include "../system_settings.h"
#include "CanBattery.h"

class OrionBms : public CanBattery {
 public:
  OrionBms(DATALAYER_BATTERY_TYPE* datalayer_ptr = &datalayer.batteries[0],
           CAN_Interface targetCan = can_config.batteries[0])
      : CanBattery(targetCan) {
    datalayer_battery = datalayer_ptr;
    const bool primary = datalayer_ptr == &datalayer.batteries[0];
    allows_contactor_closing =
        &datalayer.system.status.battery_link[datalayer_battery_instance(datalayer_ptr)].allows_contactor_closing;
  }

  virtual void setup(void);
  virtual void handle_incoming_can_frame(CAN_frame rx_frame);
  virtual void update_values();
  virtual void transmit_can(unsigned long currentMillis);
  static constexpr const char* Name = "DIY battery with Orion BMS (Victron setting)";

 private:
  bool* allows_contactor_closing;

  uint16_t Maximum_Cell_Voltage = 3700;
  uint16_t Minimum_Cell_Voltage = 3700;
  uint16_t Pack_Health = 99;
  uint16_t Pack_Summed_Voltage = 0;
  uint16_t High_Temperature = 0;
  uint16_t Pack_SOC_ppt = 0;
  uint16_t Pack_CCL = 0;  //Charge current limit (A)
  uint16_t Pack_DCL = 0;  //Discharge current limit (A)
  uint16_t Maximum_Pack_Voltage = 0;
  uint16_t Minimum_Pack_Voltage = 0;
  uint16_t CellVoltage = 0;
  uint16_t CellResistance = 0;
  uint16_t CellOpenVoltage = 0;
  uint16_t Checksum = 0;

  int16_t Average_Current = 0;

  uint8_t amount_of_detected_cells = 0;
  uint8_t CellID = 0;

  bool CellBalancing = false;

  uint16_t cellvoltages[MAX_AMOUNT_CELLS];  //array with all the cellvoltages
};

#endif
