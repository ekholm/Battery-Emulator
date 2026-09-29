#include <gtest/gtest.h>

#include "../../Software/src/battery/TESLA-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"

// Within 20 mV of the cell voltage limit (4250 mV NCA/NCM, 3650 mV LFP) the Tesla driver allows only a 200 W
// float charge, whatever the SOC. The SOC-based taper follows the SOC, which lags the cells at the top of
// charge; without this clamp a pack reaching its cell limit below the taper band is charged at full power until
// the hard stop at the limit itself.

namespace {

// 0x332 BattBrickMinMax, multiplexer 1: max brick voltage in bits 2..13, min in bits 16..27, both 2 mV/bit.
CAN_frame brick_minmax(uint16_t max_mV, uint16_t min_mV) {
  const uint16_t max_raw = max_mV / 2;
  const uint16_t min_raw = min_mV / 2;
  CAN_frame f = {};
  f.ID = 0x332;
  f.DLC = 8;
  f.data.u8[0] = static_cast<uint8_t>(((max_raw & 0x3F) << 2) | 0x01);
  f.data.u8[1] = static_cast<uint8_t>((max_raw >> 6) & 0x3F);
  f.data.u8[2] = static_cast<uint8_t>(min_raw & 0xFF);
  f.data.u8[3] = static_cast<uint8_t>((min_raw >> 8) & 0x0F);
  return f;
}

uint32_t charge_limit_W(uint16_t cell_max_mV, battery_chemistry_enum chemistry, uint32_t allowed_W = 10000) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  TeslaBattery tesla;
  tesla.setup();
  datalayer.battery.info.chemistry = chemistry;
  datalayer.battery.status.override_charge_power_W = allowed_W;
  tesla.handle_incoming_can_frame(brick_minmax(cell_max_mV, cell_max_mV - 40));
  tesla.update_values();
  return datalayer.battery.status.max_charge_power_W;
}

}  // namespace

TEST(TeslaCellVoltageClamp, NcmWithin20mVOfTheLimitAllowsOnlyFloatPower) {
  EXPECT_EQ(charge_limit_W(4240, battery_chemistry_enum::NCA), 200u);
}

TEST(TeslaCellVoltageClamp, NcmBelowTheWindowKeepsTheAllowedPower) {
  EXPECT_EQ(charge_limit_W(4220, battery_chemistry_enum::NCA), 10000u);
}

TEST(TeslaCellVoltageClamp, LfpWithin20mVOfItsLimitAllowsOnlyFloatPower) {
  EXPECT_EQ(charge_limit_W(3640, battery_chemistry_enum::LFP), 200u);
}

TEST(TeslaCellVoltageClamp, LfpBelowTheWindowKeepsTheAllowedPower) {
  EXPECT_EQ(charge_limit_W(3620, battery_chemistry_enum::LFP), 10000u);
}

TEST(TeslaCellVoltageClamp, TheClampNeverRaisesALowerLimit) {
  EXPECT_EQ(charge_limit_W(4240, battery_chemistry_enum::NCA, 100), 100u);
}

// The window edge: the clamp starts above limit - 20 mV (0x332 has 2 mV steps), not at it.
TEST(TeslaCellVoltageClamp, NcmAtTheWindowEdgeKeepsTheAllowedPower) {
  EXPECT_EQ(charge_limit_W(4230, battery_chemistry_enum::NCA), 10000u);
}

TEST(TeslaCellVoltageClamp, NcmOneStepInsideTheWindowAllowsOnlyFloatPower) {
  EXPECT_EQ(charge_limit_W(4232, battery_chemistry_enum::NCA), 200u);
}

TEST(TeslaCellVoltageClamp, LfpAtTheWindowEdgeKeepsTheAllowedPower) {
  EXPECT_EQ(charge_limit_W(3630, battery_chemistry_enum::LFP), 10000u);
}

TEST(TeslaCellVoltageClamp, LfpOneStepInsideTheWindowAllowsOnlyFloatPower) {
  EXPECT_EQ(charge_limit_W(3632, battery_chemistry_enum::LFP), 200u);
}
