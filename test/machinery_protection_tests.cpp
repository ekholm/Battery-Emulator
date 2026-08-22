#include <gtest/gtest.h>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/TEST-FAKE-BATTERY.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

// Machinery protection runs one helper per battery instance. The temperature
// events are per-pack since upstream #2799/#2850 - each pack owns its own
// OVERHEAT/FROZEN/TEMP_DEVIATION triplet, raised by
// check_battery_temperatures(), so a clean pack structurally CANNOT mask
// another's fault. Events that are still shared (the voltage pair below)
// aggregate through the verdicts: set if ANY instance trips, cleared only
// when ALL are clean.

class MachineryProtectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    reset_all_events();
    reset_safety_state();
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

TEST_F(MachineryProtectionTest, SecondaryOverheatRaisesItsOwnEvent) {
  datalayer.batteries[0].status.temperature_max_dC = 300;  // fine
  datalayer.batteries[1].status.temperature_max_dC = 600;  // > BATTERY_MAXTEMPERATURE

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "The secondary's overheat must land on the secondary's own event";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_OVERHEAT)->data, 600);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_INACTIVE)
      << "The clean primary's event must stay clear - pack 1 is not overheating";
}

TEST_F(MachineryProtectionTest, PrimaryOverheatStaysOnTheUnsuffixedEvent) {
  datalayer.batteries[0].status.temperature_max_dC = 600;
  datalayer.batteries[1].status.temperature_max_dC = 300;

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "Pack 1 keeps the unsuffixed event - existing integrations parse that name";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_OVERHEAT)->state, EVENT_STATE_INACTIVE);
}

TEST_F(MachineryProtectionTest, EachPackRaisesAndClearsItsOwnOverheat) {
  datalayer.batteries[0].status.temperature_max_dC = 550;
  datalayer.batteries[1].status.temperature_max_dC = 650;
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->data, 550);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "Both packs overheating raise both events - impossible while the event was shared";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_OVERHEAT)->data, 650);

  datalayer.batteries[1].status.temperature_max_dC = 300;  // secondary cools down
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_OVERHEAT)->state, EVENT_STATE_INACTIVE)
      << "The cooled pack clears its own event";
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE)
      << "The hot pack's event survives the other pack cooling";

  datalayer.batteries[0].status.temperature_max_dC = 300;  // all clean
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_INACTIVE);
}

// Upstream deliberately un-latched the deviation warning (it follows the
// pack; a latched event could never be released by the clear branch). The
// verdict path used to latch it - this pins the upstream semantics.
TEST_F(MachineryProtectionTest, TemperatureDeviationFollowsThePack) {
  datalayer.batteries[1].status.temperature_max_dC = 300;
  datalayer.batteries[1].status.temperature_min_dC = 50;  // deviation 250 > max

  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_TEMP_DEVIATION_HIGH)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_TEMP_DEVIATION_HIGH)->state, EVENT_STATE_INACTIVE);

  datalayer.batteries[1].status.temperature_min_dC = 250;  // deviation resolves
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY2_TEMP_DEVIATION_HIGH)->state, EVENT_STATE_INACTIVE)
      << "The warning follows the pack - it must clear when the deviation resolves";
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
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERVOLTAGE)->data, 4100);
}

TEST_F(MachineryProtectionTest, SingleBatteryEquivalence) {
  delete batteries[1];
  batteries[1] = nullptr;

  datalayer.batteries[0].status.temperature_max_dC = 600;
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->data, 600);

  datalayer.batteries[0].status.temperature_max_dC = 300;
  update_machineryprotection();
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_OVERHEAT)->state, EVENT_STATE_INACTIVE);
}
