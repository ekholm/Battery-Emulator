#include "parallel_safety.h"
#include "../../battery/BATTERIES.h"
#include "../../datalayer/datalayer.h"
#include "../utils/events.h"

void check_parallel_battery_safety(uint8_t batteryNumber) {
  /* Before the checks are started, we need to know the battery is alive via CAN, and that the voltages have ben read*/
  if ((batteryNumber == 2) && battery2_detected) {
    if (datalayer.battery2.status.voltage_dV == 0 || datalayer.battery2.status.CAN_battery_still_alive == 0) {
      // Pack 2 reads 0 V or has gone silent: it is not on the DC link, so it must not stay counted as joined
      // (a pack left joined keeps its SOC in the total the inverter sees).
      datalayer.system.status.battery2_allowed_contactor_closing = false;
      return;
    }
    if (datalayer.battery.status.voltage_dV == 0) {
      return;  // The main pack is not decoded yet: nothing to compare pack 2 against
    }
    uint16_t voltage_diff_battery2_towards_main =
        abs(datalayer.battery.status.voltage_dV - datalayer.battery2.status.voltage_dV);
    static uint8_t secondsOutOfVoltageSyncBattery2 = 0;

    if (voltage_diff_battery2_towards_main <= 15) {  // If we are within 1.5V between the batteries
      clear_event(EVENT_VOLTAGE_DIFFERENCE_BAT2);
      secondsOutOfVoltageSyncBattery2 = 0;
      if (datalayer.system.status.system_status == FAULT) {
        // If main battery is in fault state, disengage the second battery
        datalayer.system.status.battery2_allowed_contactor_closing = false;
      } else {  // If main battery is OK, allow second battery to join
        datalayer.system.status.battery2_allowed_contactor_closing = true;
      }
    } else {  //Voltage between the two packs is too large
      //If we start to drift out of sync between the two packs for more than 10 seconds, open contactors
      //We alert user if we have been out of sync for more than 3 seconds, but we allow 10 seconds before we disengage the second battery
      if (secondsOutOfVoltageSyncBattery2 < 10) {
        secondsOutOfVoltageSyncBattery2++;
        if (secondsOutOfVoltageSyncBattery2 > 3) {
          set_event(EVENT_VOLTAGE_DIFFERENCE_BAT2, (uint8_t)(voltage_diff_battery2_towards_main / 10));
        }
      } else {  //10 seconds out of sync, disengage the second battery
        datalayer.system.status.battery2_allowed_contactor_closing = false;
      }
    }
  }

  if ((batteryNumber == 3) && battery3_detected) {
    if (datalayer.battery3.status.voltage_dV == 0 || datalayer.battery3.status.CAN_battery_still_alive == 0) {
      // Pack 3 reads 0 V or has gone silent: it is not on the DC link, so it must not stay counted as joined
      // (a pack left joined keeps its SOC in the total the inverter sees).
      datalayer.system.status.battery3_allowed_contactor_closing = false;
      return;
    }
    if (datalayer.battery.status.voltage_dV == 0) {
      return;  // The main pack is not decoded yet: nothing to compare pack 3 against
    }
    uint16_t voltage_diff_battery3_towards_main =
        abs(datalayer.battery.status.voltage_dV - datalayer.battery3.status.voltage_dV);
    static uint8_t secondsOutOfVoltageSyncBattery3 = 0;

    if (voltage_diff_battery3_towards_main <= 15) {  // If we are within 1.5V between the batteries
      clear_event(EVENT_VOLTAGE_DIFFERENCE_BAT3);
      secondsOutOfVoltageSyncBattery3 = 0;
      if (datalayer.system.status.system_status == FAULT) {
        // If main battery is in fault state, disengage the second battery
        datalayer.system.status.battery3_allowed_contactor_closing = false;
      } else {  // If main battery is OK, allow second battery to join
        datalayer.system.status.battery3_allowed_contactor_closing = true;
      }
    } else {  //Voltage between the two packs is too large
      //If we start to drift out of sync between the two packs for more than 10 seconds, open contactors
      //We alert user if we have been out of sync for more than 3 seconds, but we allow 10 seconds before we disengage the second battery
      if (secondsOutOfVoltageSyncBattery3 < 10) {
        secondsOutOfVoltageSyncBattery3++;
        if (secondsOutOfVoltageSyncBattery3 > 3) {
          set_event(EVENT_VOLTAGE_DIFFERENCE_BAT3, (uint8_t)(voltage_diff_battery3_towards_main / 10));
        }
      } else {  //10 seconds out of sync, disengage the second battery
        datalayer.system.status.battery3_allowed_contactor_closing = false;
      }
    }
  }
}
