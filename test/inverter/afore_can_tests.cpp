#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/AFORE-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Afore CAN inverter driver (Afore 2.3 CAN standard,
// little-endian, 500 kbps).
//
// TX side: all frames are sent in one burst each time the inverter's 0x305
// heartbeat arrives. RX side: only 0x305 refreshes aliveness and arms the
// transmit gate.

namespace {

class AforeCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::AforeCan;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    afore = static_cast<AforeCanInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Feeding a 0x305 heartbeat arms the transmit gate (time_to_send_info).
  void wake_inverter() {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x305, .data = {0}};
    afore->map_can_frame_to_variable(f);
  }

  AforeCanInverter* afore = nullptr;
};

}  // namespace

TEST_F(AforeCanInverterTest, StaysSilentUntilInverterSpeaksFirst) {
  afore->update_values();
  afore->transmit_can(0);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Must not transmit before the inverter sends 0x305";
}

TEST_F(AforeCanInverterTest, KnownRxFrameRefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  wake_inverter();
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(AforeCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  afore->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(AforeCanInverterTest, OnWakeAllElevenFramesAreTransmitted) {
  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  // 0x350-0x358 (content), 0x359-0x35A (serial name)
  for (uint32_t id = 0x350; id <= 0x35A; ++id) {
    EXPECT_EQ(count_frames_with_id(id), 1u) << "Expected exactly one frame with ID 0x" << std::hex << id;
  }
}

TEST_F(AforeCanInverterTest, TxGateResetAfterOneBurst) {
  wake_inverter();
  afore->transmit_can(0);
  EXPECT_EQ(count_frames_with_id(0x350), 1u);
  clear_transmitted_frames();

  // Second transmit without a new 0x305 must be silent.
  afore->transmit_can(1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Transmit gate must be cleared after each burst";
}

TEST_F(AforeCanInverterTest, OperationFrameEncodesVoltageCurrentTemperature) {
  // 0x350 — Operation information (little-endian throughout)
  datalayer.battery.status.voltage_dV = 3750;                                     // 375.0 V
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-100);      // -10.0 A discharge
  datalayer.battery.status.temperature_max_dC = 250;                              // 25.0 °C

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x350);
  ASSERT_NE(f, nullptr);
  // Voltage raw (LE)
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3750u);
  // Current: raw = dA + 5000 = 4900 (offset so 0 discharge = 5000)
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), static_cast<uint16_t>(-100 + 5000));
  // Temperature: raw = max_dC + 1000 = 1250
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), static_cast<uint16_t>(250 + 1000));
}

TEST_F(AforeCanInverterTest, BatteryInfoFrameEncodesSocAndSoh) {
  // 0x351 — Battery information
  datalayer.battery.status.reported_soc = 7500;  // 75.00 %
  datalayer.battery.status.soh_pptt = 9800;      // 98.00 %
  datalayer.battery.info.number_of_cells = 96;
  datalayer.battery.status.max_charge_current_dA = 100;     // keep enable bits set
  datalayer.battery.status.max_discharge_current_dA = 200;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 75u);    // SOC %
  EXPECT_EQ(f->data.u8[1], 98u);    // SOH %
  EXPECT_EQ(f->data.u8[2], 100u);   // SOCMAX constant
  EXPECT_EQ(f->data.u8[3], 1u);     // SOCMIN constant
  // Normal operation: Bit0 (charge), Bit1 (discharge), Bit5 (normal) all set
  EXPECT_EQ(f->data.u8[4], 0x23u);
  // Number of cells LE
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 96u);
}

TEST_F(AforeCanInverterTest, StatusByteChargeFlagClearedWhenMaxChargeIsZero) {
  datalayer.battery.status.max_charge_current_dA = 0;
  datalayer.battery.status.max_discharge_current_dA = 100;
  datalayer.battery.status.reported_soc = 5000;  // non-zero so discharge condition is not also triggered

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  // Bit0 cleared (charge disabled), Bit1 set (discharge ok), Bit5 set (normal)
  EXPECT_EQ(f->data.u8[4] & 0x01u, 0u) << "Charge-enable flag must be clear";
  EXPECT_EQ(f->data.u8[4] & 0x02u, 0x02u) << "Discharge-enable flag must remain set";
}

TEST_F(AforeCanInverterTest, StatusByteChargeFlagClearedWhenSocFull) {
  datalayer.battery.status.reported_soc = 10000;  // 100.00 %
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 100;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[4] & 0x01u, 0u) << "Charge flag must be clear at 100 % SOC";
}

TEST_F(AforeCanInverterTest, StatusByteDischargeFlagClearedWhenMaxDischargeIsZero) {
  datalayer.battery.status.max_charge_current_dA = 100;
  datalayer.battery.status.max_discharge_current_dA = 0;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[4] & 0x02u, 0u) << "Discharge-enable flag must be clear";
  EXPECT_EQ(f->data.u8[4] & 0x01u, 0x01u) << "Charge-enable flag must remain set";
}

TEST_F(AforeCanInverterTest, FaultModeSetsStatusBitsAndClearsEnableFlags) {
  datalayer.system.status.system_status = FAULT;
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 200;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  // Bits 5-7 = 0b100 (Fault = 4), Bit5=0 Bit6=0 Bit7=1 -> masked into 0x80
  EXPECT_EQ(f->data.u8[4] & 0xE0u, 0x80u) << "BMS status bits must indicate Fault";
  EXPECT_EQ(f->data.u8[4] & 0x01u, 0u) << "Charge-enable must be cleared on FAULT";
  EXPECT_EQ(f->data.u8[4] & 0x02u, 0u) << "Discharge-enable must be cleared on FAULT";
}

TEST_F(AforeCanInverterTest, ProtectionParametersFrameEncodesCurrentAndVoltage) {
  // 0x352 — Protection parameters (little-endian)
  datalayer.battery.status.max_charge_current_dA = 300;
  datalayer.battery.status.max_discharge_current_dA = 500;
  datalayer.battery.info.max_design_voltage_dV = 4200;
  datalayer.battery.info.min_design_voltage_dV = 3000;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x352);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 300u);   // max charge current
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 500u);   // max discharge current
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 4200u);  // max design voltage
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 3000u);  // min design voltage
}

TEST_F(AforeCanInverterTest, CellVoltageFramePassesThroughLfpVoltages) {
  // 0x354 — LFP: values forwarded directly (no remapping)
  datalayer.battery.info.chemistry = battery_chemistry_enum::LFP;
  datalayer.battery.status.cell_max_voltage_mV = 3400;
  datalayer.battery.status.cell_min_voltage_mV = 2900;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x354);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3400u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 2900u);
}

TEST_F(AforeCanInverterTest, CellVoltageFrameRemapsNonLfpVoltages) {
  // 0x354 — Non-LFP: linear interpolation [2500-4200] -> [2500-3400]
  // Formula: 2500 + (raw - 2500) * 900 / 1700
  datalayer.battery.info.chemistry = battery_chemistry_enum::NCA;
  datalayer.battery.status.cell_max_voltage_mV = 4200;  // top of NCA range -> 3400
  datalayer.battery.status.cell_min_voltage_mV = 2500;  // bottom of range  -> 2500

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x354);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3400u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 2500u);
}

TEST_F(AforeCanInverterTest, TemperatureFrameEncodesMaxAndMinWithOffset) {
  // 0x355 — Cell temperature parameters; raw = dC + 1000
  datalayer.battery.status.temperature_max_dC = 350;   // 35.0 °C -> raw 1350
  datalayer.battery.status.temperature_min_dC = 100;   // 10.0 °C -> raw 1100

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x355);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), static_cast<uint16_t>(350 + 1000));
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), static_cast<uint16_t>(100 + 1000));
}

TEST_F(AforeCanInverterTest, SingleCellProtectionFrameEncodesInfoVoltages) {
  // 0x356 — Single cell protection parameters
  datalayer.battery.info.max_cell_voltage_mV = 3650;
  datalayer.battery.info.min_cell_voltage_mV = 2700;

  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* f = find_frame_with_id(0x356);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3650u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 2700u);
}

TEST_F(AforeCanInverterTest, SerialNameFramesCarryBatteryEmulatorAscii) {
  // 0x359 = "Battery-" (8 bytes), 0x35A = "emulator" (8 bytes)
  afore->update_values();
  wake_inverter();
  afore->transmit_can(0);

  const CAN_frame* s0 = find_frame_with_id(0x359);
  const CAN_frame* s1 = find_frame_with_id(0x35A);
  ASSERT_NE(s0, nullptr);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s0->data.u8[0], 'b');
  EXPECT_EQ(s0->data.u8[7], '-');
  EXPECT_EQ(s1->data.u8[0], 'e');
  EXPECT_EQ(s1->data.u8[7], 'r');
}
