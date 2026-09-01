#include "BMW-SBOX.h"
#include <Arduino.h>
#include "../communication/can/comm_can.h"
#include "../datalayer/datalayer.h"
#include "../devboard/utils/events.h"
#include "../devboard/utils/logging.h"

uint8_t reverse_bits(uint8_t byte) {
  uint8_t reversed = 0;
  for (int i = 0; i < 8; i++) {
    reversed = (reversed << 1) | (byte & 1);
    byte >>= 1;
  }
  return reversed;
}

/** CRC8, both inverted, poly 0x31 **/
uint8_t calculateCRC(CAN_frame CAN) {
  uint8_t crc = 0;
  for (size_t i = 0; i < CAN.DLC; i++) {
    uint8_t reversed_byte = reverse_bits(CAN.data.u8[i]);
    crc ^= reversed_byte;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x31;
      } else {
        crc <<= 1;
      }
      crc &= 0xFF;
    }
  }
  crc = reverse_bits(crc);
  return crc;
}

void BmwSbox::handle_incoming_can_frame(CAN_frame rx_frame) {
  unsigned long currentTime = millis();
  if (rx_frame.ID == 0x200) {
    ShuntLastSeen = currentTime;
    datalayer.shunt.measured_amperage_mA =
        ((rx_frame.data.u8[2] << 24) | (rx_frame.data.u8[1] << 16) | (rx_frame.data.u8[0] << 8)) / 256;
    datalayer.shunt.measured_amperage_dA = datalayer.shunt.measured_amperage_mA / 100;

    /** Calculate 1S avg current **/
    if (LastAvgTime + 100 < currentTime) {
      LastAvgTime = currentTime;
      if (k > 9) {
        k = 0;
      }
      avg_mA_array[k] = datalayer.shunt.measured_amperage_mA;
      k++;
      avg_sum = 0;
      for (uint8_t i = 0; i < 10; i++) {
        avg_sum = avg_sum + avg_mA_array[i];
      }
      datalayer.shunt.measured_avg1S_amperage_mA = avg_sum / 10;
    }
  } else if (rx_frame.ID == 0x210)  //SBOX input (battery side) voltage
  {
    ShuntLastSeen = currentTime;
    datalayer.shunt.measured_voltage_mV =
        ((rx_frame.data.u8[2] << 16) | (rx_frame.data.u8[1] << 8) | (rx_frame.data.u8[0]));
  } else if (rx_frame.ID == 0x220)  //SBOX output voltage
  {
    ShuntLastSeen = currentTime;
    datalayer.shunt.measured_outvoltage_mV =
        ((rx_frame.data.u8[2] << 16) | (rx_frame.data.u8[1] << 8) | (rx_frame.data.u8[0]));
    datalayer.shunt.available = true;
  }
}

void BmwSbox::transmit_can(unsigned long currentMillis) {

  /** Shunt can frames seen? **/
  if (ShuntLastSeen + 1000 < currentMillis) {
    datalayer.shunt.available = false;
  } else {
    datalayer.shunt.available = true;
  }

  // Send 10ms CAN Message
  if (currentMillis - previousMillis10 >= INTERVAL_10_MS) {
    previousMillis10 = currentMillis;
    // First check if we have any active errors, incase we do, turn off the battery
    if (datalayer.system.status.system_status == FAULT) {
      timeSpentInFaultedMode++;
    } else {
      timeSpentInFaultedMode = 0;
    }

    //handle contactor control SHUTDOWN_REQUESTED
    if (timeSpentInFaultedMode > MAX_ALLOWED_FAULT_TICKS) {
      contactorStatus = SHUTDOWN_REQUESTED;
      SBOX_100.data.u8[0] = 0x55;  // All open
    }

    if (contactorStatus == SHUTDOWN_REQUESTED) {
      datalayer.shunt.contactors_engaged = false;
      datalayer.system.status.dc_bus_live = false;
      return;  // A fault scenario latches the contactor control. It is not possible to recover without a powercycle (and investigation why fault occured)
    }

    // After that, check if we are OK to start turning on the contactors
    if (contactorStatus == DISCONNECTED) {
      datalayer.shunt.contactors_engaged = false;
      datalayer.system.status.dc_bus_live = false;
      SBOX_100.data.u8[0] = 0x55;  // All open

      // The voltage below arrives in frame 0x210 and is not cleared when the
      // shunt stops sending - it simply keeps its last value forever. Starting
      // the sequence on a frozen reading means starting it without knowing the
      // battery voltage at all, so the shunt has to be live for the reading to
      // mean anything. `available` is maintained a few lines up in this same
      // function, from a one-second timeout on the last frame seen.
      if (datalayer.shunt.available && datalayer.system.status.battery_allows_contactor_closing &&
          datalayer.system.status.inverter_allows_contactor_closing && !datalayer.system.info.equipment_stop_active &&
          (datalayer.shunt.measured_voltage_mV > MINIMUM_INPUT_VOLTAGE * 1000)) {
        contactorStatus = PRECHARGE;
      }
    }
    // In case the inverter requests contactors to open, set the state accordingly
    if (contactorStatus == COMPLETED) {
      //Incase inverter (or estop) requests contactors to open, make state machine jump to Disconnected state (recoverable)
      if (!datalayer.system.status.inverter_allows_contactor_closing || datalayer.system.info.equipment_stop_active) {
        contactorStatus = DISCONNECTED;
      }
    }
    // The shunt going quiet PART WAY THROUGH is the dangerous case, and it is
    // not the same as it being absent at the start.
    //
    // Precharge-complete is decided in POSITIVE below by comparing the input and
    // output voltages, and both come from frames that freeze on their last value
    // when the shunt stops sending. If the final pair happened to satisfy the
    // inequality, the positive contactor closes on a measurement that is no
    // longer being taken - on a shunt this code has already marked unavailable.
    //
    // Neither of the two obvious reactions is safe. Advancing closes the
    // positive contactor across a voltage difference nobody is measuring.
    // Latching where we are leaves the precharge resistor energised, and it is
    // rated for the seconds a precharge takes, not for however long a CAN fault
    // lasts. So do neither: open everything and abandon the sequence.
    //
    // DISCONNECTED rather than SHUTDOWN_REQUESTED, deliberately. This is
    // recoverable in exactly the way the inverter-withdrew-permission path
    // already is, and it cannot busy-loop: restarting requires `available`
    // again, so a shunt that stays away leaves the machine sitting in
    // DISCONNECTED with everything open, which is where it should sit.
    //
    // COMPLETED is deliberately NOT included. There the contactors are closed
    // and the bus is live; opening the positive contactor under load to react
    // to a lost sensor is a worse hazard than the one being avoided. That case
    // wants reporting, not switching.
    if (!datalayer.shunt.available && contactorStatus != DISCONNECTED && contactorStatus != COMPLETED &&
        contactorStatus != SHUTDOWN_REQUESTED) {
      SBOX_100.data.u8[0] = 0x55;  // All open
      contactorStatus = DISCONNECTED;
      datalayer.shunt.precharging = false;
      datalayer.shunt.contactors_engaged = false;
      datalayer.system.status.dc_bus_live = false;
      set_event(EVENT_SHUNT_LOST_DURING_PRECHARGE, 0);
      logging.println("S-BOX shunt lost during the contactor sequence - contactors opened");
    }

    // A shunt that came back clears the abandonment. Without this the ERROR
    // raised above stands forever: events.level stays at ERROR, which holds
    // system_status in FAULT, which this driver's own fault counter turns into
    // SHUTDOWN_REQUESTED after MAX_ALLOWED_FAULT_TICKS - a latch this file
    // documents as needing a power cycle. The abort is meant to be recoverable,
    // and until the event is cleared it is not: a shunt that dropped out for one
    // second and has been healthy ever since would still latch twenty seconds
    // later, mid-sequence. A shunt that STAYS away keeps the event standing and
    // still latches, which is the right outcome and the reason this clears on
    // recovery rather than immediately after raising.
    if (datalayer.shunt.available) {
      clear_event(EVENT_SHUNT_LOST_DURING_PRECHARGE);
      clear_event(EVENT_SHUNT_LOST_WITH_BUS_LIVE);
    } else if (contactorStatus == COMPLETED) {
      // The COMPLETED case deliberately does not switch: opening the positive
      // contactor under load to react to a lost sensor is a worse hazard than
      // running briefly without current measurement. It does need REPORTING,
      // though, and it must not report at ERROR level - that would drive
      // system_status into FAULT and latch SHUTDOWN_REQUESTED, opening those
      // contactors under load after all, by the very path this case exists to
      // avoid. WARNING says it without doing it.
      set_event(EVENT_SHUNT_LOST_WITH_BUS_LIVE, 0);
    }

    // Handle actual state machine. This first turns on Precharge, then Negative, then Positive, and finally turns OFF precharge
    switch (contactorStatus) {
      case PRECHARGE:
        SBOX_100.data.u8[0] = 0x86;  // Precharge relay only
        prechargeStartTime = currentMillis;
        contactorStatus = NEGATIVE;
        logging.println("S-BOX Precharge relay engaged");
        break;
      case NEGATIVE:
        if (currentMillis - prechargeStartTime >= CONTACTOR_CONTROL_T1) {
          SBOX_100.data.u8[0] = 0xA6;  // Precharge + Negative
          negativeStartTime = currentMillis;
          contactorStatus = POSITIVE;
          datalayer.shunt.precharging = true;
          logging.println("S-BOX Negative relay engaged");
        }
        break;
      case POSITIVE:
        if (currentMillis - negativeStartTime >= CONTACTOR_CONTROL_T2 &&
            (datalayer.shunt.measured_voltage_mV * MAX_PRECHARGE_RESISTOR_VOLTAGE_PERCENT <
             datalayer.shunt.measured_outvoltage_mV)) {
          SBOX_100.data.u8[0] = 0xAA;  // Precharge + Negative + Positive
          positiveStartTime = currentMillis;
          contactorStatus = PRECHARGE_OFF;
          datalayer.shunt.precharging = false;
          logging.println("S-BOX Positive relay engaged");
        }
        break;
      case PRECHARGE_OFF:
        if (currentMillis - positiveStartTime >= CONTACTOR_CONTROL_T3) {
          SBOX_100.data.u8[0] = 0x6A;  // Negative + Positive
          contactorStatus = COMPLETED;
          logging.println("S-BOX Precharge relay released");
          datalayer.shunt.contactors_engaged = true;
          datalayer.system.status.dc_bus_live = true;
        }
        break;
      case COMPLETED:
        SBOX_100.data.u8[0] = 0x6A;  // Negative + Positive
      default:
        break;
    }
    CAN100_cnt++;
    if (CAN100_cnt > 0x0E) {
      CAN100_cnt = 0;
    }
    SBOX_100.data.u8[1] = CAN100_cnt << 4 | 0x01;
    SBOX_100.data.u8[3] = 0x00;
    SBOX_100.data.u8[3] = calculateCRC(SBOX_100);
    transmit_can_frame(&SBOX_100);
    transmit_can_frame(&SBOX_300);
  }
}

void BmwSbox::setup() {
  strncpy(datalayer.system.info.shunt_protocol, Name, 31);
  datalayer.system.info.shunt_protocol[31] = '\0';
}
