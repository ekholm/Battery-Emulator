#include <gtest/gtest.h>

#include <cstring>
#include <new>

#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/battery/BOLT-AMPERA-BATTERY.h"
#include "../../Software/src/battery/ECMP-BATTERY.h"
#include "../../Software/src/battery/HYUNDAI-IONIQ-28-BATTERY.h"
#include "../../Software/src/battery/IMIEV-CZERO-ION-BATTERY.h"
#include "../../Software/src/battery/KIA-HYUNDAI-64-BATTERY.h"
#include "../../Software/src/battery/ORION-BMS.h"
#include "../../Software/src/battery/SANTA-FE-PHEV-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"

// wq309: memory-safety defects in battery drivers.
//
// The uninitialised-array tests construct the driver by PLACEMENT-NEW in
// DEFAULT-INIT form (`new (buf) T`, no parentheses) on a 0xAB-poisoned
// buffer. That sidesteps two accidents that let the defect hide:
// value-initialization (`new T()` zero-fills a class with no user-provided
// constructor - which is exactly what protects ORION and IMIEV today and
// what ECMP's user-provided constructor defeats), and the allocator handing
// out an incidentally-zero fresh page. A member array without an initialiser
// deterministically starts as 0xABAB... here; one with `= {0}` is zeroed by
// its NSDMI in every constructor under every allocation shape.
// (Constructing on a stack buffer is safe in the host harness: the
// registration calls the CanBattery ctor makes are no-op stubs in
// test/emul/can.cpp.)

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

// Constructs T by default-init placement-new on poisoned storage; the caller
// owns calling reset()/dtor via the returned pointer before buf dies.
template <typename T>
T* poisoned_construct(void* buf, size_t size) {
  // 0x42, not the traditional 0xAB: four 0xAB bytes read as a float are
  // ~1e-12, which truncates to 0 through IMIEV's `voltage * 1000 -> uint16`
  // copy and fakes a pass. 0x42424242 reads as ~48.57f and 0x4242 as 16962,
  // so poison surfaces through float and uint16 arrays alike.
  memset(buf, 0x42, size);
  return new (buf) T;  // no parentheses: default-init, NSDMIs only
}

CanBattery* make_battery(BatteryType type) {
  user_selected_battery_type = type;
  setup_battery();
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
  CanBattery* bat = make_battery(BatteryType::OrionBms);
  ASSERT_NE(bat, nullptr);

  bat->handle_incoming_can_frame(orion_cell_frame(5, 36000));
  bat->handle_incoming_can_frame(orion_cell_frame(200, 12345));
  bat->handle_incoming_can_frame(orion_cell_frame(100, 37000));
  bat->update_values();

  EXPECT_EQ(datalayer.battery.info.number_of_cells, 100)
      << "an over-range id must be dropped, not clamped into the array and the sticky cell count";
}

// Under `new OrionBms()` the factory value-initializes this class today (no
// user-provided ctor), so through the factory the defect cannot surface. The
// default-init construction here is the shape the class is one added
// constructor away from - which is exactly how ECMP's identical array went
// live-garbage - and only the `= {0}` initialiser protects it.
TEST(DriverMemoryInitTest, OrionCellvoltagesStartZeroedNotHeapGarbage) {
  alignas(OrionBms) static unsigned char buf[sizeof(OrionBms)];
  OrionBms* orion = poisoned_construct<OrionBms>(buf, sizeof(buf));

  // No cell frames at all - update_values() memcpys the whole array into the
  // datalayer and min/max-scans it regardless.
  orion->update_values();

  for (int i = 0; i < MAX_AMOUNT_CELLS; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "uninitialised memory surfaced at cell " << i;
  }
  orion->~OrionBms();
}

// --- ECMP: cellvoltages[108] had no initialiser -----------------------------
// The first 0x6FF frame fills cells 104-107 and memcpys ALL 108 entries into
// the datalayer, so cells 0-103 surface whatever the heap held. This is the
// one of the three that was LIVE garbage before the fix: EcmpBattery's
// user-provided constructors defeat the value-initialization that protects
// ORION and IMIEV, so the poison surfaced in the datalayer verbatim.

TEST(DriverMemoryInitTest, EcmpCellvoltagesStartZeroedNotHeapGarbage) {
  alignas(EcmpBattery) static unsigned char buf[sizeof(EcmpBattery)];
  EcmpBattery* bat = poisoned_construct<EcmpBattery>(buf, sizeof(buf));

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
  bat->~EcmpBattery();
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
  alignas(ImievCZeroIonBattery) static unsigned char buf[sizeof(ImievCZeroIonBattery)];
  ImievCZeroIonBattery* bat = poisoned_construct<ImievCZeroIonBattery>(buf, sizeof(buf));

  // No frames - update_values() copies cell_voltages * 1000 unconditionally.
  bat->update_values();

  for (int i = 0; i < 88; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "uninitialised memory surfaced at cell " << i;
  }
  // Zeroed temperatures pass the "> -49" plausibility gate as a real 0.0 C, so
  // the datalayer must show exactly 0 - not a garbage float scaled by 10.
  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, 0);
  EXPECT_EQ(datalayer.battery.status.temperature_min_dC, 0);
  bat->~ImievCZeroIonBattery();
}

}  // namespace

// --- The sweep's four LIVE classes -----------------------------------------
// The review sweep found eight more user-provided-ctor + uninitialised-array
// combinations; per-class liveness sorted them four live (whole-array memcpys
// into the datalayer that can run before frames fill the array) and four
// written-before-read or read-nowhere (commented at their declarations, not
// churned). Each live one gets the poisoned default-init construction and a
// deterministic zero assert through its own publish path.

TEST(DriverMemoryInitTest, BoltAmperaCellblockVoltagesStartZeroedNotHeapGarbage) {
  alignas(BoltAmperaBattery) static unsigned char buf[sizeof(BoltAmperaBattery)];
  BoltAmperaBattery* bat = poisoned_construct<BoltAmperaBattery>(buf, sizeof(buf));

  // update_values() memcpys all 96 cellblock entries unconditionally.
  bat->update_values();

  for (int i = 0; i < 96; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
  bat->~BoltAmperaBattery();
}

TEST(DriverMemoryInitTest, HyundaiIoniq28CellvoltagesStartZeroedNotHeapGarbage) {
  alignas(HyundaiIoniq28Battery) static unsigned char buf[sizeof(HyundaiIoniq28Battery)];
  HyundaiIoniq28Battery* bat = poisoned_construct<HyundaiIoniq28Battery>(buf, sizeof(buf));

  bat->update_values();

  for (int i = 0; i < 96; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
  bat->~HyundaiIoniq28Battery();
}

TEST(DriverMemoryInitTest, KiaHyundai64CellvoltagesStartZeroedNotHeapGarbage) {
  alignas(KiaHyundai64Battery) static unsigned char buf[sizeof(KiaHyundai64Battery)];
  KiaHyundai64Battery* bat = poisoned_construct<KiaHyundai64Battery>(buf, sizeof(buf));

  // The publish path is the POLL_GROUP_5 sixth datarow: a 0x10 header names
  // the group, then the 0x26 row zeroes sub-300 tails and memcpys all 98 -
  // note the <300 filter passes HIGH garbage (0x4242 = 16962 mV), which is
  // exactly why the initialiser matters.
  CAN_frame header = {};
  header.ID = 0x7EC;
  header.DLC = 8;
  header.data.u8[0] = 0x10;
  header.data.u8[3] = 0x01;  // POLL_GROUP_5 = 0x0105
  header.data.u8[4] = 0x05;
  bat->handle_incoming_can_frame(header);

  CAN_frame row = {};
  row.ID = 0x7EC;
  row.DLC = 8;
  row.data.u8[0] = 0x26;
  bat->handle_incoming_can_frame(row);

  for (int i = 0; i < 85; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
  bat->~KiaHyundai64Battery();
}

TEST(DriverMemoryInitTest, SantaFePhevCellvoltagesStartZeroedNotHeapGarbage) {
  alignas(SantaFePhevBattery) static unsigned char buf[sizeof(SantaFePhevBattery)];
  SantaFePhevBattery* bat = poisoned_construct<SantaFePhevBattery>(buf, sizeof(buf));

  // The publish path is the fifth datarow of poll 4: advance poll_data_pid
  // 1->2->3->4 through four 500 ms transmit passes, then inject the 0x25 row.
  for (unsigned long t = 500; t <= 2000; t += 500) {
    bat->transmit_can(t);
  }
  CAN_frame row = {};
  row.ID = 0x7EC;
  row.DLC = 8;
  row.data.u8[0] = 0x25;
  bat->handle_incoming_can_frame(row);

  for (int i = 0; i < 91; i++) {
    ASSERT_EQ(datalayer.battery.status.cell_voltages_mV[i], 0) << "heap garbage surfaced at cell " << i;
  }
  bat->~SantaFePhevBattery();
}
