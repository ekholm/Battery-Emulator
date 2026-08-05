#include "BMW-SBOX.h"
#include <Arduino.h>
#include "../communication/can/comm_can.h"
#include "../datalayer/datalayer.h"
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

// The close sequence in S-BOX relay order: precharge relay first, then
// negative, then positive (voltage-ratio guarded), then precharge release.
void BmwSbox::SboxContactorActuator::apply_step(State step) {
  switch (step) {
    case START_PRECHARGE:
      sbox_.SBOX_100.data.u8[0] = 0x86;  // Precharge relay only
      logging.println("S-BOX Precharge relay engaged");
      break;
    case PRECHARGE:
      sbox_.SBOX_100.data.u8[0] = 0xA6;  // Precharge + Negative
      datalayer.shunt.precharging = true;
      logging.println("S-BOX Negative relay engaged");
      break;
    case POSITIVE:
      sbox_.SBOX_100.data.u8[0] = 0xAA;  // Precharge + Negative + Positive
      datalayer.shunt.precharging = false;
      logging.println("S-BOX Positive relay engaged");
      break;
    case PRECHARGE_OFF:
      sbox_.SBOX_100.data.u8[0] = 0x6A;  // Negative + Positive
      logging.println("S-BOX Precharge relay released");
      datalayer.shunt.contactors_engaged = true;
      datalayer.system.status.dc_bus_live = true;
      break;
    default:
      break;
  }
}

unsigned long BmwSbox::SboxContactorActuator::step_delay_ms(State step) const {
  switch (step) {
    case PRECHARGE:
      return CONTACTOR_CONTROL_T1;
    case POSITIVE:
      return CONTACTOR_CONTROL_T2;
    case PRECHARGE_OFF:
      return CONTACTOR_CONTROL_T3;
    default:
      return 0;
  }
}

bool BmwSbox::SboxContactorActuator::step_gate(State step) {
  if (step == POSITIVE) {
    // The positive relay only engages once the output side has precharged to
    // the required fraction of the input voltage (faulty resistor guard).
    return datalayer.shunt.measured_voltage_mV * MAX_PRECHARGE_RESISTOR_VOLTAGE_PERCENT <
           datalayer.shunt.measured_outvoltage_mV;
  }
  return true;
}

bool BmwSbox::SboxContactorActuator::start_allowed() {
  return datalayer.system.status.battery_link[0].allows_contactor_closing &&
         (datalayer.shunt.measured_voltage_mV > MINIMUM_INPUT_VOLTAGE * 1000);
}

void BmwSbox::SboxContactorActuator::open_all(bool fault) {
  sbox_.SBOX_100.data.u8[0] = 0x55;  // All open
  datalayer.shunt.contactors_engaged = false;
  datalayer.system.status.dc_bus_live = false;
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

    contactor_fsm_.tick(currentMillis);

    if (contactor_fsm_.state() == ContactorActuator::SHUTDOWN_REQUESTED) {
      // A fault scenario latches the contactor control, and frames stop
      // entirely: CAN silence drops the S-BOX relays (fail-open). Not possible
      // to recover without a powercycle (and investigation why fault occured)
      return;
    }

    if (contactor_fsm_.state() == ContactorActuator::COMPLETED) {
      SBOX_100.data.u8[0] = 0x6A;  // Negative + Positive
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
