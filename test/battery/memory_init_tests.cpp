#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/battery/ECMP-BATTERY.h"
#include "../../Software/src/battery/IMIEV-CZERO-ION-BATTERY.h"
#include "../../Software/src/battery/ORION-BMS.h"
#include "../../Software/src/datalayer/datalayer.h"

// wq309: memory-safety defects in battery drivers.
//
// The uninitialised-array tests use a poisoned-heap technique: allocate and
// 0xAB-fill blocks of the exact object sizes, free them, then let
// setup_battery() allocate the driver - glibc hands back the just-freed block
// of the same size class, so a member array without an initialiser starts as
// 0xABAB... instead of the zeros a fresh page would fake. Without the poison,
// an uninitialised array often reads 0 by accident and the defect hides.

namespace {

// A frame for ORION's 0x36 cell broadcast: cell id, voltage in 0.1 mV units.
CAN_frame orion_cell_frame(uint8_t cell_id, uint16_t voltage_tenths_mV) {
  CAN_frame frame = {};
  frame.ID = 0x36;
  frame.DLC = 8;
  frame.data.u8[0] = cell_id;
  frame.data.u8[1] = (uint8_t)(voltage_tenths_mV >> 8);
  frame.data.u8[2] = (uint8_t)(voltage_tenths_mV & 0xFF);
  return frame;
}

// Blocks kept alive across the poisoned free()s so freed neighbours cannot
// coalesce into larger chunks (which breaks exact-size reuse for objects past
// the tcache range - IMIEV's ~1 KB object needed this).
std::vector<void*> poison_holds;

void poison_heap_for(size_t object_size) {
  // Poison a spread of sizes around the object so the allocator's size-class
  // rounding still lands the next allocation of object_size on dirty memory.
  // Pairs of blocks per size: one is freed poisoned, its neighbour stays
  // allocated until after setup_battery() so the freed one cannot coalesce.
  constexpr int kSizes = 48;
  constexpr int kRounds = 4;
  for (int r = 0; r < kRounds; r++) {
    for (int i = 0; i < kSizes; i++) {
      size_t sz = object_size + (size_t)(i - 8) * 16;
      if ((ssize_t)sz <= 0) {
        continue;
      }
      void* victim = malloc(sz);
      void* hold = malloc(32);
      if (victim != nullptr) {
        memset(victim, 0xAB, sz);
        free(victim);
      }
      if (hold != nullptr) {
        poison_holds.push_back(hold);
      }
    }
  }
}

void release_poison_holds() {
  for (void* p : poison_holds) {
    free(p);
  }
  poison_holds.clear();
}

CanBattery* make_battery(BatteryType type, size_t poison_size) {
  user_selected_battery_type = type;
  poison_heap_for(poison_size);
  setup_battery();
  release_poison_holds();
  return dynamic_cast<CanBattery*>(::battery);
}

// --- ORION-BMS: the out-of-bounds write, fixed by REJECTING the frame -------
// The defect: an over-range cell id was clamped TO MAX_AMOUNT_CELLS and then
// written, one past the end of cellvoltages[MAX_AMOUNT_CELLS]. The recorded
// fix decision (wq309): REJECT the frame rather than clamp to the last cell -
// a corrupted or mis-configured id must not overwrite a real cell's reading,
// and the sticky amount_of_detected_cells it fed must not jump to a full pack.
//
// Observable pin: pre-fix, the id-200 frame drove amount_of_detected_cells to
// 192, and because the update guard is `< MAX_AMOUNT_CELLS`, number_of_cells
// then FROZE at its old value forever - the later, valid id-100 frame could
// never surface. Post-fix the id-200 frame changes nothing and id 100 lands.

TEST(DriverMemoryInitTest, OrionOverRangeCellIdFrameIsRejected) {
  CanBattery* bat = make_battery(BatteryType::OrionBms, sizeof(OrionBms));
  ASSERT_NE(bat, nullptr);

  bat->handle_incoming_can_frame(orion_cell_frame(5, 36000));
  bat->handle_incoming_can_frame(orion_cell_frame(200, 12345));
  bat->handle_incoming_can_frame(orion_cell_frame(100, 37000));
  bat->update_values();

  EXPECT_EQ(datalayer.battery.info.number_of_cells, 100)
      << "an over-range id must be dropped, not clamped into the array and the sticky cell count";
}

// This test passed even before the fix: `new OrionBms()` value-initializes
// (no user-provided ctor) so the array started zeroed by the shape of the
// allocation expression, not by design. It is a tripwire: adding any
// constructor to OrionBms without the `= {0}` initialiser trips it under the
// heap poison (mutation-checked in wq309).
TEST(DriverMemoryInitTest, OrionCellvoltagesStartZeroedNotHeapGarbage) {
  CanBattery* bat = make_battery(BatteryType::OrionBms, sizeof(OrionBms));
  ASSERT_NE(bat, nullptr);

  // No cell frames at all - update_values() memcpys the whole array into the
  // datalayer and min/max-scans it regardless.
  bat->update_values();

  for (int i = 0; i < MAX_AMOUNT_CELLS; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
}

// --- ECMP: cellvoltages[108] had no initialiser -----------------------------
// The first 0x6FF frame fills cells 104-107 and memcpys ALL 108 entries into
// the datalayer, so cells 0-103 surface whatever the heap held. This is the
// one of the three that was LIVE garbage before the fix: EcmpBattery's
// user-provided constructors defeat the value-initialization that protects
// ORION and IMIEV, so the poison surfaced in the datalayer verbatim.

TEST(DriverMemoryInitTest, EcmpCellvoltagesStartZeroedNotHeapGarbage) {
  CanBattery* bat = make_battery(BatteryType::StellantisEcmp, sizeof(EcmpBattery));
  ASSERT_NE(bat, nullptr);

  CAN_frame frame = {};
  frame.ID = 0x6FF;
  frame.DLC = 8;
  for (int i = 0; i < 8; i++) {
    frame.data.u8[i] = 0x0E;  // cells 104-107 become 0x0E0E = 3598 mV
  }
  bat->handle_incoming_can_frame(frame);

  for (int i = 0; i < 104; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
  EXPECT_EQ(datalayer.battery.status.cell_voltages_mV[104], 3598);
  EXPECT_EQ(datalayer.battery.status.cell_voltages_mV[107], 3598);
}

// --- IMIEV: cell_temperatures[88] AND cell_voltages[88] had no initialiser --
// (the row named the temperatures; the voltages sit on the adjacent line with
// the same defect and the voltage copy into the datalayer is ungated, so both
// are fixed and the voltages carry the deterministic assert).
//
// Like ORION below, this test PASSED before the fix: `new ImievCZeroIonBattery()`
// value-initializes because the class has no user-provided constructor, so the
// arrays start zeroed by that accident of the allocation expression. The
// explicit `= {0}` and this tripwire exist for the day someone adds a
// constructor - which is precisely how ECMP's identical array went live-garbage.

TEST(DriverMemoryInitTest, ImievCellArraysStartZeroedNotHeapGarbage) {
  CanBattery* bat = make_battery(BatteryType::ImievCZeroIon, sizeof(ImievCZeroIonBattery));
  ASSERT_NE(bat, nullptr);

  // No frames - update_values() copies cell_voltages * 1000 unconditionally.
  bat->update_values();

  for (int i = 0; i < 88; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
  // Zeroed temperatures pass the "> -49" plausibility gate as a real 0.0 C, so
  // the datalayer must show exactly 0 - not a garbage float scaled by 10.
  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, 0);
  EXPECT_EQ(datalayer.battery.status.temperature_min_dC, 0);
}

}  // namespace
