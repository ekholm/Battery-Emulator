#include <gtest/gtest.h>

#include <vector>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/inverter/GROWATT-LV-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"

// Regression tests for the GROWATT-LV 0x314 capacity encoding.
//
// The driver used to compute (Wh / voltage_dV) * 100 - an integer division
// that truncates before the scaling multiply - and then packed the result
// * 100 into a field its own comment documents as 10 mAh units, so the
// transmitted capacity was 10x too high on top of the truncation. The fix
// computes 0.1 Ah as (Wh * 100) / voltage_dV and packs * 10, saturating the
// 16-bit field instead of wrapping it.

// Frame recorder provided by test/emul/can.cpp
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

const CAN_frame* find_frame_with_id(uint32_t id) {
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == id) {
      return &f;
    }
  }
  return nullptr;
}

uint16_t u16_be(uint8_t hi, uint8_t lo) {
  return static_cast<uint16_t>((hi << 8) | lo);
}

class GrowattLvCapacityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already reset the datalayer and deleted the
    // previous inverter instance, so this constructs a fresh GrowattLvInverter.
    user_selected_inverter_protocol = InverterProtocolType::GrowattLv;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    growatt_lv = static_cast<GrowattLvInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Map values and simulate the inverter's periodic 0x301 poll, which makes
  // the driver burst all data frames; return the captured 0x314.
  const CAN_frame* frame_314_after_poll() {
    growatt_lv->update_values();
    CAN_frame poll = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x301, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
    growatt_lv->map_can_frame_to_variable(poll);
    return find_frame_with_id(0x314);
  }

  GrowattLvInverter* growatt_lv = nullptr;
};

}  // namespace

// Unit boundary Wh -> 10 mAh with an exact division, so any scale error shows
// up undiluted: 5120 Wh at 51.2 V is exactly 100.00 Ah = 10000 x 10 mAh.
// The old code transmitted this 10x too high.
TEST_F(GrowattLvCapacityTest, Frame314Encodes100AhExactlyInTenMilliampHourUnits) {
  datalayer.battery.status.voltage_dV = 512;  // 51.2 V
  datalayer.battery.status.reported_remaining_capacity_Wh = 5120;
  datalayer.battery.info.reported_total_capacity_Wh = 5120;

  const CAN_frame* f = frame_314_after_poll();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 10000u);  // remaining
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 10000u);  // full
}

// Unit boundary of the division order: 10000 Wh at 360.0 V is 27.77 Ah.
// Multiplying first gives (10000 * 100) / 3600 = 277 -> 2770 x 10 mAh;
// the old divide-first order truncated 10000 / 3600 to 2 (20.0 Ah).
TEST_F(GrowattLvCapacityTest, Frame314MultipliesBeforeDividingSoTruncationCannotBite) {
  datalayer.battery.status.voltage_dV = 3600;  // 360.0 V
  datalayer.battery.status.reported_remaining_capacity_Wh = 10000;
  datalayer.battery.info.reported_total_capacity_Wh = 10000;

  const CAN_frame* f = frame_314_after_poll();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 2770u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 2770u);
}

// Unit boundary of the 16-bit field: 48000 Wh at 48.0 V is 1000 Ah, above the
// field's 655.35 Ah ceiling. It must saturate at UINT16_MAX, not wrap.
TEST_F(GrowattLvCapacityTest, Frame314SaturatesInsteadOfWrappingAbove655Ah) {
  datalayer.battery.status.voltage_dV = 480;  // 48.0 V
  datalayer.battery.status.reported_remaining_capacity_Wh = 48000;
  datalayer.battery.info.reported_total_capacity_Wh = 48000;

  const CAN_frame* f = frame_314_after_poll();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), UINT16_MAX);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), UINT16_MAX);
}

// Remaining and full must land in their own bytes: every other test in this
// file sets the two capacities equal, so a swap or aliasing of the 0x314
// byte pairs (0-1 vs 2-3) would survive them all. Distinct values pin the
// field positions: 10000 Wh at 50.0 V is 200.00 Ah remaining, 20000 Wh is
// 400.00 Ah full.
TEST_F(GrowattLvCapacityTest, Frame314KeepsRemainingAndFullCapacityInTheirOwnBytes) {
  datalayer.battery.status.voltage_dV = 500;  // 50.0 V
  datalayer.battery.status.reported_remaining_capacity_Wh = 10000;
  datalayer.battery.info.reported_total_capacity_Wh = 20000;

  const CAN_frame* f = frame_314_after_poll();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 20000u);  // remaining, 200.00 Ah
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 40000u);  // full, 400.00 Ah
}

// Unit boundary of the div0 guard: voltage_dV must be strictly above 10 for
// the capacity to update. At exactly 10 the fresh instance's zeros are sent.
TEST_F(GrowattLvCapacityTest, Frame314CapacityNotComputedAtGuardVoltage) {
  datalayer.battery.status.voltage_dV = 10;  // 1.0 V, guard boundary
  datalayer.battery.status.reported_remaining_capacity_Wh = 10000;
  datalayer.battery.info.reported_total_capacity_Wh = 10000;

  const CAN_frame* f = frame_314_after_poll();
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 0u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 0u);
}
