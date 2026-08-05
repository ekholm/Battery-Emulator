#include <gtest/gtest.h>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/safety/parallel_safety.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

class VoltageSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    init_events();
    // The drift counters in check_parallel_battery_safety() are function-local
    // statics, so a preceding run of the timeout tests leaves them latched at
    // 10 and the next run in the same process starts mid-fault - a plain
    // --gtest_repeat=2 fails without this. They are not reachable from a
    // fixture; one in-sync pass through the public API is the reset (the
    // <=1.5V branch zeroes the counter). 3750 dodges the 3700-startup-default
    // guard, which returns before touching the counter.
    battery2_detected = true;
    battery3_detected = true;
    datalayer.battery.status.voltage_dV = 3750;
    datalayer.battery2.status.voltage_dV = 3750;
    datalayer.battery3.status.voltage_dV = 3750;
    check_parallel_battery_safety(2);
    check_parallel_battery_safety(3);
    // Reset datalayer to known state
    datalayer.batteries[0].status.voltage_dV = 3700;  // 370.0V
    for (int i = 1; i < MAX_BATTERIES; i++) {
      datalayer_battery(i).status.voltage_dV = 3700;  // 370.0V
    }
    datalayer.system.status.system_status = ACTIVE;
    datalayer.system.status.battery_link[1].allowed_contactor_closing = false;
    datalayer.system.status.battery_link[2].allowed_contactor_closing = false;
    datalayer.system.status.battery_link[1].detected = true;
    datalayer.system.status.battery_link[2].detected = true;
  }
};

// Test: When battery is powered OFF (no CAN comm), the allowed closing should be false
TEST_F(VoltageSyncTest, Battery2NotPoweredOn) {
  datalayer.system.status.battery_link[1].detected = false;  //Not detected via CAN
  datalayer.batteries[0].status.voltage_dV = 3700;           //Default startup voltage
  datalayer_battery(1).status.voltage_dV = 3700;             //Default startup voltage

  check_parallel_battery_safety(2);

  EXPECT_FALSE(datalayer.system.status.battery_link[1].allowed_contactor_closing);
}

// Test: When voltages are in sync, battery2 is allowed to close contactors
TEST_F(VoltageSyncTest, Battery2AllowedWhenVoltagesInSync) {
  datalayer.batteries[0].status.voltage_dV = 3750;
  datalayer_battery(1).status.voltage_dV = 3750;

  check_parallel_battery_safety(2);

  EXPECT_TRUE(datalayer.system.status.battery_link[1].allowed_contactor_closing);
}

// Test: When voltage drifts >1.5V, battery2 should be disconnected after 10 seconds
TEST_F(VoltageSyncTest, Battery2DisconnectedAfterVoltageDriftTimeout) {
  datalayer.system.status.battery_link[1].allowed_contactor_closing = true;
  datalayer.batteries[0].status.voltage_dV = 3710;  // 370.0V
  datalayer_battery(1).status.voltage_dV = 3500;    // 350.0V — 20V difference, way over 1.5V

  // Simulate 10 seconds of calls (function called once per second)
  for (int i = 0; i < 10; i++) {
    check_parallel_battery_safety(2);
    // During the first 10 calls, battery2 should still be allowed (counting up)
    EXPECT_TRUE(datalayer.system.status.battery_link[1].allowed_contactor_closing)
        << "Should still be allowed at second " << i;
  }

  // 11th call — counter reaches 10, battery2 should be disconnected
  check_parallel_battery_safety(2);
  EXPECT_FALSE(datalayer.system.status.battery_link[1].allowed_contactor_closing);
}

// Test: After voltage drift timeout, re-syncing resets the counter and allows reconnection
TEST_F(VoltageSyncTest, Battery2ReconnectsAfterVoltagesResync) {
  datalayer.batteries[0].status.voltage_dV = 3710;
  datalayer_battery(1).status.voltage_dV = 3500;  // Out of sync

  // Drift for 11 seconds — battery2 disconnected
  for (int i = 0; i < 11; i++) {
    check_parallel_battery_safety(2);
  }
  EXPECT_FALSE(datalayer.system.status.battery_link[1].allowed_contactor_closing);

  // Voltages re-sync
  datalayer_battery(1).status.voltage_dV = 3710;
  check_parallel_battery_safety(2);
  EXPECT_TRUE(datalayer.system.status.battery_link[1].allowed_contactor_closing);
}

// Test: Battery3 has the same voltage drift timeout behavior
TEST_F(VoltageSyncTest, Battery3DisconnectedAfterVoltageDriftTimeout) {
  datalayer.system.status.battery_link[2].allowed_contactor_closing = true;
  datalayer.batteries[0].status.voltage_dV = 3710;
  datalayer_battery(2).status.voltage_dV = 3500;  // 20V difference

  for (int i = 0; i < 10; i++) {
    check_parallel_battery_safety(3);
    EXPECT_TRUE(datalayer.system.status.battery_link[2].allowed_contactor_closing)
        << "Should still be allowed at second " << i;
  }

  check_parallel_battery_safety(3);
  EXPECT_FALSE(datalayer.system.status.battery_link[2].allowed_contactor_closing);
}

// Test: Battery1 fault disengages battery2 even when voltages match
TEST_F(VoltageSyncTest, Battery1FaultDisengagesBattery2) {
  datalayer.batteries[0].status.voltage_dV = 3710;
  datalayer_battery(1).status.voltage_dV = 3700;  // In sync
  datalayer.system.status.system_status = FAULT;

  check_parallel_battery_safety(2);
  EXPECT_FALSE(datalayer.system.status.battery_link[1].allowed_contactor_closing);
}

// Test: Zero voltage skips the check entirely (no crash, no state change)
TEST_F(VoltageSyncTest, ZeroVoltageSkipsCheck) {
  datalayer.batteries[0].status.voltage_dV = 0;
  datalayer_battery(1).status.voltage_dV = 3710;
  datalayer.system.status.battery_link[1].allowed_contactor_closing = true;

  check_parallel_battery_safety(2);
  // Should remain unchanged — early return
  EXPECT_TRUE(datalayer.system.status.battery_link[1].allowed_contactor_closing);
}
