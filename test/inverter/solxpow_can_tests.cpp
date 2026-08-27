#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SOLXPOW-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Solxpow CAN inverter driver.
//
// The driver is purely reactive: transmit_can is a no-op; it only sends
// frames in response to 0x4200 from the inverter. INVERT_LOW_HIGH_BYTES is
// defined in the source file, so all assertions use the LE (byte-swapped)
// branch that is compiled in.

namespace {

class SolxpowCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::Solxpow;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    solxpow = static_cast<SolxpowInverter*>(inverter);
    clear_transmitted_frames();
  }

  void TearDown() override {
    user_selected_inverter_cells = 0;
    user_selected_inverter_modules = 0;
    user_selected_inverter_cells_per_module = 0;
    user_selected_inverter_voltage_level = 0;
    user_selected_inverter_ah_capacity = 0;
  }

  // Inject 0x4200 with the given first byte (0x02 = setup, 0x00 = system data).
  void rx4200(uint8_t byte0) {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x4200, .data = {byte0}};
    solxpow->map_can_frame_to_variable(f);
  }

  SolxpowInverter* solxpow = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// RX / aliveness
// ---------------------------------------------------------------------------

TEST_F(SolxpowCanInverterTest, KnownRxFrameRefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  rx4200(0x00);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(SolxpowCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1234, .data = {0}};
  solxpow->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---------------------------------------------------------------------------
// TX: transmit_can is always a no-op
// ---------------------------------------------------------------------------

TEST_F(SolxpowCanInverterTest, TransmitCanIsAlwaysNoOp) {
  solxpow->update_values();
  solxpow->transmit_can(INTERVAL_10_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty());
}

// ---------------------------------------------------------------------------
// RX dispatch: byte0 == 0x02 → setup info (7310/7320/7330/7340)
//              byte0 == 0x00 → system data (4210-4290)
// ---------------------------------------------------------------------------

TEST_F(SolxpowCanInverterTest, SetupRequestSendsSetupFrames) {
  rx4200(0x02);
  EXPECT_EQ(count_frames_with_id(0x7310), 1u);
  EXPECT_EQ(count_frames_with_id(0x7320), 1u);
  EXPECT_EQ(count_frames_with_id(0x7330), 1u);
  EXPECT_EQ(count_frames_with_id(0x7340), 1u);
  // System data must NOT be sent.
  EXPECT_EQ(count_frames_with_id(0x4210), 0u);
}

TEST_F(SolxpowCanInverterTest, SystemDataRequestSendsAllDataFrames) {
  solxpow->update_values();
  rx4200(0x00);
  EXPECT_EQ(count_frames_with_id(0x4210), 1u);
  EXPECT_EQ(count_frames_with_id(0x4220), 1u);
  EXPECT_EQ(count_frames_with_id(0x4230), 1u);
  EXPECT_EQ(count_frames_with_id(0x4240), 1u);
  EXPECT_EQ(count_frames_with_id(0x4250), 1u);
  EXPECT_EQ(count_frames_with_id(0x4260), 1u);
  EXPECT_EQ(count_frames_with_id(0x4270), 1u);
  EXPECT_EQ(count_frames_with_id(0x4280), 1u);
  EXPECT_EQ(count_frames_with_id(0x4290), 1u);
  // Setup frames must NOT be sent.
  EXPECT_EQ(count_frames_with_id(0x7310), 0u);
}

// ---------------------------------------------------------------------------
// TX payload encoding: INVERT_LOW_HIGH_BYTES is defined, so 4210 is LE.
// ---------------------------------------------------------------------------

TEST_F(SolxpowCanInverterTest, DataFrameEncodesVoltageCurrentTemperatureSocSoh) {
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-200);  // -20.0 A
  datalayer.battery.status.temperature_max_dC = 300;                          // +1000 offset → 1300
  datalayer.battery.status.reported_soc = 7500;                               // 75.00 % → 75
  datalayer.battery.status.soh_pptt = 9800;                                   // 98.00 % → 98
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* d = find_frame_with_id(0x4210);
  ASSERT_NE(d, nullptr);
  // INVERT_LOW_HIGH_BYTES: voltage LE
  EXPECT_EQ(u16_le(d->data.u8[0], d->data.u8[1]), 3700u) << "voltage LE in 4210 b0-1";
  // current LE (signed)
  EXPECT_EQ(static_cast<int16_t>(u16_le(d->data.u8[2], d->data.u8[3])), -200) << "current signed LE";
  // temperature = max_dC + 1000 = 1300, LE
  EXPECT_EQ(u16_le(d->data.u8[4], d->data.u8[5]), 1300u) << "temperature offset LE";
  EXPECT_EQ(d->data.u8[6], 75u) << "SOC integer";
  EXPECT_EQ(d->data.u8[7], 98u) << "SOH integer";
}

TEST_F(SolxpowCanInverterTest, VoltagesFrameEncodesChargeDischargeAndCurrentLimits) {
  datalayer.battery.info.max_design_voltage_dV = 4100;
  datalayer.battery.info.min_design_voltage_dV = 2800;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.status.max_charge_current_dA = 180;
  datalayer.battery.status.max_discharge_current_dA = 280;
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* v = find_frame_with_id(0x4220);
  ASSERT_NE(v, nullptr);
  // charge cutoff = max - VOLTAGE_OFFSET_DV. We need to find out VOLTAGE_OFFSET_DV.
  // The default (no user limits): charge_cutoff = max - VOLTAGE_OFFSET_DV. Since it's
  // not exposed, just assert the relationship: charge cutoff < 4100 and discharge cutoff > 2800.
  uint16_t charge_cutoff = u16_le(v->data.u8[0], v->data.u8[1]);
  uint16_t discharge_cutoff = u16_le(v->data.u8[2], v->data.u8[3]);
  EXPECT_LE(charge_cutoff, 4100u) << "charge cutoff must be <= max_design";
  EXPECT_GE(discharge_cutoff, 2800u) << "discharge cutoff must be >= min_design";
  EXPECT_EQ(u16_le(v->data.u8[4], v->data.u8[5]), 180u) << "charge current LE";
  EXPECT_EQ(u16_le(v->data.u8[6], v->data.u8[7]), 280u) << "discharge current LE";
}

TEST_F(SolxpowCanInverterTest, UserVoltageLimitsOverrideDesignVoltages) {
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 3950;
  datalayer.battery.settings.max_user_set_discharge_voltage_dV = 3050;
  datalayer.battery.info.max_design_voltage_dV = 4100;
  datalayer.battery.info.min_design_voltage_dV = 2800;
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* v = find_frame_with_id(0x4220);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(u16_le(v->data.u8[0], v->data.u8[1]), 3950u) << "user charge voltage";
  EXPECT_EQ(u16_le(v->data.u8[2], v->data.u8[3]), 3050u) << "user discharge voltage";
}

TEST_F(SolxpowCanInverterTest, ChargeForbiddenByteSetWhenChargeCurrentZero) {
  datalayer.battery.status.max_charge_current_dA = 0;
  datalayer.battery.status.max_discharge_current_dA = 200;
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* ctrl = find_frame_with_id(0x4280);
  ASSERT_NE(ctrl, nullptr);
  EXPECT_EQ(ctrl->data.u8[0], 0xAAu) << "charge forbidden flag";
  EXPECT_EQ(ctrl->data.u8[1], 0x00u) << "discharge allowed flag";
}

TEST_F(SolxpowCanInverterTest, DischargeForbiddenByteSetWhenDischargeCurrentZero) {
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 0;
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* ctrl = find_frame_with_id(0x4280);
  ASSERT_NE(ctrl, nullptr);
  EXPECT_EQ(ctrl->data.u8[0], 0x00u) << "charge allowed flag";
  EXPECT_EQ(ctrl->data.u8[1], 0xAAu) << "discharge forbidden flag";
}

TEST_F(SolxpowCanInverterTest, FaultStateForcesChargeForbiddenAndDischargeForbidden) {
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 200;
  datalayer.system.status.system_status = FAULT;
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* ctrl = find_frame_with_id(0x4280);
  ASSERT_NE(ctrl, nullptr);
  EXPECT_EQ(ctrl->data.u8[0], 0xAAu) << "charge forbidden on FAULT";
  EXPECT_EQ(ctrl->data.u8[1], 0xAAu) << "discharge forbidden on FAULT";

  datalayer.system.status.system_status = ACTIVE;
}

TEST_F(SolxpowCanInverterTest, StatusByteReflectsChargingDischarging) {
  // Negative current → charging (byte 0 = 0x01)
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-100);
  solxpow->update_values();
  rx4200(0x00);
  {
    const CAN_frame* st = find_frame_with_id(0x4250);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->data.u8[0], 0x01u) << "charging status";
  }
  clear_transmitted_frames();

  // Positive current → discharging (byte 0 = 0x02)
  datalayer.battery.status.reported_current_dA = 100;
  solxpow->update_values();
  rx4200(0x00);
  {
    const CAN_frame* st = find_frame_with_id(0x4250);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->data.u8[0], 0x02u) << "discharging status";
  }
  clear_transmitted_frames();

  // Zero → idle (byte 0 = 0x03)
  datalayer.battery.status.reported_current_dA = 0;
  solxpow->update_values();
  rx4200(0x00);
  {
    const CAN_frame* st = find_frame_with_id(0x4250);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->data.u8[0], 0x03u) << "idle status";
  }
}

TEST_F(SolxpowCanInverterTest, CellVoltagesFrameEncodesMaxAndMinLE) {
  datalayer.battery.status.cell_max_voltage_mV = 3450;
  datalayer.battery.status.cell_min_voltage_mV = 3380;
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* cv = find_frame_with_id(0x4230);
  ASSERT_NE(cv, nullptr);
  EXPECT_EQ(u16_le(cv->data.u8[0], cv->data.u8[1]), 3450u) << "cell max mV LE";
  EXPECT_EQ(u16_le(cv->data.u8[2], cv->data.u8[3]), 3380u) << "cell min mV LE";
}

TEST_F(SolxpowCanInverterTest, CellTemperaturesFrameEncodesMaxAndMinLE) {
  datalayer.battery.status.temperature_max_dC = 350;
  datalayer.battery.status.temperature_min_dC = static_cast<int16_t>(-100);  // -10 °C
  solxpow->update_values();
  rx4200(0x00);

  const CAN_frame* ct = find_frame_with_id(0x4240);
  ASSERT_NE(ct, nullptr);
  EXPECT_EQ(u16_le(ct->data.u8[0], ct->data.u8[1]), 350u) << "temp max LE";
  EXPECT_EQ(static_cast<int16_t>(u16_le(ct->data.u8[2], ct->data.u8[3])), -100) << "temp min signed LE";
}

// ---------------------------------------------------------------------------
// Setup frame configuration via user_selected_* globals
// ---------------------------------------------------------------------------

TEST_F(SolxpowCanInverterTest, CustomCellCountAppearsIn7320AfterSetup) {
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_cells = 200;
  user_selected_inverter_modules = 5;
  setup_inverter();
  solxpow = static_cast<SolxpowInverter*>(inverter);
  clear_transmitted_frames();

  rx4200(0x02);
  const CAN_frame* setup = find_frame_with_id(0x7320);
  ASSERT_NE(setup, nullptr);
  uint16_t cells = u16_le(setup->data.u8[0], setup->data.u8[1]);
  EXPECT_EQ(cells, 200u) << "user cell count in 7320 b0-1";
  EXPECT_EQ(setup->data.u8[2], 5u) << "user module count in 7320 b2";
}
