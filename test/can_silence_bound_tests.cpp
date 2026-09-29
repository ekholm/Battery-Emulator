#include <gtest/gtest.h>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/TEST-FAKE-BATTERY.h"
#include "../Software/src/charger/CHARGERS.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/parallel_safety.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

// A battery whose CAN goes silent, in two stages. update_machineryprotection() runs once a second; drivers
// refresh the pack's alive counter to CAN_STILL_ALIVE on every frame.
//   - After 10 s of silence the pack offers no charge or discharge power: its last limits are stale.
//   - After 45 s its missing event is raised. For pack 1 that is an error, so FAULT and the contactors open;
//     a second pack is taken off the DC link.
// A pack never heard from since boot keeps the whole 60 s window before it is reported missing.

extern bool charger_detected;

namespace {

constexpr uint32_t LIMIT_W = 5000;

class CanSilenceBoundTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    init_events();  // the event levels: pack 1 missing is an error
    reset_all_events();
    init_hal();
    datalayer.system.info.CPU_free_heap = 200000;  // stay clear of the low-heap check
    battery = new TestFakeBattery();
    battery_detected = true;
    emulator_pause_request_ON = false;
    datalayer.system.status.system_status = ACTIVE;
    // A healthy pack, so no other check touches the limits or raises an error of its own.
    datalayer.battery.status.reported_soc = 5000;
    datalayer.battery.status.real_soc = 5000;
    datalayer.battery.status.soh_pptt = 9900;
    datalayer.battery.status.voltage_dV = 4000;
    datalayer.battery.status.cell_max_voltage_mV = 3700;
    datalayer.battery.status.cell_min_voltage_mV = 3690;
  }

  // The driver's side of one second: publish limits, and a frame if the pack spoke.
  void driver(DATALAYER_BATTERY_STATUS_TYPE& status, bool frame) {
    status.max_charge_power_W = LIMIT_W;
    status.max_discharge_power_W = LIMIT_W;
    if (frame) {
      status.CAN_battery_still_alive = CAN_STILL_ALIVE;
    }
  }

  // One safety cycle in which the pack was heard, then `silent` cycles in which it was not.
  void heard_then_silent(DATALAYER_BATTERY_STATUS_TYPE& status, int silent) {
    driver(status, true);
    update_machineryprotection();
    for (int i = 0; i < silent; i++) {
      driver(status, false);
      update_machineryprotection();
    }
  }

  EVENTS_STATE_TYPE state(EVENTS_ENUM_TYPE e) { return get_event_pointer(e)->state; }
};

}  // namespace

TEST_F(CanSilenceBoundTest, Pack1KeepsItsLimitsFor9sAndLosesThemAt10s) {
  heard_then_silent(datalayer.battery.status, 9);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, LIMIT_W) << "9 s of silence must not cut the limits yet";
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, LIMIT_W);

  driver(datalayer.battery.status, false);
  update_machineryprotection();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u) << "10 s of silence must zero the charge limit";
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u) << "10 s of silence must zero the discharge limit";
  EXPECT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_INACTIVE) << "stage 1 is not the missing event";
  EXPECT_NE(datalayer.system.status.system_status, FAULT);
}

TEST_F(CanSilenceBoundTest, Pack1IsReportedMissingAt45sNot44s) {
  heard_then_silent(datalayer.battery.status, 44);
  EXPECT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_INACTIVE) << "44 s of silence must not fault yet";
  EXPECT_NE(datalayer.system.status.system_status, FAULT);

  driver(datalayer.battery.status, false);
  update_machineryprotection();
  EXPECT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_ACTIVE) << "45 s of silence must report pack 1 missing";
  EXPECT_EQ(datalayer.system.status.system_status, FAULT) << "a missing pack 1 is a FAULT, which opens the contactors";
}

TEST_F(CanSilenceBoundTest, AFrameBringsTheLimitsBackAndClearsMissing) {
  heard_then_silent(datalayer.battery.status, 50);
  ASSERT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_ACTIVE);

  datalayer.system.status.system_status = ACTIVE;  // what the event level returns to once the error clears
  heard_then_silent(datalayer.battery.status, 0);
  EXPECT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_INACTIVE);
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, LIMIT_W);
}

// Before its first frame a pack may still be waking up: no stage 1, and missing only when the counter runs out.
TEST_F(CanSilenceBoundTest, APackNeverHeardFromKeepsTheWholeWindow) {
  battery_detected = false;
  datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE - 1;  // the boot value
  for (int s = 1; s < CAN_STILL_ALIVE; s++) {
    driver(datalayer.battery.status, false);
    update_machineryprotection();
  }
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, LIMIT_W) << "an undetected pack was cut as if it fell silent";
  EXPECT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_INACTIVE) << "an undetected pack was reported before 60 s";

  driver(datalayer.battery.status, false);
  update_machineryprotection();
  EXPECT_EQ(state(EVENT_CAN_BATTERY_MISSING), EVENT_STATE_ACTIVE);
}

class CanSilenceBoundPack2Test : public CanSilenceBoundTest {
 protected:
  void SetUp() override {
    CanSilenceBoundTest::SetUp();
    battery2 = new TestFakeBattery(&datalayer.battery2, CAN_Interface::CAN_NATIVE);
    battery2_detected = true;
    datalayer.battery.status.voltage_dV = 4000;
    datalayer.battery2.status.voltage_dV = 4000;
    datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;
  }

  // Pack 1 stays heard; pack 2 goes silent. Parallel safety runs before the protection check, as in the loop.
  void cycle(bool pack2_frame) {
    datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;
    driver(datalayer.battery2.status, pack2_frame);
    check_parallel_battery_safety(2);
    update_machineryprotection();
  }
};

TEST_F(CanSilenceBoundPack2Test, Pack2LosesItsLimitsAt10sAndItsJoinAt45s) {
  cycle(true);
  for (int s = 1; s <= 9; s++) {
    cycle(false);
  }
  EXPECT_EQ(datalayer.battery2.status.max_charge_power_W, LIMIT_W);
  EXPECT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing);

  cycle(false);  // 10 s
  EXPECT_EQ(datalayer.battery2.status.max_charge_power_W, 0u) << "10 s of pack 2 silence must zero its limits";
  EXPECT_EQ(datalayer.battery2.status.max_discharge_power_W, 0u);

  for (int s = 11; s <= 44; s++) {
    cycle(false);
  }
  EXPECT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing) << "pack 2 was dropped before 45 s";
  EXPECT_EQ(state(EVENT_CAN_BATTERY2_MISSING), EVENT_STATE_INACTIVE);

  cycle(false);  // 45 s
  EXPECT_FALSE(datalayer.system.status.battery2_allowed_contactor_closing) << "45 s of silence must drop pack 2";
  EXPECT_EQ(state(EVENT_CAN_BATTERY2_MISSING), EVENT_STATE_ACTIVE);
  EXPECT_NE(datalayer.system.status.system_status, FAULT) << "a missing pack 2 is a warning, not a system fault";

  cycle(true);
  EXPECT_TRUE(datalayer.system.status.battery2_allowed_contactor_closing) << "a pack heard again rejoins";
}

TEST_F(CanSilenceBoundTest, Pack3LosesItsLimitsAt10sAndItsJoinAt45s) {
  battery3 = new TestFakeBattery(&datalayer.battery3, CAN_Interface::CAN_NATIVE);
  battery3_detected = true;
  datalayer.battery.status.voltage_dV = 4000;
  datalayer.battery3.status.voltage_dV = 4000;
  auto cycle = [this](bool frame) {
    datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;
    driver(datalayer.battery3.status, frame);
    check_parallel_battery_safety(3);
    update_machineryprotection();
  };
  cycle(true);
  for (int s = 1; s <= 9; s++) {
    cycle(false);
  }
  EXPECT_EQ(datalayer.battery3.status.max_discharge_power_W, LIMIT_W);
  cycle(false);  // 10 s
  EXPECT_EQ(datalayer.battery3.status.max_charge_power_W, 0u) << "10 s of pack 3 silence must zero its limits";
  EXPECT_EQ(datalayer.battery3.status.max_discharge_power_W, 0u);
  for (int s = 11; s <= 44; s++) {
    cycle(false);
  }
  EXPECT_TRUE(datalayer.system.status.battery3_allowed_contactor_closing) << "pack 3 was dropped before 45 s";
  cycle(false);  // 45 s
  EXPECT_FALSE(datalayer.system.status.battery3_allowed_contactor_closing) << "45 s of silence must drop pack 3";
  EXPECT_EQ(state(EVENT_CAN_BATTERY3_MISSING), EVENT_STATE_ACTIVE);
}

// The charger is not a pack: it keeps the old 60 s window.
TEST_F(CanSilenceBoundTest, TheChargerKeepsTheWhole60sWindow) {
  user_selected_charger_type = ChargerType::ChevyVolt;
  setup_charger();
  ASSERT_NE(charger, nullptr);
  charger_detected = true;
  datalayer.charger.CAN_charger_still_alive = CAN_STILL_ALIVE;
  for (int s = 0; s < CAN_STILL_ALIVE; s++) {
    datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;
    update_machineryprotection();
  }
  EXPECT_EQ(state(EVENT_CAN_CHARGER_MISSING), EVENT_STATE_INACTIVE);
  update_machineryprotection();
  EXPECT_EQ(state(EVENT_CAN_CHARGER_MISSING), EVENT_STATE_ACTIVE);
}
