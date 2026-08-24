#include "MG-5-BATTERY.h"
#include <Arduino.h>
#include <cmath>    //For unit test
#include <cstring>  //For unit test
#include "../communication/can/comm_can.h"
#include "../datalayer/datalayer.h"
#include "../devboard/utils/events.h"
#include "../devboard/utils/logging.h"

#ifndef SMALL_FLASH_DEVICE

/* TODO: 
- Get contactor closing working
- Figure out which CAN messages need to be sent towards the battery to keep it alive
- Map all values from battery CAN messages
- Most important ones 
*/

inline const char* getBMStatus(int index) {
  switch (index) {
    case 0:
      return "INVALID";
    case 1:
      return "DISCONNECTED";
    case 2:
      return "PRECHARGE";
    case 3:
      return "CONNECTED";
    //case 6:
    //  return "AC CHARGING";
    //case 7:
    // return "DC CHARGING";
    case 15:
      return "ISOLATION FAULT";
    default:
      return "UNKNOWN";
  }
}

void Mg5Battery::update_soc(uint16_t soc_times_ten) {

#if MG5_USE_FULL_CAPACITY
  // The SoC hits 100% at 4.1V/cell. To get the full 4.2V/cell we need to use
  // voltage instead for the last bit.

  if (cellVoltageValidTime == 0) {
    // We don't have a recent cell max voltage reading, so can't do
    // voltage-based SoC.
  } else if (soc_times_ten > 900 && datalayer.battery.status.cell_max_voltage_mV < 4000) {
    // Something is wrong with our max cell voltage reading (it is too low), so
    // don't trust it - we'll just let the SoC hit 100%.
  } else if (soc_times_ten == 1000 && datalayer.battery.status.cell_max_voltage_mV >= 4100) {
    // We've hit 100%, so use voltage-based-SoC calculation for the last bit.

    // We usually hit 92% at ~369V, and the pack max is 378V.

    // Scale so that 100% becomes 92%
    soc_times_ten = (uint16_t)(((uint32_t)soc_times_ten * 9200) / 10000);

    // Add on the last 100mV as the last 8% of SoC.
    soc_times_ten += (uint16_t)((((uint32_t)datalayer.battery.status.cell_max_voltage_mV - 4100) * 800) / 1000);
    if (soc_times_ten > 1000) {
      soc_times_ten = 1000;  // Don't let it go above 100%
    }
  } else {
    // Scale so that 100% becomes 92%
    soc_times_ten = (uint16_t)(((uint32_t)soc_times_ten * 9200) / 10000);
  }
#endif

  // Set the state of charge in the datalayer
  datalayer.battery.status.real_soc = soc_times_ten * 10;

  RealSoC = datalayer.battery.status.real_soc / 100;

  // Calculate the remaining capacity.
  tempfloat = datalayer.battery.info.total_capacity_Wh * (RealSoC) / 100;
  if (tempfloat > 0) {
    datalayer.battery.status.remaining_capacity_Wh = tempfloat;
  } else {
    datalayer.battery.status.remaining_capacity_Wh = 0;
  }

  //#if MG5_USE_FULL_CAPACITY

  // Calculate the maximum charge power. Taper the charge power between 90% and 100% SoC, as 100% SoC is approached
  if (RealSoC < StartChargeTaper) {
    datalayer.battery.status.max_charge_power_W = MaxChargePower;
  } else if (RealSoC >= 100) {
    datalayer.battery.status.max_charge_power_W = TricklePower;
  } else {
    //Taper the charge to the Trickle value. The shape and start point of the taper is set by the constants
    datalayer.battery.status.max_charge_power_W =
        (MaxChargePower * pow(((100 - RealSoC) / (100 - StartChargeTaper)), ChargeTaperExponent)) + TricklePower;
  }

  // Calculate the maximum discharge power. Taper the discharge power between 10% and Min% SoC, as Min% SoC is approached
  if (RealSoC > StartDischargeTaper) {
    datalayer.battery.status.max_discharge_power_W = MaxDischargePower;
  } else if (RealSoC < MinSoC) {
    datalayer.battery.status.max_discharge_power_W = TricklePower;
  } else {
    //Taper the charge to the Trickle value. The shape and start point of the taper is set by the constants
    datalayer.battery.status.max_discharge_power_W =
        (MaxDischargePower * pow(((RealSoC - MinSoC) / (StartDischargeTaper - MinSoC)), DischargeTaperExponent)) +
        TricklePower;
  }

  //#endif
}

void Mg5Battery::
    update_values() {  //This function maps all the values fetched via CAN to the correct parameters used for modbus

  //all data are already update when it is received via CAN

  //reduce timout valeue for cell voltage timeout, reduce by 1 every second
  if (cellVoltageValidTime > 0) {
    cellVoltageValidTime--;
  }

  // The BMS only answers diagnostics inside the extended session. When the
  // diag side has been quiet for a while, (re-)enter it - the pre-superclass
  // code sent 10 03 on every transaction timeout, this is the same reflex on
  // the superclass's scan. start_sequence only queues; it runs once the
  // in-flight request resolves, and refuses (false) if a sequence is already
  // queued - then we simply try again next second.
  if ((millis() - last_uds_activity_ms) > SESSION_RETRY_MS) {
    if (start_sequence(MG5_STATE_SESSION_START)) {
      last_uds_activity_ms = millis();
    }
  }

  // Contactor closing wants the stored DTCs erased (DTC 293 blocks closing);
  // run the superclass erase as soon as the diag side is free to take it.
  if (dtc_clear_wanted && !uds_is_busy()) {
    dtc_clear_wanted = false;
    reset_DTC();
  }
}

void Mg5Battery::handle_incoming_can_frame(CAN_frame rx_frame) {
  // We start polling with UDS ID 0x7DF, the generic broadcast one. Our first
  // reply will indicate what the BMS-specific one is, which we switch to (the
  // superclass then also pins the response address).
  if (uds_address == 0x7DF && rx_frame.ID == 0x789) {
    logging.println("MG5: Detected UDS address 0x781");
    setup_uds(0x781, 0x789);
  } else if (uds_address == 0x7DF && rx_frame.ID == 0x7ED) {
    logging.println("MG5: Detected UDS address 0x7E5");
    setup_uds(0x7E5, 0x7ED);
  }

  uint32_t v, i;

  switch (rx_frame.ID) {
    case 0x297: {                                                          //BMS state
      datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;  // Let system know battery is sending CAN

      // Contains battery status in rx_frame.data.u8[1]
      // Presumed mapping:
      // 1 = disconnected
      // 2 = precharge
      // 3 = connected
      // 15 = isolation fault
      // 0/8 = checking

      if (rx_frame.data.u8[1] != previousState) {
        logging.print("MG5: Battery status changed to: ");
        logging.println(getBMStatus(rx_frame.data.u8[1]));
      }

      if (rx_frame.data.u8[1] == 0xf && previousState != 0xf) {
        // Isolation fault, set event
        set_event(EVENT_BATTERY_ISOLATION, rx_frame.data.u8[0], battery_index);
      } else if (rx_frame.data.u8[1] != 0xf && previousState == 0xf) {
        // Isolation fault has cleared, clear event
        clear_event(EVENT_BATTERY_ISOLATION, battery_index);
      }

      if (rx_frame.data.u8[1] == 0x03 && previousState != 0x03) {
        datalayer.system.status.battery_allows_contactor_closing = true;  //signal to the UI that contactors are closed
      } else {
        datalayer.system.status.battery_allows_contactor_closing = false;
      }

      previousState = rx_frame.data.u8[1];
      break;
    }
    case 0x172:
      break;
    case 0x173:
      // Contains cell min/max voltages
      v = (rx_frame.data.u8[4] << 8) | rx_frame.data.u8[5];
      if (v > 0 && v < 0x2000) {
        datalayer.battery.status.cell_max_voltage_mV = v;
        cellVoltageValidTime = CELL_VOLTAGE_TIMEOUT;
      }
      v = (rx_frame.data.u8[6] << 8) | rx_frame.data.u8[7];
      if (v > 0 && v < 0x2000) {
        datalayer.battery.status.cell_min_voltage_mV = v;
      }
      break;
    case 0x293:
      break;
    case 0x295:
      break;
    case 0x29B:
      break;
    case 0x29C:
      break;
    case 0x2A0:
      break;
    case 0x2A2:

      if (rx_frame.data.u8[0] < 0xfe) {
        // Max cell temp
        datalayer.battery.status.temperature_max_dC = ((rx_frame.data.u8[0] << 8) / 50) - 400;
      }
      if (rx_frame.data.u8[5] < 0xfe) {
        // Min cell temp
        datalayer.battery.status.temperature_min_dC = ((rx_frame.data.u8[5] << 8) / 50) - 400;
      }
      break;
    case 0x322:
      break;
    case 0x334:
      break;
    case 0x33F:
      break;
    case 0x391:
      break;
    case 0x393:
      break;
    case 0x3AB:
      break;
    case 0x3AC:  // battery summary: SoC, HV voltage, HV current

      // Contains SoCs, voltage, current. Is emitted by both PTCAN and HybridCAN, but
      // There does not seem to be 2 SOC"s present like in MG HS battery, so we just read the meassages from the PTCAN bus

      if ((((rx_frame.data.u8[4] & 0x0F) << 8) | rx_frame.data.u8[5]) != 0) {
        // 3AC message contains a nonzero voltage (so must have come from PTCAN)

        // Battery voltage
        v = (((rx_frame.data.u8[4] & 0x0F) << 8) | rx_frame.data.u8[5]);
        // Current
        i = (rx_frame.data.u8[6] << 8 | rx_frame.data.u8[7]);

        if (v > 0 && v < 2400 && i > 16000 && i < 24000) {
          // 3AC message contains a credible voltage and current (so must have come from PTCAN)
          // (voltage between 0 and 600V, current between -200A and +200A)

          datalayer.battery.status.voltage_dV = (v * 5) / 2;
          datalayer.battery.status.current_dA = -(i - 20000) / 2;
        }

        // SOC
        soc = (rx_frame.data.u8[2] << 8) | rx_frame.data.u8[3];
        if (soc < 1022) {
          update_soc(soc);
        }
      }
      break;
    case 0x3B8:
      break;
    case 0x3BA:
      break;
    case 0x3BC:
      break;
    case 0x3BE:

      // Per-cell voltages and temps
      cell_id = rx_frame.data.u8[5];
      if (cell_id < 96) {
        v = 1000 + ((rx_frame.data.u8[2] << 8) | rx_frame.data.u8[3]);
        datalayer.battery.status.cell_voltages_mV[cell_id] = v < 10000 ? v : 0;
        // cell temperature is rx_frame.data.u8[1]-40 but BE doesn't use it
      }
      break;
    case 0x3C0:
      break;
    case 0x3C2:
      break;
    case 0x400:
      break;
    case 0x402:
      break;
    case 0x418:
      break;
    case 0x44C:
      break;
    case 0x620:
      break;
    case 0x7ED:
    case 0x789:  // response from UDS diagnostic service (ISO-TP)
      // Single/multi-frame reassembly, flow control, the PID scan match, the
      // DTC readout (which now lands on the web page instead of only in the
      // log) and the erase ack all live in the UDS superclass.
      last_uds_activity_ms = millis();
      handle_incoming_uds_can_frame(rx_frame);
      break;

    default:
      break;
  }
}

uint16_t Mg5Battery::handle_pid(uint16_t pid, uint32_t value, const uint8_t* data, uint16_t length) {
  // `data` points at the raw value bytes after the echoed DID. Of the 15
  // scanned DIDs only SoH was ever stored; the rest of the pre-superclass
  // switch was commented-out logging and is gone with it.
  last_uds_activity_ms = millis();
  switch (pid) {
    case 0xB061:  // State of health, 0.01 % units (original: (u8[4] << 8) | u8[5])
      datalayer.battery.status.soh_pptt = (uint16_t)value;
      break;
    default:
      break;
  }
  return 0;  // Continue the scan list in order.
}

void Mg5Battery::on_uds_sequence_step(uint16_t state, uint8_t sid, const uint8_t* data, uint16_t len) {
  switch (state) {
    case MG5_STATE_SESSION_START:
      // Enter the extended diagnostic session (10 03), like the hand-rolled
      // timeout path did.
      send_sequence_message(MG5_STATE_SESSION_DONE, SID::DiagnosticSessionControl, (const uint8_t*)"\x03", 1, 10, 2);
      break;
    case MG5_STATE_SESSION_DONE:
      last_uds_activity_ms = millis();
      break;
  }
}

void Mg5Battery::transmit_can(unsigned long currentMillis) {
  // UDS transmit path: the 15-DID scan, session/DTC sequences and ISO-TP
  // pacing (the hand-rolled phase-2 block below is gone).
  transmit_uds_can(currentMillis);

  static int8_t send_phase = -1;
  if (++send_phase > 3) {
    send_phase = 0;
  }

  // Send 10ms CAN Message
  if (currentMillis - previousMillis10 >= INTERVAL_10_MS && send_phase == 0) {
    previousMillis10 = currentMillis;

    if (datalayer.system.status.system_status != FAULT                // Fault, so open contactors!
        && userRequestContactorClose == true                          // User requested contactor closing
        && datalayer.system.status.inverter_allows_contactor_closing  // Inverter requests contactor closing
    ) {
      MG5_8A.data.u8[5] = 0x02;  // Command to close contactors
      //logging.println("contactor close command sent");

      if (warmupCounter < 110) {
        // Keep the 1 asserted for 110 messages
        MG5_8A.data.u8[6] = 0x10 | eightAcycle;
        warmupCounter++;
      } else {
        MG5_8A.data.u8[6] = 0x30 | eightAcycle;
      }

      if (contactorClosed == false) {
        // Just changed to closed
        contactorClosed = true;
        dtc_clear_wanted = true;  //clear DTCs to clear DTC 293, otherwise contactors won't close
        //datalayer.battery.status.max_charge_power_W = MaxChargePower; //set the power limits, as they are set to zero when contactors are open
        //datalayer.battery.status.max_discharge_power_W = MaxDischargePower;
      }
    } else {
      contactorClosed = false;
      warmupCounter = 0;
      MG5_8A.data.u8[5] = 0x00;  // Command to open contactors
      //userRequestClearDTC = true; //clear DTCs to be able to close the contactors afterwards
      //logging.println("conctactor open command sent");

      MG5_8A.data.u8[6] = 0x10 | eightAcycle;
    }

    MG5_8A.data.u8[7] = (MG5_8A.data.u8[0] ^ MG5_8A.data.u8[1] ^ MG5_8A.data.u8[2] ^ MG5_8A.data.u8[3] ^
                         MG5_8A.data.u8[4] ^ MG5_8A.data.u8[5] ^ MG5_8A.data.u8[6]);
    eightAcycle = (eightAcycle + 1) & 0xF;

    transmit_can_frame(&MG5_8A);
  }

  if (currentMillis - previousMillis20 >= INTERVAL_20_MS && send_phase == 1) {
    previousMillis20 = currentMillis;
    transmit_can_frame(&MG5_1F1);
  }

  if (currentMillis - previousMillis200 >= INTERVAL_200_MS) {
    previousMillis200 = currentMillis;
  }

  if (currentMillis - previousMillis1000 >= INTERVAL_1_S) {
    previousMillis1000 = currentMillis;
  }

  if (currentMillis - previousMillis2000 >= INTERVAL_2_S) {
    previousMillis2000 = currentMillis;
  }
}

void Mg5Battery::setup(void) {  // Performs one time setup at startup
  strncpy(datalayer.system.info.battery_protocol, Name, 63);
  datalayer.system.info.battery_protocol[63] = '\0';
  datalayer.system.status.battery_allows_contactor_closing = true;
  datalayer.battery.info.max_design_voltage_dV = MAX_PACK_VOLTAGE_DV;
  datalayer.battery.info.min_design_voltage_dV = MIN_PACK_VOLTAGE_DV;
  datalayer.battery.info.max_cell_voltage_mV = MAX_CELL_VOLTAGE_MV;
  datalayer.battery.info.min_cell_voltage_mV = MIN_CELL_VOLTAGE_MV;
  datalayer.battery.info.total_capacity_Wh = TOTAL_BATTERY_CAPACITY_WH;
  datalayer.battery.info.number_of_cells = 96;

  // BMS diagnostics: start on the generic broadcast address; the first reply
  // pins the real request/response pair (see handle_incoming_can_frame).
  setup_uds(0x7DF, 0);
  set_pid_scan_list(MG5_PID_SCAN, 15);
  // Same 2 s boot grace the hand-rolled code gave the BMS before diagnostics.
  pause_uds(20);
  last_uds_activity_ms = millis();
}

#endif
