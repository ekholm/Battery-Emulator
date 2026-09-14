#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/safety/safety.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/PYLON-LV-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Pylontech LV CAN inverter driver (PYLON-LV-CAN).
//
// TX side: all six frames (0x351..0x35E) are sent every 1000 ms.  Payload
// encoding uses little-endian (LE) word layout throughout.  RX side: only
// 0x305 refreshes aliveness.

namespace {

class PylonLvCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::PylonLv;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    pylon_lv = static_cast<PylonLvInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Advance time past the 1000 ms cadence so all frames are sent.
  void tick_1s(unsigned long base_ms = INTERVAL_1_S + 1) {
    pylon_lv->update_values();
    pylon_lv->transmit_can(base_ms);
  }

  PylonLvInverter* pylon_lv = nullptr;
};

}  // namespace

// ---- Periodic cadence -------------------------------------------------------

TEST_F(PylonLvCanInverterTest, DoesNotTransmitBeforeIntervalElapses) {
  pylon_lv->update_values();
  pylon_lv->transmit_can(999);  // just under 1 s
  EXPECT_TRUE(get_transmitted_frames().empty()) << "Must not send before 1000 ms have elapsed";
}

TEST_F(PylonLvCanInverterTest, SendsAllSixFramesEvery1s) {
  tick_1s();
  EXPECT_NE(find_frame_with_id(0x351), nullptr);
  EXPECT_NE(find_frame_with_id(0x355), nullptr);
  EXPECT_NE(find_frame_with_id(0x356), nullptr);
  EXPECT_NE(find_frame_with_id(0x359), nullptr);
  EXPECT_NE(find_frame_with_id(0x35C), nullptr);
  EXPECT_NE(find_frame_with_id(0x35E), nullptr);
}

TEST_F(PylonLvCanInverterTest, SecondTickDoesNotRetransmitBeforeNextInterval) {
  tick_1s(INTERVAL_1_S + 1);
  clear_transmitted_frames();
  // Same millisecond value → interval has not elapsed again
  pylon_lv->transmit_can(INTERVAL_1_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty());
}

// ---- RX aliveness -----------------------------------------------------------

TEST_F(PylonLvCanInverterTest, Frame0x305RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x305, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  pylon_lv->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(PylonLvCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  pylon_lv->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---- Payload: 0x351 (charge voltage LE, charge and discharge currents LE) ---

TEST_F(PylonLvCanInverterTest, Frame351EncodesChargeVoltageAndCurrentsLE) {
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 300;
  // Keep real_soc in midrange so 35C enables both directions (avoids current zeroing)
  datalayer.battery.status.real_soc = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  // Charge voltage: 4000 LE → bytes 0-1 = 0xA0, 0x0F
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4000u);
  // Max charge current: 200 LE → 0xC8, 0x00
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 200u);
  // Max discharge current: 300 LE → 0x2C, 0x01
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 300u);
}

TEST_F(PylonLvCanInverterTest, Frame351UseUserSuppliedChargeVoltageWhenActive) {
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 3850;
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.status.real_soc = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3850u);
}

TEST_F(PylonLvCanInverterTest, Frame351ClampsUserVoltageToDesignMax) {
  // If user sets a voltage above max_design, the driver clamps to max_design.
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 5500;
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.status.real_soc = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4000u);
}

// ---- Payload: 0x355 (SOC and SOH as integer percent, LE) --------------------

TEST_F(PylonLvCanInverterTest, Frame355EncodesSocAndSohLE) {
  datalayer.battery.status.reported_soc = 7550;  // 75.50% → integer 75
  datalayer.battery.status.soh_pptt = 9900;      // 99.00% → integer 99
  datalayer.battery.status.real_soc = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x355);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 75u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 99u);
}

// ---- Payload: 0x356 (voltage in cV LE, current LE, avg temp LE) -------------

TEST_F(PylonLvCanInverterTest, Frame356EncodesVoltageCvCurrentAndAvgTempLE) {
  datalayer.battery.status.voltage_dV = 4800;                                // 4800 * 10 = 48000 cV = 0xBB80
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(100);  // 10.0 A discharge
  datalayer.battery.status.temperature_max_dC = 300;
  datalayer.battery.status.temperature_min_dC = 100;  // avg = 200
  datalayer.battery.status.real_soc = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x356);
  ASSERT_NE(f, nullptr);
  // Voltage in cV (deciV * 10): 48000 LE
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 48000u);
  // Current (signed LE)
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[2], f->data.u8[3])), 100);
  // Average temperature = (300 + 100) / 2 = 200
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[4], f->data.u8[5])), 200);
}

// ---- Payload: 0x35C (command byte, charge/discharge enable) -----------------

TEST_F(PylonLvCanInverterTest, Frame35CEnablesBothDirectionsByDefault) {
  // Normal: ACTIVE, voltage between design limits, real_soc in [min, max]
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.info.max_design_voltage_dV = 5000;
  datalayer.battery.info.min_design_voltage_dV = 2500;
  datalayer.battery.status.real_soc = 5000;  // between 2000 and 8000

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xC0u);  // bit7=charge, bit6=discharge
}

TEST_F(PylonLvCanInverterTest, Frame35CDisablesAllOnFault) {
  datalayer.system.status.system_status = FAULT;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u);
}

TEST_F(PylonLvCanInverterTest, Frame35CEnablesChargeImmediatelyWhenVoltageBelowMin) {
  datalayer.battery.status.voltage_dV = 2400;
  datalayer.battery.info.min_design_voltage_dV = 2500;
  datalayer.battery.info.max_design_voltage_dV = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xA0u);  // immediate-charge flag
}

TEST_F(PylonLvCanInverterTest, Frame35CAllowsDischargeOnlyWhenVoltageAtMax) {
  datalayer.battery.status.voltage_dV = 5000;
  datalayer.battery.info.max_design_voltage_dV = 5000;
  datalayer.battery.info.min_design_voltage_dV = 2500;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x35C);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x40u);  // discharge only
}

// ---- 35C → 351 feedback: zeroing currents when direction is disabled --------

TEST_F(PylonLvCanInverterTest, ChargeCurrentZeroedWhenChargeDisabledByMaxVoltage) {
  // Voltage at max → 35C = 0x40 (charge disabled) → charge current in 351 = 0
  datalayer.battery.status.voltage_dV = 5000;
  datalayer.battery.info.max_design_voltage_dV = 5000;
  datalayer.battery.info.min_design_voltage_dV = 2500;
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 300;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 0u);    // charge zeroed
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 300u);  // discharge intact
}

// ---- Payload: 0x359 (error/warning flags) -----------------------------------

TEST_F(PylonLvCanInverterTest, Frame359BmsFaultBitSetOnSystemFault) {
  datalayer.system.status.system_status = FAULT;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x359);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->data.u8[1] & 0x80) << "BMS fault bit must be set on FAULT state";
}

TEST_F(PylonLvCanInverterTest, Frame359OverCurrentErrorBitSetWhenDischargeExceedsLimit) {
  // reported_current_dA follows the datalayer's "+ = charging" convention, so a
  // DISCHARGE over-current is a sufficiently negative current: the check is
  // current <= -(max_discharge_current_dA + 10), here -200 <= -190.
  datalayer.battery.status.reported_current_dA = -200;
  datalayer.battery.status.max_discharge_current_dA = 180;
  datalayer.battery.status.real_soc = 5000;

  tick_1s();

  const CAN_frame* f = find_frame_with_id(0x359);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->data.u8[0] & 0x80) << "Discharge overcurrent error bit must be set";
}

// ---- Payload: 0x35E (manufacturer name) ------------------------------------

TEST_F(PylonLvCanInverterTest, Frame35EContainsManufacturerName) {
  tick_1s();
  const CAN_frame* f = find_frame_with_id(0x35E);
  ASSERT_NE(f, nullptr);
  // MANUFACTURER_NAME, "PYLON   " - eight bytes, space-padded. Inverters match
  // on this string, so it is the vendor's name and not this firmware's.
  EXPECT_EQ(f->data.u8[0], 'P');
  EXPECT_EQ(f->data.u8[1], 'Y');
  EXPECT_EQ(f->data.u8[2], 'L');
  EXPECT_EQ(f->data.u8[3], 'O');
  EXPECT_EQ(f->data.u8[4], 'N');
  EXPECT_EQ(f->data.u8[5], ' ');
  EXPECT_EQ(f->data.u8[6], ' ');
  EXPECT_EQ(f->data.u8[7], ' ');
}
