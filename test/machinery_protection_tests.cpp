#include <gtest/gtest.h>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/TEST-FAKE-BATTERY.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

// Machinery protection runs one helper per battery instance, but the shared
// events (EVENT_BATTERY_OVERHEAT etc.) must aggregate: set if ANY instance
// trips, cleared only when ALL are clean. A per-instance set/clear would let
// a clean battery mask another's active fault - the exact masking regression
// documented for check_can_component_alive.

class MachineryProtectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    reset_all_events();
    init_hal();
    datalayer.system.status.system_status = STANDBY;
    batteries[0] = new TestFakeBattery(&datalayer.batteries[0], CAN_NATIVE);
    batteries[1] = new TestFakeBattery(&datalayer.batteries[1], CAN_NATIVE);
  }

  void TearDown() override {
    for (int i = 0; i < MAX_BATTERIES; ++i) {
      delete batteries[i];
      batteries[i] = nullptr;
    }
    reset_all_events();
  }
};

TEST_F(MachineryProtectionTest, SecondaryOverheatNotMaskedByCleanPrimary) {
  datalayer.batteries[0].status.temperature_max_dC = 300;  // fine
  datalayer.batteries[1].status.temperature_max_dC = 600;  // > BATTERY_MAXTEMPERATURE

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "A clean primary must not mask the secondary's overheat";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->data, static_cast<uint8_t>(600));
}

TEST_F(MachineryProtectionTest, PrimaryOverheatNotMaskedByCleanSecondary) {
  datalayer.batteries[0].status.temperature_max_dC = 600;
  datalayer.batteries[1].status.temperature_max_dC = 300;

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "A clean secondary must not mask the primary's overheat";
}

TEST_F(MachineryProtectionTest, SharedEventClearsOnlyWhenAllInstancesClean) {
  datalayer.batteries[0].status.temperature_max_dC = 550;
  datalayer.batteries[1].status.temperature_max_dC = 650;
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->data, static_cast<uint8_t>(650))
      << "Payload is the worst offender's";

  datalayer.batteries[1].status.temperature_max_dC = 300;  // secondary cools down
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "The primary still trips - one cooled battery must not clear the event";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->data, static_cast<uint8_t>(550));

  datalayer.batteries[0].status.temperature_max_dC = 300;  // all clean
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_INACTIVE);
}

TEST_F(MachineryProtectionTest, PowerLimitZeroingIsPerInstance) {
  for (int i = 0; i < 2; ++i) {
    datalayer.batteries[i].info.max_design_voltage_dV = 4000;
    datalayer.batteries[i].status.max_charge_power_W = 5000;
    datalayer.batteries[i].status.max_discharge_power_W = 5000;
  }
  datalayer.batteries[0].status.voltage_dV = 3900;  // within design
  datalayer.batteries[1].status.voltage_dV = 4100;  // over design max

  update_machineryprotection();

  EXPECT_EQ(datalayer.batteries[0].status.max_charge_power_W, 5000u) << "The clean pack keeps its own charge limit";
  EXPECT_EQ(datalayer.batteries[1].status.max_charge_power_W, 0u) << "The overvoltage pack zeroes its own charge limit";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERVOLTAGE)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERVOLTAGE)->data, static_cast<uint8_t>(4100));
}

TEST_F(MachineryProtectionTest, SingleBatteryEquivalence) {
  delete batteries[1];
  batteries[1] = nullptr;

  datalayer.batteries[0].status.temperature_max_dC = 600;
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->data, static_cast<uint8_t>(600));

  datalayer.batteries[0].status.temperature_max_dC = 300;
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_INACTIVE);
}
