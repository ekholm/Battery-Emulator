#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/safety/safety.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/SOL-ARK-LV-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Sol-Ark LV CAN inverter driver (v1.3 protocol,
// little-endian, 500 kbps, 1 s period).
//
// Six frames are sent every second: 0x351 (voltage/current limits), 0x355
// (SOC/SOH), 0x356 (voltage/current/temp), 0x359 (protection/status),
// 0x35C (charge-control byte), 0x35E (manufacturer string "BAT-EMU ").
// RX: only 0x305 refreshes aliveness.

namespace {

class SolArkLvInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::SolArkLv;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    solark = static_cast<SolArkLvInverter*>(inverter);
    clear_transmitted_frames();
  }

  SolArkLvInverter* solark = nullptr;
};

}  // namespace

TEST_F(SolArkLvInverterTest, KnownRxFrame305RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x305, .data = {0}};
  solark->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(SolArkLvInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  solark->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(SolArkLvInverterTest, NoTransmitBeforeIntervalExpires) {
  solark->update_values();
  solark->transmit_can(500);  // only 500 ms; 1 s interval not yet due
  EXPECT_TRUE(get_transmitted_frames().empty());
}

TEST_F(SolArkLvInverterTest, PeriodicCadenceSendsSixFramesAt1s) {
  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  for (uint32_t id : {0x351u, 0x355u, 0x356u, 0x359u, 0x35Cu, 0x35Eu}) {
    EXPECT_EQ(count_frames_with_id(id), 1u) << "Missing frame 0x" << std::hex << id;
  }
}

TEST_F(SolArkLvInverterTest, LimitsFrameEncodesDesignVoltagesAndCurrents) {
  // 0x351 — little-endian; charge voltage = max_design (no user limit)
  datalayer.battery.info.max_design_voltage_dV = 5000;
  datalayer.battery.status.max_charge_current_dA = 300;
  datalayer.battery.status.max_discharge_current_dA = 500;
  datalayer.battery.info.min_design_voltage_dV = 4000;
  datalayer.battery.settings.user_set_voltage_limits_active = false;

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 5000u);  // charge voltage = max_design
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 300u);   // max charge current
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 500u);   // max discharge current
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 4000u);  // min design voltage
}

TEST_F(SolArkLvInverterTest, LimitsFrameHonoursUserChargeVoltage) {
  // When user_set_voltage_limits_active, charge voltage comes from user setting
  datalayer.battery.info.max_design_voltage_dV = 5000;
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 4800;

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4800u);
}

TEST_F(SolArkLvInverterTest, LimitsFrameCapsUserVoltageAtDesignMax) {
  // User-set voltage above max_design must be clamped to max_design
  datalayer.battery.info.max_design_voltage_dV = 5000;
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 5200;  // exceeds design

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 5000u) << "User voltage must not exceed design max";
}

TEST_F(SolArkLvInverterTest, SocSohFrameEncodesWholePercent) {
  // 0x355 — LE; SOC and SOH in whole percent (pptt / 100)
  datalayer.battery.status.reported_soc = 7500;  // 75.00 %
  datalayer.battery.status.soh_pptt = 9800;      // 98.00 %

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x355);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 75u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 98u);
}

TEST_F(SolArkLvInverterTest, VoltageCurrentTempFrameEncodesSignedValues) {
  // 0x356 — LE; voltage dV, current signed dA, average temperature signed dC
  datalayer.battery.status.voltage_dV = 4800;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-100);  // charging
  datalayer.battery.status.temperature_min_dC = 150;
  datalayer.battery.status.temperature_max_dC = 250;  // avg = 200

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x356);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4800u);
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[2], f->data.u8[3])), -100);
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[4], f->data.u8[5])), 200);  // (250+150)/2
}

TEST_F(SolArkLvInverterTest, StatusFrameCarriesModuleNumberAndPN) {
  // 0x359 — byte 4 = MODULE_NUMBER (1), bytes 5-6 = 'P','N'
  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x359);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[4], 1u);     // MODULE_NUMBER
  EXPECT_EQ(f->data.u8[5], 0x50u);  // 'P'
  EXPECT_EQ(f->data.u8[6], 0x4Eu);  // 'N'
  EXPECT_EQ(f->data.u8[7], 0x00u);  // unused
}

TEST_F(SolArkLvInverterTest, ControlFrameIsC0InNormalOperation) {
  // 0x35C byte 0 == 0xC0: both charge and discharge enabled
  datalayer.system.status.system_status = ACTIVE;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  // real_soc between min and max percentages (defaults 2000–8000 in pptt units)
  datalayer.battery.status.real_soc = 5000;

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xC0u) << "Normal: both charge and discharge enabled";
}

TEST_F(SolArkLvInverterTest, ControlFrameIs00OnFault) {
  datalayer.system.status.system_status = FAULT;

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u) << "FAULT: all charge/discharge disabled";
}

TEST_F(SolArkLvInverterTest, ControlFrameIs40WhenVoltageAboveUserChargeLimit) {
  // Only discharge allowed when pack voltage exceeds user charge voltage
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 4800;
  datalayer.battery.status.voltage_dV = 4900;  // above limit

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x40u) << "Above user charge limit: discharge only";
}

TEST_F(SolArkLvInverterTest, ControlFrameIsA0WhenVoltageUnderUserDischargeLimit) {
  // Charge forced when voltage is below user discharge voltage
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 5000;   // high enough not to trigger charge-overvolt
  datalayer.battery.settings.max_user_set_discharge_voltage_dV = 4500;
  datalayer.battery.status.voltage_dV = 4400;  // below discharge limit

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xA0u) << "Under user discharge limit: force charge";
}

TEST_F(SolArkLvInverterTest, ControlFrameIsA0WhenSocAtMin) {
  // Charge forced when real_soc reaches min_percentage (default 2000 pptt = 20%)
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.status.real_soc = 1000;  // below min_percentage (2000)

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xA0u) << "SOC at min: force charge";
}

TEST_F(SolArkLvInverterTest, ControlFrameIs40WhenSocAtMax) {
  // Only discharge when real_soc >= max_percentage (default 8000 pptt = 80%)
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.status.real_soc = 9000;  // above max_percentage (8000)

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x40u) << "SOC at max: discharge only";
}

TEST_F(SolArkLvInverterTest, ManufacturerFrameIsFixedBatEmu) {
  // 0x35E carries "BAT-EMU " as pre-filled static data
  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35E);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 'B');
  EXPECT_EQ(f->data.u8[1], 'A');
  EXPECT_EQ(f->data.u8[2], 'T');
  EXPECT_EQ(f->data.u8[3], '-');
  EXPECT_EQ(f->data.u8[4], 'E');
  EXPECT_EQ(f->data.u8[5], 'M');
  EXPECT_EQ(f->data.u8[6], 'U');
  EXPECT_EQ(f->data.u8[7], ' ');
}

TEST_F(SolArkLvInverterTest, ProtectionByte1OvercurrentBitSet) {
  // 0x359 bit 7 of byte 0: current >= max_discharge + 50
  datalayer.battery.status.max_discharge_current_dA = 500;
  datalayer.battery.status.reported_current_dA = 551;  // 500 + 51 > 50 threshold

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x359);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0] & 0x80u, 0x80u) << "Discharge overcurrent bit must be set";
}

TEST_F(SolArkLvInverterTest, ProtectionByte2FaultBitSet) {
  // 0x359 bit 7 of byte 1: system_status == FAULT
  datalayer.system.status.system_status = FAULT;

  solark->update_values();
  solark->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x359);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[1] & 0x80u, 0x80u) << "Fault bit must be set in byte 1";
}
