#include <gtest/gtest.h>

#include <Arduino.h>  // Emul: set_millis64() to control the test clock

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/TEST-FAKE-BATTERY.h"
#include "../Software/src/communication/contactorcontrol/comm_contactorcontrol.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/parallel_safety.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

// Tests for the symmetric parallel-join gate (#2711): the 1.5 V rule used to
// gate only battery 2/3 toward battery 1 - battery 1's own (re-)close was
// never checked, so after a main-pack dropout it could close onto a link
// another pack holds live, with the surge limited only by pack resistances.

class ParallelJoinSymmetryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    reset_all_events();
    init_hal();
    datalayer.system.status.battery_link[1].detected = true;
    datalayer.system.status.battery_link[2].detected = false;
    datalayer.system.status.system_status = ACTIVE;
    // Not 0 and not the 3700 init default (both make the check abort)
    datalayer.batteries[0].status.voltage_dV = 3900;
    datalayer.batteries[1].status.voltage_dV = 3900;
  }

  void TearDown() override {
    datalayer.system.status.battery_link[1].detected = false;
    if (batteries[1]) {
      delete batteries[1];
      batteries[1] = nullptr;
    }
  }
};

TEST_F(ParallelJoinSymmetryTest, EngagedPackWithLargeDiffBlocksMainClose) {
  datalayer.system.status.battery_link[1].contactors_engaged = true;
  datalayer.batteries[1].status.voltage_dV = 3700 + 250;  // 25 V below main

  check_parallel_battery_safety(2);

  EXPECT_FALSE(datalayer.system.status.battery_link[0].allowed_contactor_closing)
      << "Main battery must not close onto a live link with a large voltage difference";
}

TEST_F(ParallelJoinSymmetryTest, EngagedPackWithinWindowAllowsMainClose) {
  datalayer.system.status.battery_link[1].contactors_engaged = true;
  datalayer.batteries[1].status.voltage_dV = 3910;  // 1.0 V difference

  check_parallel_battery_safety(2);

  EXPECT_TRUE(datalayer.system.status.battery_link[0].allowed_contactor_closing);
}

TEST_F(ParallelJoinSymmetryTest, DisengagedPackDoesNotBlockMain) {
  datalayer.system.status.battery_link[1].contactors_engaged = false;
  datalayer.batteries[1].status.voltage_dV = 3600;  // 30 V difference, but link is dead

  check_parallel_battery_safety(2);

  EXPECT_TRUE(datalayer.system.status.battery_link[0].allowed_contactor_closing)
      << "A pack with open contactors holds no link - any voltage difference is fine";
}

TEST_F(ParallelJoinSymmetryTest, ExistingBattery2GatingUnchanged) {
  // Regression guard for the original direction of the rule
  datalayer.batteries[1].status.voltage_dV = 3905;
  check_parallel_battery_safety(2);
  EXPECT_TRUE(datalayer.system.status.battery_link[1].allowed_contactor_closing);

  datalayer.batteries[1].status.voltage_dV = 3600;
  for (int i = 0; i < 11; ++i) {
    check_parallel_battery_safety(2);
  }
  EXPECT_FALSE(datalayer.system.status.battery_link[1].allowed_contactor_closing)
      << "Battery 2 must still disengage after 10 s out of sync";
}

TEST_F(ParallelJoinSymmetryTest, UnknownReportedStateFallsBackToCommanded) {
  // TestFake does not override reported_contactor_state() -> Unknown
  batteries[1] = new TestFakeBattery(&datalayer.batteries[1], CAN_NATIVE);
  datalayer.system.status.battery_link[1].contactors_engaged = true;
  datalayer.batteries[1].status.voltage_dV = 3600;

  check_parallel_battery_safety(2);

  EXPECT_FALSE(datalayer.system.status.battery_link[0].allowed_contactor_closing)
      << "Unknown reported state must fall back to the BE-commanded engaged state";

  datalayer.system.status.battery_link[1].contactors_engaged = false;
  check_parallel_battery_safety(2);
  EXPECT_TRUE(datalayer.system.status.battery_link[0].allowed_contactor_closing);
}

TEST_F(ParallelJoinSymmetryTest, GateBlocksStartPrecharge) {
  set_millis64(100000);  // Past the 10 s startup window
  contactor_control_enabled[0] = true;
  precharge_fsm.set_state(ContactorActuator::DISCONNECTED);
  datalayer.system.status.battery_link[0].detected = true;
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.info.equipment_stop_active = false;

  datalayer.system.status.battery_link[0].allowed_contactor_closing = false;
  handle_contactors();
  EXPECT_EQ(precharge_fsm.state(), ContactorActuator::DISCONNECTED)
      << "The join gate must hold the main battery in DISCONNECTED";

  datalayer.system.status.battery_link[0].allowed_contactor_closing = true;
  handle_contactors();
  // The FSM transitions DISCONNECTED -> START_PRECHARGE -> PRECHARGE within one tick
  EXPECT_EQ(precharge_fsm.state(), ContactorActuator::PRECHARGE);

  contactor_control_enabled[0] = false;
  precharge_fsm.set_state(ContactorActuator::DISCONNECTED);
  set_millis64(0);
}
