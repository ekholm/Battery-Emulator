#include <gtest/gtest.h>

#include "../../Software/src/battery/TESLA-LEGACY-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/utils/events.h"

#include "Arduino.h"

/* The legacy Tesla driver derives pack capacity from the hwID it reads off
 * 0x5D2 (mux 0x0A: hwID = u8[4] + u8[5]). Every hwID group in the switch is
 * labelled with the capacity it sets, and this table is that labelling. The
 * 100 kWh group (79, 89) used to set 70000, the value of the 70 kWh group,
 * which no other group did; it now sets what its label says.
 */
namespace {

CAN_frame hwid_frame(uint8_t hwid) {
  CAN_frame f = {};
  f.DLC = 8;
  f.ID = 0x5D2;
  f.data.u8[0] = 0x0A;
  f.data.u8[4] = hwid;
  return f;
}

void reset_battery_state() {
  datalayer.battery.status = {};
  datalayer.battery.info = {};
}

uint32_t capacity_for_hwid(uint8_t hwid) {
  reset_battery_state();
  TeslaLegacyBattery battery;
  battery.handle_incoming_can_frame(hwid_frame(hwid));
  battery.update_values();
  return datalayer.battery.info.total_capacity_Wh;
}

struct HwIdCapacity {
  uint8_t hwid;
  uint32_t capacity_Wh;
};

constexpr uint8_t kUnlistedHwId = 1;  // in no group of the switch

}  // namespace

TEST(TeslaLegacyCapacity, EveryHwIdGroupSetsTheCapacityItIsLabelledWith) {
  const HwIdCapacity table[] = {
      {27, 60000},  {39, 60000},  {42, 60000}, {50, 60000},                            // 60kWh
      {61, 70000},  {65, 70000},  {67, 70000},                                         // 70kWh
      {74, 75000},  {80, 75000},  {84, 75000},                                         // 75kWh
      {24, 85000},  {31, 85000},  {41, 85000}, {46, 85000}, {49, 85000}, {57, 85000},  // 85kWh
      {60, 85000},  {64, 85000},  {69, 85000}, {70, 85000}, {86, 85000},               //
      {68, 90000},  {71, 90000},  {73, 90000}, {75, 90000}, {76, 90000}, {77, 90000},  // 90kWh
      {81, 90000},  {82, 90000},  {83, 90000}, {85, 90000}, {94, 90000},               //
      {79, 100000}, {89, 100000},                                                      // 100kWh
  };
  for (const auto& row : table) {
    EXPECT_EQ(capacity_for_hwid(row.hwid), row.capacity_Wh) << "hwID " << int(row.hwid);
  }
}

TEST(TeslaLegacyCapacity, HwIdZeroLeavesCapacityUntouched) {
  // hwID 0 means "not read yet": whatever was loaded before (the stored
  // BATTERY_WH_MAX, or nothing) stays, and it is not reported as an unknown
  // hwID on every boot before the first 0x5D2 arrives.
  reset_battery_state();
  reset_all_events();
  datalayer.battery.info.total_capacity_Wh = 12345;
  TeslaLegacyBattery battery;
  battery.update_values();
  EXPECT_EQ(datalayer.battery.info.total_capacity_Wh, 12345u);
  EXPECT_NE(get_event_pointer(EVENT_BATTERY_VALUE_UNAVAILABLE)->state, EVENT_STATE_ACTIVE);
}

// The other side of the event check above, live in this fixture: an hwID in
// no group does raise the event and leaves the capacity alone.
TEST(TeslaLegacyCapacity, UnlistedHwIdRaisesValueUnavailable) {
  reset_battery_state();
  reset_all_events();
  datalayer.battery.info.total_capacity_Wh = 12345;
  TeslaLegacyBattery battery;
  battery.handle_incoming_can_frame(hwid_frame(kUnlistedHwId));
  battery.update_values();
  EXPECT_EQ(datalayer.battery.info.total_capacity_Wh, 12345u);
  EXPECT_EQ(get_event_pointer(EVENT_BATTERY_VALUE_UNAVAILABLE)->state, EVENT_STATE_ACTIVE);
}

// PIN of current behaviour, not a design claim: once the hwID is known the
// driver rewrites the capacity on every update_values(), so a capacity the
// user stored through the settings page is overwritten, not merely defaulted.
TEST(TeslaLegacyCapacity, KnownHwIdOverwritesAStoredCapacityOnEveryUpdate) {
  reset_battery_state();
  datalayer.battery.info.total_capacity_Wh = 12345;
  TeslaLegacyBattery battery;
  battery.handle_incoming_can_frame(hwid_frame(79));
  battery.update_values();
  EXPECT_EQ(datalayer.battery.info.total_capacity_Wh, 100000u);
  datalayer.battery.info.total_capacity_Wh = 12345;  // what /updateBatterySize does
  battery.update_values();
  EXPECT_EQ(datalayer.battery.info.total_capacity_Wh, 100000u);
}
