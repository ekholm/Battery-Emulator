#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/BYD-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the BYD Battery-Box Premium HVS CAN inverter driver.
//
// TX side: datalayer values must arrive on the wire in the frames and byte
// positions the inverter documents. RX side: inverter frames must refresh the
// aliveness counter and (in shunt mode) populate the shunt datalayer.

namespace {

class BydCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The global DataLayerResetListener has already reset datalayer/events and
    // destroyed the previous inverter instance before this runs.
    user_selected_inverter_protocol = InverterProtocolType::BydCan;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    byd = static_cast<BydCanInverter*>(inverter);
    clear_transmitted_frames();
  }

  // The driver refuses to talk until the inverter has sent us something.
  void wake_inverter() {
    CAN_frame keepalive = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x191, .data = {0}};
    byd->map_can_frame_to_variable(keepalive);
  }

  BydCanInverter* byd = nullptr;
};

}  // namespace

TEST_F(BydCanInverterTest, StaysSilentUntilInverterSpeaksFirst) {
  byd->update_values();
  byd->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Driver must not transmit before the inverter has sent a frame";
}

TEST_F(BydCanInverterTest, KnownRxFramesRefreshAliveness) {
  for (uint32_t id : {0x091u, 0x0D1u, 0x111u, 0x191u}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    byd->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE) << "ID 0x" << std::hex << id;
  }
}

TEST_F(BydCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  byd->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(BydCanInverterTest, IdentificationRequestTriggersInitialData) {
  CAN_frame ident = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x151, .data = {0x01, 0, 0, 0, 0, 0, 0, 0}};
  byd->map_can_frame_to_variable(ident);

  // 0x250, 0x290, 0x2D0 and four 0x3D0 name fragments
  EXPECT_EQ(count_frames_with_id(0x250), 1u);
  EXPECT_EQ(count_frames_with_id(0x290), 1u);
  EXPECT_EQ(count_frames_with_id(0x2D0), 1u);
  EXPECT_EQ(count_frames_with_id(0x3D0), 4u);
}

TEST_F(BydCanInverterTest, FirstTransmitAfterWakeSendsInitialDataOnce) {
  wake_inverter();
  byd->transmit_can(1);
  EXPECT_EQ(count_frames_with_id(0x250), 1u);
  clear_transmitted_frames();
  byd->transmit_can(2);
  EXPECT_EQ(count_frames_with_id(0x250), 0u) << "Initial data must only be sent once";
}

TEST_F(BydCanInverterTest, LimitsFrameEncodesDesignVoltageWindowWithOffset) {
  datalayer.battery.info.max_design_voltage_dV = 4040;  // 404.0 V
  datalayer.battery.info.min_design_voltage_dV = 3000;  // 300.0 V
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.status.max_discharge_current_dA = 300;  // 30.0 A
  datalayer.battery.status.max_charge_current_dA = 250;     // 25.0 A

  byd->update_values();
  wake_inverter();
  byd->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* limits = find_frame_with_id(0x110);
  ASSERT_NE(limits, nullptr);
  // Charge voltage target = max design - 2.0 V offset = 4020
  EXPECT_EQ(u16_be(limits->data.u8[0], limits->data.u8[1]), 4020);
  // Discharge voltage target = min design + 2.0 V offset = 3020
  EXPECT_EQ(u16_be(limits->data.u8[2], limits->data.u8[3]), 3020);
  EXPECT_EQ(u16_be(limits->data.u8[4], limits->data.u8[5]), 300);
  EXPECT_EQ(u16_be(limits->data.u8[6], limits->data.u8[7]), 250);
}

TEST_F(BydCanInverterTest, LimitsFrameHonoursUserVoltageLimits) {
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 3900;
  datalayer.battery.settings.max_user_set_discharge_voltage_dV = 3100;
  datalayer.battery.info.max_design_voltage_dV = 4040;
  datalayer.battery.info.min_design_voltage_dV = 3000;

  byd->update_values();
  wake_inverter();
  byd->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* limits = find_frame_with_id(0x110);
  ASSERT_NE(limits, nullptr);
  EXPECT_EQ(u16_be(limits->data.u8[0], limits->data.u8[1]), 3900);
  EXPECT_EQ(u16_be(limits->data.u8[2], limits->data.u8[3]), 3100);
}

TEST_F(BydCanInverterTest, StatesFrameEncodesSocSohAndAhCapacities) {
  datalayer.battery.status.reported_soc = 7550;  // 75.50 %
  datalayer.battery.status.soh_pptt = 9900;      // 99.00 %
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.info.min_design_voltage_dV = 3000;  // nominal = 3500 dV
  datalayer.battery.status.reported_remaining_capacity_Wh = 21000;
  datalayer.battery.info.reported_total_capacity_Wh = 30000;

  byd->update_values();
  wake_inverter();
  byd->transmit_can(INTERVAL_10_S + 1);

  const CAN_frame* states = find_frame_with_id(0x150);
  ASSERT_NE(states, nullptr);
  EXPECT_EQ(u16_be(states->data.u8[0], states->data.u8[1]), 7550);
  EXPECT_EQ(u16_be(states->data.u8[2], states->data.u8[3]), 9900);
  // remaining Ah*10 = Wh * 100 / nominal_dV = 21000*100/3500 = 600 (60.0 Ah)
  EXPECT_EQ(u16_be(states->data.u8[4], states->data.u8[5]), 600);
  // full Ah*10 = 30000*100/3500 = 857
  EXPECT_EQ(u16_be(states->data.u8[6], states->data.u8[7]), 857);
}

TEST_F(BydCanInverterTest, AhCapacityFallsBackToPackVoltageWithoutDesignLimits) {
  datalayer.battery.info.max_design_voltage_dV = 0;
  datalayer.battery.info.min_design_voltage_dV = 0;
  datalayer.battery.status.voltage_dV = 3500;
  datalayer.battery.status.reported_remaining_capacity_Wh = 21000;
  datalayer.battery.info.reported_total_capacity_Wh = 30000;

  byd->update_values();
  wake_inverter();
  byd->transmit_can(INTERVAL_10_S + 1);

  const CAN_frame* states = find_frame_with_id(0x150);
  ASSERT_NE(states, nullptr);
  EXPECT_EQ(u16_be(states->data.u8[4], states->data.u8[5]), 600);
  EXPECT_EQ(u16_be(states->data.u8[6], states->data.u8[7]), 857);
}

TEST_F(BydCanInverterTest, DeyeWorkaroundForcesSocEndpoints) {
  user_selected_inverter_deye_workaround = true;
  datalayer.battery.status.reported_soc = 5000;

  // Battery refuses charge -> report full
  datalayer.battery.status.max_charge_current_dA = 0;
  datalayer.battery.status.max_discharge_current_dA = 100;
  byd->update_values();
  wake_inverter();
  byd->transmit_can(INTERVAL_10_S + 1);
  const CAN_frame* states = find_frame_with_id(0x150);
  ASSERT_NE(states, nullptr);
  EXPECT_EQ(u16_be(states->data.u8[0], states->data.u8[1]), 10000);

  // Battery refuses discharge -> report empty (discharge branch runs last and wins)
  clear_transmitted_frames();
  datalayer.battery.status.max_charge_current_dA = 100;
  datalayer.battery.status.max_discharge_current_dA = 0;
  byd->update_values();
  byd->transmit_can(2 * (INTERVAL_10_S + 1));
  states = find_frame_with_id(0x150);
  ASSERT_NE(states, nullptr);
  EXPECT_EQ(u16_be(states->data.u8[0], states->data.u8[1]), 0);

  user_selected_inverter_deye_workaround = false;
}

TEST_F(BydCanInverterTest, BatteryInfoFrameEncodesVoltageCurrentAndAvgTemperature) {
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-810);  // -81.0 A
  datalayer.battery.status.temperature_max_dC = 250;
  datalayer.battery.status.temperature_min_dC = 210;

  byd->update_values();
  wake_inverter();
  byd->transmit_can(INTERVAL_10_S + 1);

  const CAN_frame* info = find_frame_with_id(0x1D0);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(u16_be(info->data.u8[0], info->data.u8[1]), 3700);
  EXPECT_EQ(static_cast<int16_t>(u16_be(info->data.u8[2], info->data.u8[3])), -810);
  EXPECT_EQ(static_cast<int16_t>(u16_be(info->data.u8[4], info->data.u8[5])), 230);  // (250+210)/2

  const CAN_frame* cells = find_frame_with_id(0x210);
  ASSERT_NE(cells, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(cells->data.u8[0], cells->data.u8[1])), 250);
  EXPECT_EQ(static_cast<int16_t>(u16_be(cells->data.u8[2], cells->data.u8[3])), 210);
}

TEST_F(BydCanInverterTest, PeriodicCadenceSendsEachGroupAtItsInterval) {
  wake_inverter();
  byd->transmit_can(1);  // initial data only
  clear_transmitted_frames();

  byd->transmit_can(INTERVAL_2_S + 2);  // 2s group due, 10s/60s not
  EXPECT_EQ(count_frames_with_id(0x110), 1u);
  EXPECT_EQ(count_frames_with_id(0x150), 0u);
  EXPECT_EQ(count_frames_with_id(0x190), 0u);

  clear_transmitted_frames();
  byd->transmit_can(INTERVAL_10_S + 2);  // 10s group joins
  EXPECT_EQ(count_frames_with_id(0x150), 1u);
  EXPECT_EQ(count_frames_with_id(0x1D0), 1u);
  EXPECT_EQ(count_frames_with_id(0x210), 1u);
  EXPECT_EQ(count_frames_with_id(0x190), 0u);

  clear_transmitted_frames();
  byd->transmit_can(INTERVAL_60_S + 2);  // 60s alarm frame joins
  EXPECT_EQ(count_frames_with_id(0x190), 1u);
}

TEST_F(BydCanInverterTest, ShuntModePopulatesShuntDatalayerFromInverterMeasurement) {
  byd->enable_shunt();
  // 370.0 V -> raw 3700 * 0.1 stored; +50 A; 25.0 C -> raw 250.
  // A negative current is NOT covered here: measured_amperage_dA is uint16_t
  // (datalayer.h) while measured_amperage_mA is int32_t, so a discharge
  // current wraps in the dA field - recorded as an upstream finding rather
  // than pinned as intended behaviour.
  CAN_frame meas = {.FD = false,
                    .ext_ID = false,
                    .DLC = 8,
                    .ID = 0x091,
                    .data = {0x0E, 0x74, 0x00, 0x32, 0x00, 0xFA, 0x00, 0x00}};
  byd->map_can_frame_to_variable(meas);

  EXPECT_TRUE(datalayer.shunt.available);
  EXPECT_TRUE(datalayer.shunt.contactors_engaged);
  EXPECT_FALSE(datalayer.shunt.precharging);
  // 0x0E74 = 3700 raw -> *0.1 = 370 dV stored
  EXPECT_EQ(datalayer.shunt.measured_voltage_dV, 370);
  EXPECT_EQ(datalayer.shunt.measured_voltage_mV, 37000);
  // 0x0032 = +50 -> dA = 5, mA = 500
  EXPECT_EQ(datalayer.shunt.measured_amperage_dA, 5);
  EXPECT_EQ(datalayer.shunt.measured_amperage_mA, 500);
}

TEST_F(BydCanInverterTest, WithoutShuntModeInverterMeasurementLeavesShuntAlone) {
  CAN_frame meas = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x091, .data = {0x0E, 0x74, 0, 0, 0, 0, 0, 0}};
  byd->map_can_frame_to_variable(meas);
  EXPECT_FALSE(datalayer.shunt.available);
}
