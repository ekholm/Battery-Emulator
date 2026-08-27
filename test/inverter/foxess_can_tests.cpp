#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/FOXESS-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the FoxESS HV2600/ECS4100 CAN inverter driver.
//
// The driver is state-machine + flag-driven: RX on 0x1871 sets boolean
// flags (send_bms_info, send_individual_pack_status, send_cellvoltages,
// send_serial_numbers); transmit_can drains those flags in batches.
// NOTE: test/battery/foxess_tests.cpp tests the Foxess BATTERY driver.
// This file tests the FOXESS-CAN INVERTER driver.

namespace {

// Batch delay is 10 ms. Drive time in multiples past that to flush each batch.
static constexpr unsigned long BATCH_MS = 15;  // > delay_between_batches_ms

class FoxessCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::Foxess;
    // Defaults — reset before every test so mutations don't leak.
    user_selected_inverter_foxess_modules = 0;
    user_selected_inverter_foxess_type    = 0;
    user_selected_inverter_foxess_subtype = 0;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    foxess = static_cast<FoxessCanInverter*>(inverter);
    clear_transmitted_frames();
  }

  void TearDown() override {
    user_selected_inverter_foxess_modules = 0;
    user_selected_inverter_foxess_type    = 0;
    user_selected_inverter_foxess_subtype = 0;
  }

  // Inject a 0x1871 frame with given payload bytes.
  void rx1871(uint8_t b0, uint8_t b4 = 0x00) {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1871, .data = {0}};
    f.data.u8[0] = b0;
    f.data.u8[4] = b4;
    foxess->map_can_frame_to_variable(f);
  }

  // Trigger BMS info batch and drive transmit_can to flush both batches.
  void flush_bms_info(unsigned long t_start = 0) {
    rx1871(0x01, 0x00);  // b4=0x00 → send_bms_info
    foxess->transmit_can(t_start + BATCH_MS);
    foxess->transmit_can(t_start + 2 * BATCH_MS);
  }

  FoxessCanInverter* foxess = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// RX / aliveness
// ---------------------------------------------------------------------------

TEST_F(FoxessCanInverterTest, KnownRxFrameRefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  rx1871(0x01, 0x00);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(FoxessCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0xABCD, .data = {0}};
  foxess->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---------------------------------------------------------------------------
// RX dispatch: 0x1871 byte0 and byte4 routing
// ---------------------------------------------------------------------------

TEST_F(FoxessCanInverterTest, Byte0Eq01Byte4Eq00TriggersBmsInfoBatch) {
  foxess->update_values();
  flush_bms_info();
  // BMS info batch: case 0 → 1872..1875, case 1 → 1876..1879
  EXPECT_GT(count_frames_with_id(0x1872), 0u);
  EXPECT_GT(count_frames_with_id(0x1873), 0u);
  EXPECT_GT(count_frames_with_id(0x1874), 0u);
  EXPECT_GT(count_frames_with_id(0x1875), 0u);
  EXPECT_GT(count_frames_with_id(0x1876), 0u);
  EXPECT_GT(count_frames_with_id(0x1877), 0u);
  EXPECT_GT(count_frames_with_id(0x1878), 0u);
  EXPECT_GT(count_frames_with_id(0x1879), 0u);
}

TEST_F(FoxessCanInverterTest, Byte0Eq01Byte4Eq01TriggersIndividualPackBatch) {
  foxess->update_values();
  rx1871(0x01, 0x01);
  foxess->transmit_can(BATCH_MS);
  foxess->transmit_can(2 * BATCH_MS);
  EXPECT_GT(count_frames_with_id(0x0C05), 0u);
  EXPECT_GT(count_frames_with_id(0x0C06), 0u);
  EXPECT_GT(count_frames_with_id(0x0C07), 0u);
  EXPECT_GT(count_frames_with_id(0x0C08), 0u);
  EXPECT_GT(count_frames_with_id(0x0C09), 0u);
}

TEST_F(FoxessCanInverterTest, Byte0Eq05TriggersSerialNumbers) {
  rx1871(0x05);
  // Drive enough batches: slot 0 = master, slots 1-8 for modules.
  // Default 8 modules: 9 * 3 frames = 27 send operations across 9 batches.
  for (int i = 0; i < 10; i++) {
    foxess->transmit_can((i + 1) * BATCH_MS);
  }
  // Slot 0 (master) always sent.
  EXPECT_GT(count_frames_with_id(0x1881), 0u);
  EXPECT_GT(count_frames_with_id(0x1882), 0u);
  EXPECT_GT(count_frames_with_id(0x1883), 0u);
}

TEST_F(FoxessCanInverterTest, Byte0Eq04TriggersCellVoltagesBatch) {
  foxess->update_values();
  rx1871(0x01, 0x04);
  // Cell voltage batch has 9 cases (0-8). Drive them all.
  for (int i = 0; i < 10; i++) {
    foxess->transmit_can((i + 1) * BATCH_MS);
  }
  // Cell frames use variable IDs. We just check that something was sent.
  EXPECT_GT(get_transmitted_frames().size(), 0u);
}

// ---------------------------------------------------------------------------
// TX payload encoding: update_values → transmit via flush_bms_info
// ---------------------------------------------------------------------------

TEST_F(FoxessCanInverterTest, LimitsFrameEncodesVoltageAndCurrentLE) {
  datalayer.battery.info.max_design_voltage_dV      = 4000;
  datalayer.battery.info.min_design_voltage_dV      = 3200;
  datalayer.battery.status.max_charge_current_dA    = 200;
  datalayer.battery.status.max_discharge_current_dA = 300;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1872);
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(f->ext_ID) << "0x1872 must be extended ID";
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4000u) << "max voltage LE b0-1";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 3200u) << "min voltage LE b2-3";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 200u)  << "charge current LE b4-5";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 300u)  << "discharge current LE b6-7";
}

TEST_F(FoxessCanInverterTest, PackDataFrameEncodesVoltageCurrentSocAndRemaining) {
  datalayer.battery.status.voltage_dV                     = 3700;
  datalayer.battery.status.reported_current_dA            = static_cast<int16_t>(-300);  // -30 A
  datalayer.battery.status.reported_soc                   = 6000;  // 60.00 % → byte 60
  datalayer.battery.status.reported_remaining_capacity_Wh = 12000;  // /10 = 1200
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1873);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3700u) << "voltage LE";
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[2], f->data.u8[3])), -300) << "signed current";
  EXPECT_EQ(f->data.u8[4], 60u) << "SOC integer";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 1200u) << "remaining Wh/10 LE";
}

TEST_F(FoxessCanInverterTest, CellDataFrameEncodesTemperaturesAndTweakedVoltages) {
  // LFP chemistry: cell voltages pass through unchanged.
  datalayer.battery.info.chemistry               = battery_chemistry_enum::LFP;
  datalayer.battery.status.temperature_max_dC    = 350;
  datalayer.battery.status.temperature_min_dC    = static_cast<int16_t>(-100); // -10 °C
  datalayer.battery.status.cell_max_voltage_mV   = 3400;
  datalayer.battery.status.cell_min_voltage_mV   = 3300;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1874);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[0], f->data.u8[1])), 350) << "temp max LE signed";
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[2], f->data.u8[3])), -100) << "temp min LE negative";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 3400u) << "cell max LE (LFP pass-through)";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 3300u) << "cell min LE (LFP pass-through)";
}

TEST_F(FoxessCanInverterTest, NonLfpChemistryCellVoltagesAreRescaled) {
  // Non-LFP: [2500-4200] → [2500-3400]. Cell at 4200 should map to 3400.
  datalayer.battery.info.chemistry             = battery_chemistry_enum::NCA;
  datalayer.battery.status.cell_max_voltage_mV = 4200;
  datalayer.battery.status.cell_min_voltage_mV = 2500;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1874);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 3400u) << "NCA max rescaled to 3400";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 2500u) << "NCA min stays at 2500";
}

TEST_F(FoxessCanInverterTest, StatusFrameEncodesAverageTemperatureAndModuleCount) {
  datalayer.battery.status.temperature_max_dC = 280;
  datalayer.battery.status.temperature_min_dC = 220;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1875);
  ASSERT_NE(f, nullptr);
  // Average = (280+220)/2 = 250
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 250u) << "temp average b0-1";
  // b3 = configured_number_of_modules (default 8)
  EXPECT_EQ(f->data.u8[3], 8u) << "module count b3";
  // b4 = contactor status = 1 (always on in FoxESS)
  EXPECT_EQ(f->data.u8[4], 1u) << "contactor status b4 always 1";
}

TEST_F(FoxessCanInverterTest, PackTempsFrameChargeNotAllowedBitWhenCurrentZero) {
  // Charge not allowed flag in 0x1876 b0 bit0.
  datalayer.battery.status.max_charge_current_dA    = 0;
  datalayer.battery.status.max_discharge_current_dA = 100;
  datalayer.battery.status.reported_soc             = 5000;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1876);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x01u) << "charge not allowed bit set";
}

TEST_F(FoxessCanInverterTest, PackTempsFrameChargeAllowedWhenCurrentNonZero) {
  datalayer.battery.status.max_charge_current_dA    = 100;
  datalayer.battery.status.max_discharge_current_dA = 100;
  datalayer.battery.status.reported_soc             = 5000;
  datalayer.system.status.system_status             = ACTIVE;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1876);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u) << "charge allowed (byte 0 = 0)";
}

TEST_F(FoxessCanInverterTest, FaultStateSetsErrorByteIn1877) {
  datalayer.system.status.system_status = FAULT;
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1877);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x02u) << "error byte 0x02 on FAULT";

  datalayer.system.status.system_status = ACTIVE;
}

TEST_F(FoxessCanInverterTest, StatusByteIn1879ReflectsChargingVsDischarging) {
  // Positive current → charging → byte[1] = 0x35
  datalayer.battery.status.reported_current_dA = 100;
  foxess->update_values();
  flush_bms_info();
  {
    const CAN_frame* f = find_frame_with_id(0x1879);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->data.u8[1], 0x35u) << "charging status byte";
  }
  clear_transmitted_frames();

  // Zero or negative current → discharging → byte[1] = 0x2B
  datalayer.battery.status.reported_current_dA = 0;
  foxess->update_values();
  flush_bms_info(100);
  {
    const CAN_frame* f = find_frame_with_id(0x1879);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->data.u8[1], 0x2Bu) << "discharging status byte";
  }
}

// ---------------------------------------------------------------------------
// Per-pack encoding
// ---------------------------------------------------------------------------

TEST_F(FoxessCanInverterTest, IndividualPackVoltageAndCurrentDividedByModuleCount) {
  // Default: 8 modules. voltage / 8 * 10 (cV), current / 8.
  datalayer.battery.status.voltage_dV          = 3200;  // 320 V / 8 = 40 V → 400 cV
  datalayer.battery.status.reported_current_dA = 80;    // / 8 = 10 dA
  datalayer.battery.status.reported_soc        = 7000;  // 70%
  foxess->update_values();

  rx1871(0x01, 0x01);  // individual pack
  foxess->transmit_can(BATCH_MS);
  foxess->transmit_can(2 * BATCH_MS);

  const CAN_frame* f = find_frame_with_id(0x0C05);
  ASSERT_NE(f, nullptr);
  // voltage_per_pack = (3200 / 8) * 10 = 4000 cV
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 4000u) << "pack voltage cV";
  // current_per_pack = 80 / 8 = 10 dA
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 10u) << "pack current dA";
  // SOC byte
  EXPECT_EQ(f->data.u8[4], 70u) << "pack SOC";
}

// ---------------------------------------------------------------------------
// user_selected_* options
// ---------------------------------------------------------------------------

TEST_F(FoxessCanInverterTest, CustomModuleCountAppearsInStatusFrame) {
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_foxess_modules = 4;
  setup_inverter();
  foxess = static_cast<FoxessCanInverter*>(inverter);
  clear_transmitted_frames();

  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1875);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[3], 4u) << "custom module count in 0x1875 b3";
}

TEST_F(FoxessCanInverterTest, CustomBatteryTypeAppearsIn1877WhenMainPack) {
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_foxess_type = 0x55;
  setup_inverter();
  foxess = static_cast<FoxessCanInverter*>(inverter);
  clear_transmitted_frames();

  // current_pack_info starts at 0 = MAIN; update_values increments it,
  // so call update_values first then verify the MAIN entry was written.
  // The driver writes the main type when current_pack_info == MAIN (0)
  // at the time update_values runs.
  // We need to catch the frame from the first transmit where pack_info was 0.
  // update_values increments current_pack_info at the end of each call,
  // wrapping back to 0 after module_count. Re-init ensures pack_info starts at 0.
  foxess->update_values();
  flush_bms_info();

  const CAN_frame* f = find_frame_with_id(0x1877);
  ASSERT_NE(f, nullptr);
  // b4 = configured_battery_type when current_pack_info == MAIN (0)
  // After one update_values call, pack_info is now 1, but the frame was built with 0.
  EXPECT_EQ(f->data.u8[4], 0x55u) << "custom battery type in 0x1877 b4";
}
