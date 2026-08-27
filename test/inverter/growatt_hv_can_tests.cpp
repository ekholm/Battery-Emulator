#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/GROWATT-HV-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Growatt High Voltage CAN inverter driver.
//
// The driver batches all frames into five groups (case 0..4) that are each
// sent 10 ms apart once per second, and only after the inverter has sent at
// least one 0x3010 heartbeat (inverter_alive gate).
//
// Helper drain_all_batches() calls transmit_can() five times with 11 ms
// increments so that every batch fires exactly once in a single 1 s window.

namespace {

class GrowattHvCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::GrowattHv;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    growatt_hv = static_cast<GrowattHvInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Send the heartbeat that unlocks TX.
  void wake_inverter(unsigned long ms = 1) {
    CAN_frame hb = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x3010, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
    growatt_hv->map_can_frame_to_variable(hb);
  }

  // Drain all five batches within a single 1 s window starting at base_ms.
  // base_ms must be > 1000 so the 1 s trigger fires on the first call.
  void drain_all_batches(unsigned long base_ms = INTERVAL_1_S + 50) {
    for (int i = 0; i < 5; i++) {
      growatt_hv->transmit_can(base_ms + i * 11);
    }
  }

  GrowattHvInverter* growatt_hv = nullptr;
};

}  // namespace

// ---- Startup gate -----------------------------------------------------------

TEST_F(GrowattHvCanInverterTest, StaysSilentUntilHeartbeatReceived) {
  growatt_hv->update_values();
  drain_all_batches();
  EXPECT_TRUE(get_transmitted_frames().empty()) << "Driver must not transmit before the inverter sends 0x3010";
}

TEST_F(GrowattHvCanInverterTest, StartsSendingAfterHeartbeat) {
  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();
  EXPECT_FALSE(get_transmitted_frames().empty());
}

// ---- RX aliveness -----------------------------------------------------------

TEST_F(GrowattHvCanInverterTest, HeartbeatFrame0x3010RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  wake_inverter();
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(GrowattHvCanInverterTest, ControlFrame0x3020RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x3020, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  growatt_hv->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(GrowattHvCanInverterTest, TimeFrame0x3030RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x3030, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  growatt_hv->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(GrowattHvCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x3040, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  growatt_hv->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---- Batch cadence: all five batches appear in one 1 s window ---------------

TEST_F(GrowattHvCanInverterTest, AllBatchFramesTransmittedInOneCycle) {
  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  // Batch 0: 0x3110, 0x3120, 0x3130, 0x3140
  EXPECT_NE(find_frame_with_id(0x3110), nullptr);
  EXPECT_NE(find_frame_with_id(0x3120), nullptr);
  EXPECT_NE(find_frame_with_id(0x3130), nullptr);
  EXPECT_NE(find_frame_with_id(0x3140), nullptr);
  // Batch 1: 0x3150, 0x3160, 0x3170, 0x3180
  EXPECT_NE(find_frame_with_id(0x3150), nullptr);
  EXPECT_NE(find_frame_with_id(0x3160), nullptr);
  EXPECT_NE(find_frame_with_id(0x3170), nullptr);
  EXPECT_NE(find_frame_with_id(0x3180), nullptr);
  // Batch 2: 0x3190, 0x3200, 0x3210, 0x3220
  EXPECT_NE(find_frame_with_id(0x3190), nullptr);
  EXPECT_NE(find_frame_with_id(0x3200), nullptr);
  // Batch 3: 0x3230, 0x3240, 0x3250, 0x3260
  EXPECT_NE(find_frame_with_id(0x3230), nullptr);
  EXPECT_NE(find_frame_with_id(0x3240), nullptr);
  // Batch 4: 0x3270, 0x3280, 0x3290, 0x3F00
  EXPECT_NE(find_frame_with_id(0x3270), nullptr);
  EXPECT_NE(find_frame_with_id(0x3F00), nullptr);
}

// ---- Payload: 0x3110 (charge voltage, currents, status bits) ----------------

TEST_F(GrowattHvCanInverterTest, Frame3110EncodesDesignVoltageAndCurrentsBE) {
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.status.max_charge_current_dA = 250;
  datalayer.battery.status.max_discharge_current_dA = 300;

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3110);
  ASSERT_NE(f, nullptr);
  // Charge voltage = max_design = 4000, big-endian
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 4000u);
  // Charge current limit
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 250u);
  // Discharge current limit
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 300u);
}

TEST_F(GrowattHvCanInverterTest, Frame3110HonoursUserChargeVoltage) {
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 3950;

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3110);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3950u);
}

TEST_F(GrowattHvCanInverterTest, Frame3110StatusBitsSetNoChargeWhenCurrentZero) {
  datalayer.battery.status.max_charge_current_dA = 0;
  datalayer.battery.status.max_discharge_current_dA = 100;
  datalayer.battery.status.reported_soc = 5000;  // not 0 or 10000

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3110);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->data.u8[7] & 0x40) << "Bit6: no-charge flag must be set";
}

TEST_F(GrowattHvCanInverterTest, Frame3110StatusBitsSetNoChargeOnFullSoc) {
  datalayer.battery.status.reported_soc = 10000;
  datalayer.battery.status.max_charge_current_dA = 100;
  datalayer.battery.status.max_discharge_current_dA = 100;

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3110);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->data.u8[7] & 0x40) << "Bit6: no-charge flag must be set at 100% SOC";
}

TEST_F(GrowattHvCanInverterTest, Frame3110StatusBitsSetNoDischargeOnEmptySoc) {
  datalayer.battery.status.reported_soc = 0;
  datalayer.battery.status.max_charge_current_dA = 100;
  datalayer.battery.status.max_discharge_current_dA = 100;

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3110);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->data.u8[7] & 0x20) << "Bit5: no-discharge flag must be set at 0% SOC";
}

// ---- Payload: 0x3130 (pack voltage, current, temp, SOC, SOH) ----------------

TEST_F(GrowattHvCanInverterTest, Frame3130EncodesVoltageCurrentTempSocSoh) {
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-100);  // -10 A
  datalayer.battery.status.temperature_max_dC = 250;
  datalayer.battery.status.reported_soc = 8000;  // 80.00% → 80
  datalayer.battery.status.soh_pptt = 9700;      // 97.00% → 97

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3130);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3700u);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -100);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 250u);
  EXPECT_EQ(f->data.u8[6], 80u);
  EXPECT_EQ(f->data.u8[7], 97u);
}

// ---- Payload: 0x3140 (capacity in 10 mAh units) ----------------------------

TEST_F(GrowattHvCanInverterTest, Frame3140EncodesCapacityIn10mAhUnits) {
  // capacity_remaining_10mAh = Wh * 1000 / voltage_dV
  // 37000 * 1000 / 3700 = 10000
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.status.reported_remaining_capacity_Wh = 37000;
  datalayer.battery.info.reported_total_capacity_Wh = 74000;  // 74000*1000/3700 = 20000

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3140);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 10000u);  // remaining
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 20000u);  // full
}

TEST_F(GrowattHvCanInverterTest, Frame3140FallsBackToSocDerivedCapacityWhenRemainingIsZero) {
  // When reported_remaining_capacity_Wh == 0, driver derives from SOC.
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.status.reported_remaining_capacity_Wh = 0;
  datalayer.battery.info.reported_total_capacity_Wh = 74000;
  datalayer.battery.status.reported_soc = 5000;  // 50.00% → 50
  // full = 74000*1000/3700 = 20000; rem = 20000 * 50 / 100 = 10000

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3140);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 10000u);
}

// ---- Payload: 0x3150 (discharge cutoff voltage, temp, topology) -------------

TEST_F(GrowattHvCanInverterTest, Frame3150EncodesDischargeVoltageAndDefaultTopology) {
  datalayer.battery.info.min_design_voltage_dV = 3000;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  // number_of_cells < 10 → uses hard-coded TOTAL_NUMBER_OF_CELLS=300

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3150);
  ASSERT_NE(f, nullptr);
  // Discharge cutoff = min_design = 3000
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3000u);
  // Total cells fallback = 300
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 300u);
}

TEST_F(GrowattHvCanInverterTest, Frame3150UsesDynamicCellCountWhenProvided) {
  datalayer.battery.info.number_of_cells = 119;  // >= 10 → override fallback

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3150);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 119u);
  // modules_in_series also becomes 1 when using dynamic count
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 1u);
}

// ---- Payload: 0x3190 (cell min/max voltages) --------------------------------

TEST_F(GrowattHvCanInverterTest, Frame3190EncodesCellMinMaxVoltages) {
  datalayer.battery.status.cell_max_voltage_mV = 4150;
  datalayer.battery.status.cell_min_voltage_mV = 3750;

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3190);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[1], f->data.u8[2]), 4150u);
  EXPECT_EQ(u16_be(f->data.u8[3], f->data.u8[4]), 3750u);
}

// ---- Payload: 0x3220 (rated energy in 0.1 kWh units) -----------------------

TEST_F(GrowattHvCanInverterTest, Frame3220EncodesRatedEnergyIn01kWhUnits) {
  // rated energy = Wh / 100 → BE in bytes 5-6
  datalayer.battery.info.reported_total_capacity_Wh = 30000;  // 30000/100 = 300

  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3220);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[5], f->data.u8[6]), 300u);
}

// ---- Payload: 0x3140 manufacturer code ------------------------------------

TEST_F(GrowattHvCanInverterTest, Frame3140ContainsManufacturerAscii) {
  growatt_hv->update_values();
  wake_inverter();
  drain_all_batches();

  const CAN_frame* f = find_last_frame_with_id(0x3140);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[4], 'G');  // MANUFACTURER_ASCII_0 = 0x47
  EXPECT_EQ(f->data.u8[5], 'T');  // MANUFACTURER_ASCII_1 = 0x54
}
