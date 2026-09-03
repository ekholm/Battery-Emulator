#include <gtest/gtest.h>

#include "../../Software/src/battery/ENNOID-BMS.h"
#include "../../Software/src/battery/IMIEV-CZERO-ION-BATTERY.h"
#include "../../Software/src/battery/RANGE-ROVER-PHEV-BATTERY.h"
#include "../../Software/src/battery/RELION-LV-BATTERY.h"
#include "../../Software/src/battery/TESLA-LEGACY-BATTERY.h"
#include "../../Software/src/battery/VOLVO-SPA-HYBRID-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"

#include "Arduino.h"

/* decode arithmetic, evidence first.
 *
 * Every test here either proves a FIX whose evidence is named at the fix site
 * (the driver's own declaration, its own guards, its own captured frames, the
 * datalayer's documented units, or plain C++ semantics), or pins CURRENT
 * BEHAVIOUR for a suspicion no trace, datasheet or sibling settles - the
 * withdrawn BMW-PHEV DTC claim is why the second kind changes nothing.
 */
namespace {

CAN_frame frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
  CAN_frame f = {};
  f.DLC = 8;
  f.ID = id;
  uint8_t i = 0;
  for (uint8_t b : bytes) {
    if (i >= 8) {
      break;
    }
    f.data.u8[i++] = b;
  }
  return f;
}

void reset_battery_state() {
  datalayer.battery.status = {};
  datalayer.battery.info = {};
}

}  // namespace

// FIX, evidence: the driver's own declaration - "(0 - 16777215) Scaling: 0.025
// Offset: -209715.175" is a 24-bit range whose midpoint offset only a 24-bit
// assembly can reach. The old expression shifted BOTH high bytes by 8, so
// bytes 5 and 6 collided in one lane: 0x800100 decoded as 0x8100 = 33024,
// i.e. -208889 dA out of a field whose whole point is a +/-209715 span.
TEST(RangeRoverPhevDecode, CurrentIsAssembledFromAllThreeBytes) {
  reset_battery_state();
  RangeRoverPhevBattery battery;

  // 0x800100 = 8388864. * 0.025 - 209715 = 6.6 -> 6 dA.
  battery.handle_incoming_can_frame(frame(0x102, {0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.current_dA, 6);
}

// FIX, evidence: C++ semantics alone. The decode is (raw * 0.5) - 40, whose
// own range is -40..+87.5 C, and the intermediates were uint8_t: raw 8 means
// -36 C, which wrapped to 220 and reached the datalayer as +2200 dC.
TEST(TeslaLegacyDecode, SubzeroBrickTemperaturesStaySubzero) {
  reset_battery_state();
  TeslaLegacyBattery battery;

  // u8[3] = 8 -> TMax = -36 C; u8[7] = 6 -> TMin = -37 C.
  battery.handle_incoming_can_frame(frame(0x332, {0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, -360);
  EXPECT_EQ(datalayer.battery.status.temperature_min_dC, -370);
}

// CURRENT BEHAVIOUR, pinned and NOT changed: hwID 79/89 carry the comment
// "100kWh" and set 70000 Wh. One of the two is wrong - every other group in
// the switch has comment == value (60/70/75/85/90) - but nothing in the tree,
// the originating commit (d242d4cd, message only), or public pack tables
// settles WHICH, and 79/89 do not appear in any second in-repo mapping. A
// driver nobody can test is not improved by a confident guess; whoever has a
// 100 kWh legacy pack can settle it in one boot.
TEST(TeslaLegacyDecode, HwId79SetsSeventyKwhDespiteItsHundredKwhLabel) {
  reset_battery_state();
  TeslaLegacyBattery battery;

  // 0x5D2 with u8[0] == 0x0A: hwID = u8[4] + u8[5].
  battery.handle_incoming_can_frame(frame(0x5D2, {0x0A, 0x00, 0x00, 0x00, 0x4F, 0x00, 0x00, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.info.total_capacity_Wh, 70000u);
}

// FIX, evidence: the driver's own guards. Channels 2 and 3 GUARD on u8[2] and
// u8[3] and then read u8[1] - the guard names the byte the author meant. One
// frame with three distinct sensors used to store channel 1's value three
// times, and the pack maximum could hide a hot sensor entirely.
TEST(ImievDecode, EachTemperatureChannelReadsItsOwnByte) {
  reset_battery_state();
  ImievCZeroIonBattery battery;

  // 0x6e1, cmu 1, pid 0: temps 10 / 20 / 40 C. pid 0 stores channels 2 and 3,
  // so the array holds 20 and 40, and the maximum is 40 C - the old code
  // mirrored channel 1 and reported 10 C.
  battery.handle_incoming_can_frame(frame(0x6E1, {0x01, 0x3C, 0x46, 0x5A, 0x00, 0x00, 0x00, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, 400);
}

// Note: the frame above pins channel 3 (it carries the maximum there), but a
// temp2-only revert to u8[1] survived it - channel 3 still supplied the 40 C.
// Here channel 2 alone carries the maximum, so each channel is now pinned
// independently.
TEST(ImievDecode, Channel2AloneCanCarryTheMaximum) {
  reset_battery_state();
  ImievCZeroIonBattery battery;

  // temps 10 / 40 / 20 C: the maximum sits in u8[2], channel 2's own byte.
  battery.handle_incoming_can_frame(frame(0x6E1, {0x01, 0x3C, 0x5A, 0x46, 0x00, 0x00, 0x00, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, 400) << "channel 2 mirroring channel 1 would report 20 C here";
}

// FIX, evidence: float semantics alone. 0.005f and 2.1f are not exact in
// binary, so a cell the decode means as 3.700 V arrives as 3.69999...f, and
// the truncating cast published 3699 mV. Round, don't truncate.
TEST(ImievDecode, CellVoltagesRoundToTheMillivoltTheDecodeMeans) {
  reset_battery_state();
  ImievCZeroIonBattery battery;

  // Fill the whole 88-cell pack at 3.700 V (raw 320) so the min scan has no
  // unreported zeros to trip its own guard on, then put one cell at 3.500 V
  // (raw 280).
  for (uint8_t cmu = 1; cmu <= 12; cmu++) {
    for (uint8_t pid = 0; pid < 4; pid++) {
      battery.handle_incoming_can_frame(frame(0x6E1 + pid, {cmu, 0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x40}));
    }
  }
  battery.handle_incoming_can_frame(frame(0x6E1, {0x01, 0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x18}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.cell_max_voltage_mV, 3700u);
  EXPECT_EQ(datalayer.battery.status.cell_min_voltage_mV, 3500u);
  EXPECT_EQ(datalayer.battery.status.cell_voltages_mV[0], 3700u);
  EXPECT_EQ(datalayer.battery.status.cell_voltages_mV[1], 3500u);
}

// FIX, evidence: the case's own captured frame plus the decoded-but-unwired
// variable. min_cell_temperature existed and nothing read it, so both
// datalayer fields carried the maximum; and its decode read u8[2], an id byte
// per the captured "47 01 01 47 01 01" - two (value, id, id) triplets, the
// same shape the driver's own ID3 uses for cell voltages - which would have
// been a constant 0x01 - 50 = -49 C.
TEST(RelionDecode, MinimumTemperatureComesFromItsOwnValueByte) {
  reset_battery_state();
  RelionBattery battery;

  // max = 0x47 - 50 = 21 C, min = 0x3C - 50 = 10 C.
  battery.handle_incoming_can_frame(frame(0x02048100, {0x47, 0x01, 0x01, 0x3C, 0x01, 0x01, 0x00, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, 210);
  EXPECT_EQ(datalayer.battery.status.temperature_min_dC, 100);
}

// CURRENT BEHAVIOUR, pinned and NOT changed: 0x02648100 reads the discharge
// current from u8[2..3] - the same bytes as the regen charge current - and
// u8[4..5] go unread. No captured frame or datasheet in the tree names the
// discharge bytes, and the only consumer (the power calculation) is commented
// out ("Shows 0A all the time?"), so today the duplication is dead code: the
// frame must not move the discharge limit, which stays the user override.
TEST(RelionDecode, ChargingLimitsFrameDoesNotMoveTheDischargeLimit) {
  reset_battery_state();
  RelionBattery battery;
  datalayer.battery.status.override_discharge_power_W = 4321;
  datalayer.battery.status.cell_min_voltage_mV = 3300;  // above the low cutoff

  battery.handle_incoming_can_frame(frame(0x02648100, {0x03, 0x84, 0x03, 0x52, 0x04, 0xB0, 0x00, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 4321u);
}

// FIX, evidence: the datalayer's documented unit ("150 = 15.0 C") and the
// max >= min invariant. The protocol reports only the high temperature; the
// old lines put it in the MIN field, made the max one degree COLDER, and
// skipped the x10 - 21 C displayed as 2.1 C with max < min. The author's
// synthetic 1-degree spread is kept, ordered and scaled.
TEST(EnnoidDecode, TemperaturesAreOrderedAndInDeciCelsius) {
  reset_battery_state();
  EnnoidBms battery;

  battery.handle_incoming_can_frame(frame(0x2d0a, {0x0C, 0xE4, 0x0E, 0x74, 0x00, 0x00, 21, 0x00}));
  battery.update_values();

  EXPECT_EQ(datalayer.battery.status.temperature_max_dC, 210);
  EXPECT_EQ(datalayer.battery.status.temperature_min_dC, 200);
  EXPECT_GE(datalayer.battery.status.temperature_max_dC, datalayer.battery.status.temperature_min_dC);
}

// CURRENT BEHAVIOUR, pinned and NOT changed: 0x369's low byte is read from
// u8[6] - the byte that also supplies the two high bits - so bits 8-9 of the
// result duplicate bits 0-1 and u8[7] is never read. The SIBLING driver
// (VOLVO-SPA-BATTERY.cpp:203) has the IDENTICAL expression, so no sibling
// settles the true layout, no trace in the tree carries a 0x369, and the field
// reaches only the extended-info page. 0xAB gives (3 << 8 | 0xAB) >> 2 = 234.
TEST(VolvoSpaHybridDecode, PowerLimitLowByteStillComesFromByteSix) {
  reset_battery_state();
  datalayer_extended.VolvoHybrid.HvBattPwrLimDchaSoft = 0;
  VolvoSpaHybridBattery battery;

  battery.handle_incoming_can_frame(frame(0x369, {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAB, 0xCD}));
  battery.update_values();

  EXPECT_EQ(datalayer_extended.VolvoHybrid.HvBattPwrLimDchaSoft, 234u);
}
