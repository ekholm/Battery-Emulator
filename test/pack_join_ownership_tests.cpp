#include <gtest/gtest.h>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/safety/parallel_safety.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

// A second or third pack that reads 0 V, or whose CAN has gone silent, is not on the DC link, so parallel
// safety must take it out of the join. It used to return early on a 0 V reading and leave the join flag as it
// was: a pack that had joined stayed "joined" after it dropped out, and its SOC stayed in the total the inverter
// sees (a live report had the inverter shown 0 % that way).

namespace {

class PackJoinOwnership : public ::testing::Test {
 protected:
  void SetUp() override {
    init_events();
    datalayer = DataLayer();
    battery2_detected = true;
    battery3_detected = true;
    datalayer.system.status.system_status = ACTIVE;
    // All three packs in sync at a value clear of the 3700 startup default, so both extra packs join.
    datalayer.battery.status.voltage_dV = 3750;
    datalayer.battery2.status.voltage_dV = 3750;
    datalayer.battery3.status.voltage_dV = 3750;
    check_parallel_battery_safety(2);
    check_parallel_battery_safety(3);
  }
};

}  // namespace

TEST_F(PackJoinOwnership, AJoinedPackThatReadsZeroVoltsLeavesTheJoin) {
  ASSERT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing) << "precondition: pack 2 joined";
  datalayer.battery2.status.voltage_dV = 0;
  check_parallel_battery_safety(2);
  EXPECT_FALSE(datalayer.system.status.battery2_allowed_contactor_closing)
      << "pack 2 reads 0 V but is still counted as joined";
}

TEST_F(PackJoinOwnership, AJoinedPackThatGoesSilentLeavesTheJoin) {
  ASSERT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing) << "precondition: pack 2 joined";
  datalayer.battery2.status.CAN_battery_still_alive = 0;  // its last voltage is still in the datalayer
  check_parallel_battery_safety(2);
  EXPECT_FALSE(datalayer.system.status.battery2_allowed_contactor_closing)
      << "pack 2's CAN is silent but it is still counted as joined on its last voltage";
}

TEST_F(PackJoinOwnership, TheThirdPackIsTreatedTheSame) {
  ASSERT_TRUE(datalayer.system.status.battery3_allowed_contactor_closing) << "precondition: pack 3 joined";
  datalayer.battery3.status.voltage_dV = 0;
  check_parallel_battery_safety(3);
  EXPECT_FALSE(datalayer.system.status.battery3_allowed_contactor_closing);
}

// The main pack reading 0 V says nothing about pack 2; it only means there is nothing to compare against yet.
TEST_F(PackJoinOwnership, TheMainPackAtZeroVoltsDoesNotUnjoinPack2) {
  datalayer.battery.status.voltage_dV = 0;
  check_parallel_battery_safety(2);
  EXPECT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing);
}

TEST_F(PackJoinOwnership, APackThatComesBackInSyncJoinsAgain) {
  datalayer.battery2.status.voltage_dV = 0;
  check_parallel_battery_safety(2);
  ASSERT_FALSE(datalayer.system.status.battery2_allowed_contactor_closing);
  datalayer.battery2.status.voltage_dV = 3752;  // back, within 1.5 V
  check_parallel_battery_safety(2);
  EXPECT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing) << "pack 2 did not rejoin once back in sync";
}
