#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/inverter/GROWATT-WIT-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Growatt WIT CAN inverter driver.
//
// TX side: the driver is gated on inverter_alive (set by receiving a heartbeat
// 0x1AB5FFF1 frame). After that, frames are sent on three period groups
// (100 ms, 500 ms, 1000 ms, 2000 ms). All IDs are extended 29-bit.

namespace {

// FSN 0xB5 at bits [23:16] → 0x1AB5XXXX. Driver matches (id >> 16) & 0xFF.
// Actual frame ID from the inverter is 0x1AB5F1FF (SA=0xF1=PCS, TA=0xFF=broadcast).
static constexpr uint32_t ID_HEARTBEAT   = 0x1AB5F1FF;
static constexpr uint32_t ID_PCS_PRODUCT = 0x1ABEF1FF;

class GrowattWitCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::GrowattWit;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    gw = static_cast<GrowattWitInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Wake the driver: send one heartbeat to set inverter_alive.
  void wake_inverter() {
    CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = ID_HEARTBEAT, .data = {0}};
    gw->map_can_frame_to_variable(f);
  }

  GrowattWitInverter* gw = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// RX / aliveness and gating
// ---------------------------------------------------------------------------

TEST_F(GrowattWitCanInverterTest, HeartbeatRefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  wake_inverter();
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(GrowattWitCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x12345678, .data = {0}};
  gw->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(GrowattWitCanInverterTest, StandardFrameIsIgnoredByRxHandler) {
  // Driver must reject non-extended frames (ext_ID == false).
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = ID_HEARTBEAT, .data = {0}};
  gw->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0)
      << "Standard (11-bit) frame must be ignored by Growatt driver";
}

TEST_F(GrowattWitCanInverterTest, StaysSilentUntilInverterSpeaksFirst) {
  gw->update_values();
  gw->transmit_can(INTERVAL_2_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Driver must not transmit before receiving heartbeat";
}

// ---------------------------------------------------------------------------
// State: PCS product info triggers event messages
// ---------------------------------------------------------------------------

TEST_F(GrowattWitCanInverterTest, PcsProductTriggersSendsEventMessages) {
  gw->update_values();
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = ID_PCS_PRODUCT, .data = {0}};
  gw->map_can_frame_to_variable(f);
  // 1AC2 (product version) and 1A80 (BMS SW version) must be sent.
  EXPECT_GT(count_frames_with_id(0x1AC2FFF3), 0u) << "1AC2 product version expected";
  EXPECT_GT(count_frames_with_id(0x1A80FFF3), 0u) << "1A80 BMS SW version expected";
  // Serial number: 3 frames of 1A82.
  EXPECT_EQ(count_frames_with_id(0x1A82FFF3), 3u)  << "3 serial-number frames expected";
}

// ---------------------------------------------------------------------------
// TX: periodic cadence
// ---------------------------------------------------------------------------

TEST_F(GrowattWitCanInverterTest, PeriodicGroupsCadence) {
  wake_inverter();
  gw->update_values();

  // 100 ms group: 1AC3, 1AC4, 1AC5, 1AC7, 1ACE, 1ACF, 1AD8, 1AD9
  gw->transmit_can(INTERVAL_100_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x1AC3FFF3), 1u) << "1AC3 at 100ms";
  EXPECT_EQ(count_frames_with_id(0x1AC4FFF3), 1u) << "1AC4 at 100ms";
  EXPECT_EQ(count_frames_with_id(0x1AC5FFF3), 1u) << "1AC5 at 100ms";
  EXPECT_EQ(count_frames_with_id(0x1AC7FFF3), 1u) << "1AC7 at 100ms";
  EXPECT_EQ(count_frames_with_id(0x1ACEFFF3), 1u) << "1ACE at 100ms";
  EXPECT_EQ(count_frames_with_id(0x1ACFFFF3), 1u) << "1ACF at 100ms";
  // 500 ms group must NOT have fired yet.
  EXPECT_EQ(count_frames_with_id(0x1AC6FFF3), 0u) << "1AC6 not yet at 100ms";

  clear_transmitted_frames();

  // 500 ms group: 1AC6, 1AC8
  gw->transmit_can(INTERVAL_500_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x1AC6FFF3), 1u) << "1AC6 at 500ms";
  EXPECT_EQ(count_frames_with_id(0x1AC8FFF3), 1u) << "1AC8 at 500ms";

  clear_transmitted_frames();

  // 1000 ms group: 1AC9, 1ACA, 1ACC, 1ACD, 1AD0, 1AD1
  gw->transmit_can(INTERVAL_1_S + 1);
  EXPECT_EQ(count_frames_with_id(0x1AC9FFF3), 1u);
  EXPECT_EQ(count_frames_with_id(0x1ACAFFF3), 1u);
  EXPECT_EQ(count_frames_with_id(0x1ACCFFF3), 1u);
  EXPECT_EQ(count_frames_with_id(0x1ACDFFF3), 1u);
  EXPECT_EQ(count_frames_with_id(0x1AD0FFF3), 1u);
  EXPECT_EQ(count_frames_with_id(0x1AD1FFF3), 1u);

  clear_transmitted_frames();

  // 2000 ms group: 1AC0
  gw->transmit_can(INTERVAL_2_S + 1);
  EXPECT_EQ(count_frames_with_id(0x1AC0FFF3), 1u) << "1AC0 at 2000ms";
}

// ---------------------------------------------------------------------------
// TX payload encoding
// ---------------------------------------------------------------------------

TEST_F(GrowattWitCanInverterTest, LimitsFrameEncodesCurrentAndVoltageLE) {
  datalayer.battery.status.max_charge_current_dA    = 150;
  datalayer.battery.status.max_discharge_current_dA = 250;
  datalayer.battery.info.max_design_voltage_dV      = 4100;
  datalayer.battery.info.min_design_voltage_dV      = 2900;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC3FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->ext_ID) << "1AC3 must use extended ID";
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 150u)  << "charge current LE";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 250u)  << "discharge current LE";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 4100u) << "max charge voltage LE";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 2900u) << "min discharge voltage LE";
}

TEST_F(GrowattWitCanInverterTest, UserVoltageLimitsOverrideDesignInLimitsFrame) {
  datalayer.battery.settings.user_set_voltage_limits_active    = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV    = 3950;
  datalayer.battery.settings.max_user_set_discharge_voltage_dV = 3100;
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC3FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 3950u);
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 3100u);
}

TEST_F(GrowattWitCanInverterTest, CVVoltageIs100dVBelowMaxChargeVoltage) {
  datalayer.battery.info.max_design_voltage_dV = 4200;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC4FFF3);
  ASSERT_NE(f, nullptr);
  // cv_voltage = max_charge_voltage - 100 dV
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 4100u) << "CV voltage = max-100";
}

TEST_F(GrowattWitCanInverterTest, SocSohAndCapacityEncodedIn1AC6) {
  datalayer.battery.status.reported_soc         = 6500;  // 65.00 % → byte 65
  datalayer.battery.status.soh_pptt             = 9200;  // 92.00 % → byte 92
  datalayer.battery.info.reported_total_capacity_Wh = 20000;
  datalayer.battery.status.voltage_dV           = 4000;  // 400 V
  // dAh = Wh * 100 / voltage_dV = 20000*100/4000 = 500
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC6FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 65u) << "SOC byte";
  EXPECT_EQ(f->data.u8[1], 92u) << "SOH byte (SOH >=50, no scrap bit)";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 500u) << "rated capacity dAh LE";
}

TEST_F(GrowattWitCanInverterTest, LowSohSetsScrapWarningBit) {
  datalayer.battery.status.soh_pptt   = 4900;  // 49 % → scrap warning bit
  datalayer.battery.status.voltage_dV = 3500;
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC6FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_NE(f->data.u8[1] & 0x80, 0) << "SOH scrap warning bit (bit7) must be set";
  EXPECT_EQ(f->data.u8[1] & 0x7F, 49u) << "SOH value byte (without scrap bit)";
}

TEST_F(GrowattWitCanInverterTest, VoltageCurrentEncodedIn1AC7WithOffset) {
  datalayer.battery.status.voltage_dV  = 3800;
  datalayer.battery.status.current_dA  = static_cast<int16_t>(-300);  // -30.0 A
  // Raw current = current_dA + 10000 = 9700
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC7FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 3800u)  << "voltage LE b2-3";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 9700u)  << "current + offset LE";
}

TEST_F(GrowattWitCanInverterTest, WorkingStatusByteFaultChargeDischarge) {
  // Fault → status byte = 5
  datalayer.system.status.system_status = FAULT;
  datalayer.battery.status.current_dA   = 200;
  gw->update_values();
  wake_inverter();
  gw->transmit_can(INTERVAL_100_MS + 1);
  {
    const CAN_frame* f = find_frame_with_id(0x1AC5FFF3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->data.u8[0], 5u) << "Fault status";
  }
  datalayer.system.status.system_status = ACTIVE;
  clear_transmitted_frames();

  // Charging: current > 5 dA → status = 2
  datalayer.battery.status.current_dA = 10;
  gw->update_values();
  gw->transmit_can(INTERVAL_100_MS + 101);
  {
    const CAN_frame* f = find_last_frame_with_id(0x1AC5FFF3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->data.u8[0], 2u) << "Charging status";
  }
  clear_transmitted_frames();

  // Discharging: current < -5 dA → status = 3
  datalayer.battery.status.current_dA = static_cast<int16_t>(-10);
  gw->update_values();
  gw->transmit_can(INTERVAL_100_MS + 202);
  {
    const CAN_frame* f = find_last_frame_with_id(0x1AC5FFF3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->data.u8[0], 3u) << "Discharging status";
  }
}

TEST_F(GrowattWitCanInverterTest, TemperatureEncodingWithOffsetIn1ACC) {
  // Max temp = 25.0 °C = 250 dC → raw = 250 + 400 = 650
  // Min temp = -5.0 °C = -50 dC → raw: clamped to 0 (< 0)
  datalayer.battery.status.temperature_max_dC = 250;
  datalayer.battery.status.temperature_min_dC = static_cast<int16_t>(-50);
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x1ACCFFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 650u) << "max temp raw (250+400)";
  // avg = (250 + (-50)) / 2 = 100; raw = 100 + 400 = 500
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 500u) << "avg temp raw";
}

TEST_F(GrowattWitCanInverterTest, MinTempBelowOffsetClampedToZeroIn1ACD) {
  // TEMP_OFFSET_DC = 400 (represents +40 °C). calc = temp_dC + 400.
  // Clamp to 0 only fires when temp_dC < -400 (i.e., below -40.0 °C).
  // Using -500 dC (-50.0 °C): calc = -500 + 400 = -100 < 0 → clamped to 0.
  datalayer.battery.status.temperature_min_dC = static_cast<int16_t>(-500);
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x1ACDFFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 0u) << "temp below -40 °C clamped to raw 0";
}

TEST_F(GrowattWitCanInverterTest, ChargeForbiddenFlagIn1AC5) {
  datalayer.battery.status.max_charge_current_dA    = 0;
  datalayer.battery.status.max_discharge_current_dA = 100;
  // reported_soc must be non-zero to not also trigger the discharge-forbidden path
  // (which fires when reported_soc == 0, see GROWATT-WIT-CAN.cpp).
  datalayer.battery.status.reported_soc = 5000;
  gw->update_values();

  wake_inverter();
  gw->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1AC5FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_NE(f->data.u8[1] & 0x01, 0) << "charge-prohibited bit in byte 1";
  EXPECT_EQ(f->data.u8[2] & 0x01, 0) << "discharge-allowed bit in byte 2 must be clear";
}
