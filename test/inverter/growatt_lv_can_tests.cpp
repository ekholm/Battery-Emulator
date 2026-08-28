#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/GROWATT-LV-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Growatt Low Voltage (48V) CAN inverter driver.
//
// This driver uses an inverter-polls pattern: the inverter sends 0x301 every
// second, and that triggers the BMS to burst all data frames.  transmit_can()
// is a no-op; there is no periodic self-initiated TX.
//
// NOTE on capacity calculation: since fix/growatt-lv-capacity (wq298) the
// driver computes ampere-hours as 100 * Wh / voltage_dV (multiply before
// divide, uint32 intermediate) and packs the frame field as ampere_hours * 10,
// giving the 10 mAh unit the format wants. The old order of operations was
// both 10x too large and truncating; growatt_lv_capacity_tests.cpp carries the
// unit-boundary and saturation coverage that came with the fix.

namespace {

class GrowattLvCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::GrowattLv;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    growatt_lv = static_cast<GrowattLvInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Simulate the inverter sending its periodic poll (0x301).
  void inverter_poll() {
    CAN_frame poll = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x301, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
    growatt_lv->map_can_frame_to_variable(poll);
  }

  GrowattLvInverter* growatt_lv = nullptr;
};

}  // namespace

// ---- Periodic TX gate -------------------------------------------------------

TEST_F(GrowattLvCanInverterTest, TransmitCanIsNoOp) {
  growatt_lv->update_values();
  growatt_lv->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "GROWATT-LV must not transmit periodically; data is only sent in response to 0x301";
}

// ---- RX aliveness and dispatch ----------------------------------------------

TEST_F(GrowattLvCanInverterTest, Poll0x301RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  inverter_poll();
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(GrowattLvCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x302, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  growatt_lv->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(GrowattLvCanInverterTest, Poll0x301SendsAllDataFrames) {
  growatt_lv->update_values();
  inverter_poll();
  EXPECT_NE(find_frame_with_id(0x311), nullptr);
  EXPECT_NE(find_frame_with_id(0x312), nullptr);
  EXPECT_NE(find_frame_with_id(0x313), nullptr);
  EXPECT_NE(find_frame_with_id(0x314), nullptr);
  EXPECT_NE(find_frame_with_id(0x315), nullptr);
  EXPECT_NE(find_frame_with_id(0x316), nullptr);
  EXPECT_NE(find_frame_with_id(0x317), nullptr);
  EXPECT_NE(find_frame_with_id(0x318), nullptr);
  EXPECT_NE(find_frame_with_id(0x319), nullptr);
  EXPECT_NE(find_frame_with_id(0x320), nullptr);
  EXPECT_NE(find_frame_with_id(0x321), nullptr);
}

// ---- Payload: 0x311 (charge voltage, charge/discharge limits, status bits) --

TEST_F(GrowattLvCanInverterTest, Frame311EncodesChargeVoltageMinus40Offset) {
  datalayer.battery.info.max_design_voltage_dV = 4000;  // charge voltage = 4000 - 40 = 3960

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x311);
  ASSERT_NE(f, nullptr);
  // 3960 = 0x0F78, big-endian
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3960u);
}

TEST_F(GrowattLvCanInverterTest, Frame311EncodesChargeAndDischargeCurrentLimitsBE) {
  datalayer.battery.status.max_charge_current_dA = 150;
  datalayer.battery.status.max_discharge_current_dA = 250;

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x311);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 150u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 250u);
}

TEST_F(GrowattLvCanInverterTest, Frame311StatusBitsReflectActivePower) {
  growatt_lv->update_values();

  // Idle (active_power_W = 0 by default)
  inverter_poll();
  const CAN_frame* f = find_frame_with_id(0x311);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x04u);  // 0b01 idle on bit10-11
  EXPECT_EQ(f->data.u8[7], 0x01u);  // 0b01 idle on bit0-1

  clear_transmitted_frames();
  datalayer.battery.status.active_power_W = -1000;  // Discharging
  growatt_lv->update_values();
  inverter_poll();
  f = find_frame_with_id(0x311);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x0Cu);  // 0b11 discharging
  EXPECT_EQ(f->data.u8[7], 0x03u);

  clear_transmitted_frames();
  datalayer.battery.status.active_power_W = 1000;  // Charging
  growatt_lv->update_values();
  inverter_poll();
  f = find_frame_with_id(0x311);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x08u);  // 0b10 charging
  EXPECT_EQ(f->data.u8[7], 0x02u);
}

// ---- Payload: 0x313 (voltage ×10, current, temp, SOC, SOH) -----------------

TEST_F(GrowattLvCanInverterTest, Frame313EncodesVoltageTimes10InBE) {
  datalayer.battery.status.voltage_dV = 3700;  // * 10 = 37000 = 0x9088

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x313);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 37000u);
}

TEST_F(GrowattLvCanInverterTest, Frame313EncodesSignedCurrentBE) {
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-200);  // -20.0 A

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x313);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -200);
}

TEST_F(GrowattLvCanInverterTest, Frame313EncodesSocAndSohAsIntegerPercent) {
  datalayer.battery.status.reported_soc = 8000;  // 80.00% → 80
  datalayer.battery.status.soh_pptt = 9500;      // 95.00% → 95

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x313);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 80u);
  EXPECT_EQ(f->data.u8[7], 95u);
}

// ---- Payload: 0x314 (capacity, delta V) ------------------------------------

TEST_F(GrowattLvCanInverterTest, Frame314EncodesCapacityIn10mAhUnits) {
  // Post-fix (wq298): ampere_hours = 100 * Wh / voltage_dV, packed * 10.
  // 3600 Wh at 360.0 V is 10.0 Ah -> 1000 in the 10 mAh field.
  datalayer.battery.status.voltage_dV = 3600;  // must be >10 to update
  datalayer.battery.status.reported_remaining_capacity_Wh = 3600;
  datalayer.battery.info.reported_total_capacity_Wh = 3600;

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x314);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 1000u);  // remaining, 10.00 Ah
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 1000u);  // full, 10.00 Ah
}

TEST_F(GrowattLvCanInverterTest, Frame314EncodesCellDeltaVoltage) {
  datalayer.battery.status.cell_max_voltage_mV = 4100;
  datalayer.battery.status.cell_min_voltage_mV = 3900;  // delta = 200 mV

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x314);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 200u);
}

// ---- Payload: 0x319 (charge/discharge enable, cell voltages) ----------------

TEST_F(GrowattLvCanInverterTest, Frame319EnablesChargeAndDischargeWhenActive) {
  datalayer.system.status.system_status = ACTIVE;

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x319);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0xC0u);  // bit7=charge enable, bit6=discharge enable
}

TEST_F(GrowattLvCanInverterTest, Frame319DisablesChargeDischargeWhenNotActive) {
  datalayer.system.status.system_status = FAULT;

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x319);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u);
}

// ---- Payload: cell voltages in 0x315..0x318 ---------------------------------

TEST_F(GrowattLvCanInverterTest, Frame315EncodesCellVoltages1Through4) {
  datalayer.battery.status.cell_voltages_mV[0] = 3700;
  datalayer.battery.status.cell_voltages_mV[1] = 3750;
  datalayer.battery.status.cell_voltages_mV[2] = 3800;
  datalayer.battery.status.cell_voltages_mV[3] = 3850;

  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x315);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3700u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 3750u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 3800u);
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 3850u);
}

// ---- Payload: 0x320 (manufacturer identifier) -------------------------------

TEST_F(GrowattLvCanInverterTest, Frame320ContainsBEManufacturerAscii) {
  growatt_lv->update_values();
  inverter_poll();

  const CAN_frame* f = find_frame_with_id(0x320);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 'B');  // 0x42
  EXPECT_EQ(f->data.u8[1], 'E');  // 0x45
}
