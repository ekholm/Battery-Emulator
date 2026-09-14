#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SMA-LV-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the SMA Sunny Island Low Voltage (48 V) CAN inverter
// driver.
//
// TX: all frames sent every 100 ms (big-endian byte order). RX: both 0x305
// and 0x306 bump aliveness; an emergency-stop frame (0x00F) is appended when
// system_status == FAULT.
//
// Voltage limits:
//   Charge voltage = max_design_voltage_dV - 40 dV  (capped at MAX 630)
//   Discharge voltage = min_design_voltage_dV + 40 dV (floored at MIN 41)

namespace {

class SmaLvInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::SmaLv;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    sma = static_cast<SmaLvInverter*>(inverter);
    clear_transmitted_frames();
  }

  SmaLvInverter* sma = nullptr;
};

}  // namespace

TEST_F(SmaLvInverterTest, KnownRxFrame305RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x305, .data = {0}};
  sma->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 3);
}

TEST_F(SmaLvInverterTest, KnownRxFrame306RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x306, .data = {0}};
  sma->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 3);
}

TEST_F(SmaLvInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  sma->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(SmaLvInverterTest, NoTransmitBeforeIntervalExpires) {
  sma->update_values();
  sma->transmit_can(50);  // only 50 ms elapsed; 100 ms interval not yet due
  EXPECT_TRUE(get_transmitted_frames().empty());
}

TEST_F(SmaLvInverterTest, PeriodicCadenceSendsSevenFramesAt100ms) {
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  for (uint32_t id : {0x351u, 0x355u, 0x356u, 0x35Au, 0x35Bu, 0x35Eu, 0x35Fu}) {
    EXPECT_EQ(count_frames_with_id(id), 1u) << "Missing frame 0x" << std::hex << id;
  }
  // Emergency-stop must NOT appear in normal operation.
  EXPECT_EQ(count_frames_with_id(0x00F), 0u);
}

TEST_F(SmaLvInverterTest, LimitsFrameEncodesChargeVoltageWithOffset) {
  // 0x351 — big-endian; charge voltage = max_design - 40 dV
  datalayer.battery.info.max_design_voltage_dV = 580;  // -> 540
  datalayer.battery.status.max_discharge_current_dA = 500;
  datalayer.battery.status.max_charge_current_dA = 125;
  datalayer.battery.info.min_design_voltage_dV = 420;  // -> 460 (420+40)

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  // BE: byte 0 = high byte, byte 1 = low byte
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), static_cast<uint16_t>(580 - 40));
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 500u);  // discharge current
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 125u);  // charge current
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), static_cast<uint16_t>(420 + 40));
}

TEST_F(SmaLvInverterTest, LimitsFrameCapsChargeVoltageAtMax630) {
  // max_design > 630 -> capped at 630 (MAX_VOLTAGE_DV)
  datalayer.battery.info.max_design_voltage_dV = 700;

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 630u);
}

TEST_F(SmaLvInverterTest, LimitsFrameFloorsDischargeVoltageAtMin41) {
  // min_design < 41 -> floored at 41 (MIN_VOLTAGE_DV) after the +40 offset
  datalayer.battery.info.min_design_voltage_dV = 0;

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 41u);
}

TEST_F(SmaLvInverterTest, SocSohFrameEncodesSocSohAndHighResSoc) {
  // 0x355 — big-endian; SOC and SOH in whole percent; HiRes = raw pptt
  datalayer.battery.status.reported_soc = 7550;  // 75.50 %
  datalayer.battery.status.soh_pptt = 9800;      // 98.00 %

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x355);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 75u);    // SOC whole %
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 98u);    // SOH whole %
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 7550u);  // HiRes SOC (pptt)
}

TEST_F(SmaLvInverterTest, VoltageCurrentTempFrameEncodesValues) {
  // 0x356 — voltage in mV (dV * 10), current signed dA, average temperature dC
  datalayer.battery.status.voltage_dV = 520;                                  // 52.0 V -> raw 5200
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-300);  // -30.0 A
  datalayer.battery.status.temperature_max_dC = 250;
  datalayer.battery.status.temperature_min_dC = 150;  // average = 200

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x356);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), static_cast<uint16_t>(520 * 10));
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -300);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[4], f->data.u8[5])), 200);  // (250+150)/2
}

TEST_F(SmaLvInverterTest, FaultAppendsEmergencyStopFrame) {
  datalayer.system.status.system_status = FAULT;

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  EXPECT_EQ(count_frames_with_id(0x00F), 1u) << "FAULT must send the emergency-stop frame";
}

TEST_F(SmaLvInverterTest, SecondIntervalSendsFramesAgain) {
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);
  clear_transmitted_frames();
  sma->transmit_can(2 * (INTERVAL_100_MS + 1));
  EXPECT_EQ(count_frames_with_id(0x351), 1u) << "Frames must repeat each 100 ms interval";
}
