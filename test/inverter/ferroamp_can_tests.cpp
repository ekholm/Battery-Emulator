#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/FERROAMP-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Ferroamp Pylon-variant CAN inverter driver.
//
// Ferroamp uses a request/response model: the inverter sends 0x4200 to request
// data; byte 0 == 0x02 triggers setup info (0x7311/0x7321), byte 0 == 0x00
// triggers full system data (0x4211-0x4291). No periodic transmission occurs.
// All multi-byte fields are little-endian; current/charge fields carry a +30000
// offset; discharge uses 30000 - value.

namespace {

class FerroampCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::FerroampCan;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    ferro = static_cast<FerroampCanInverter*>(inverter);
    clear_transmitted_frames();
  }

  void TearDown() override {
    // Reset any user_selected_inverter_* globals this fixture may have set.
    user_selected_inverter_cells = 0;
    user_selected_inverter_modules = 0;
    user_selected_inverter_cells_per_module = 0;
    user_selected_inverter_voltage_level = 0;
    user_selected_inverter_ah_capacity = 0;
  }

  // Send a 0x4200 with the given request byte.
  void send_inverter_request(uint8_t request_byte) {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x4200, .data = {request_byte}};
    ferro->map_can_frame_to_variable(f);
  }

  FerroampCanInverter* ferro = nullptr;
};

}  // namespace

TEST_F(FerroampCanInverterTest, KnownRxFrameRefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  send_inverter_request(0x00);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(FerroampCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FFF, .data = {0}};
  ferro->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(FerroampCanInverterTest, NoPeriodicTransmission) {
  ferro->update_values();
  ferro->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty()) << "Ferroamp has no periodic TX; only reacts to RX";
}

TEST_F(FerroampCanInverterTest, SetupRequestTriggers7311And7321) {
  ferro->update_values();
  send_inverter_request(0x02);
  EXPECT_EQ(count_frames_with_id(0x7311), 1u);
  EXPECT_EQ(count_frames_with_id(0x7321), 1u);
  // System-data frames must not appear for a setup request.
  EXPECT_EQ(count_frames_with_id(0x4211), 0u);
}

TEST_F(FerroampCanInverterTest, SystemDataRequestTriggersAllDataFrames) {
  ferro->update_values();
  send_inverter_request(0x00);

  for (uint32_t id : {0x4211u, 0x4221u, 0x4231u, 0x4241u, 0x4251u, 0x4261u, 0x4271u, 0x4281u, 0x4291u}) {
    EXPECT_EQ(count_frames_with_id(id), 1u) << "Missing frame 0x" << std::hex << id;
  }
  // Setup frames must not appear for a data request.
  EXPECT_EQ(count_frames_with_id(0x7311), 0u);
}

TEST_F(FerroampCanInverterTest, SystemDataFrameEncodesVoltageCurrentTempSocSoh) {
  // 0x4211 — little-endian; current has +30000 offset; temperature has +1000
  datalayer.battery.status.voltage_dV = 4000;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-150);  // charging
  datalayer.battery.status.temperature_max_dC = 300;
  datalayer.battery.status.reported_soc = 6500;  // 65.00 %
  datalayer.battery.status.soh_pptt = 9500;      // 95.00 %

  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* f = find_frame_with_id(0x4211);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4000u);
  // Current offset: -150 + 30000 = 29850
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), static_cast<uint16_t>(-150 + 30000));
  // Temperature offset: 300 + 1000 = 1300
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), static_cast<uint16_t>(300 + 1000));
  EXPECT_EQ(f->data.u8[6], 65u);  // SOC %
  EXPECT_EQ(f->data.u8[7], 95u);  // SOH %
}

TEST_F(FerroampCanInverterTest, LimitsFrameEncodesVoltagesAndCurrents) {
  // 0x4221 — charge voltage = max design, discharge = 30000 - discharge_current
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.info.min_design_voltage_dV = 3000;
  datalayer.battery.status.max_charge_current_dA = 100;     // +30000 -> 30100
  datalayer.battery.status.max_discharge_current_dA = 200;  // 30000 - 200 -> 29800

  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* f = find_frame_with_id(0x4221);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4000u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 3000u);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), static_cast<uint16_t>(100 + 30000));
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), static_cast<uint16_t>(30000 - 200));
}

TEST_F(FerroampCanInverterTest, CellVoltageFramePassesThroughLfpValues) {
  // 0x4231 — LFP: no remapping
  datalayer.battery.info.chemistry = battery_chemistry_enum::LFP;
  datalayer.battery.status.cell_max_voltage_mV = 3300;
  datalayer.battery.status.cell_min_voltage_mV = 2900;

  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* f = find_frame_with_id(0x4231);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3300u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 2900u);
}

/* 0x4231 - Non-LFP: the DONOR cell voltages are not reported at all.
 *
 * Ferroamp only accepts an LFP-shaped pack, and a non-LFP donor's voltage/SOC
 * curve is a different shape, not a different range - so scaling the donor's
 * own cell voltages into an LFP window (which is what this driver used to do)
 * still hands the inverter a curve it does not recognise. The driver now
 * estimates a Pylontech-LFP cell voltage from SOC instead and reports that,
 * spread by PYLON_CELL_SPREAD_mV either side.
 *
 * So the property is INDEPENDENCE from the donor cell voltages, and it is
 * asserted that way rather than by pinning one output: pinning a number alone
 * would still pass if the remap came back under a new name. The curve point
 * used here is 50% -> 3310 mV, read from the table in FERROAMP-CAN.cpp.
 */
TEST_F(FerroampCanInverterTest, CellVoltageFrameReportsAnLfpCurveForNonLfpDonors) {
  datalayer.battery.info.chemistry = battery_chemistry_enum::NCA;
  datalayer.battery.status.reported_soc = 5000;  // 50% -> 3310 mV on the curve
  datalayer.battery.status.cell_max_voltage_mV = 4200;
  datalayer.battery.status.cell_min_voltage_mV = 2500;

  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* f = find_frame_with_id(0x4231);
  ASSERT_NE(f, nullptr);
  const uint16_t reported_max = u16_le(f->data.u8[0], f->data.u8[1]);
  const uint16_t reported_min = u16_le(f->data.u8[2], f->data.u8[3]);
  EXPECT_EQ(reported_max, 3313u) << "50% SOC is 3310 mV on the curve, + the 3 mV spread";
  EXPECT_EQ(reported_min, 3307u) << "50% SOC is 3310 mV on the curve, - the 3 mV spread";

  // Same SOC, wildly different donor cells: the frame must not move.
  clear_transmitted_frames();
  datalayer.battery.status.cell_max_voltage_mV = 3900;
  datalayer.battery.status.cell_min_voltage_mV = 3600;
  ferro->update_values();
  send_inverter_request(0x00);
  const CAN_frame* g = find_frame_with_id(0x4231);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(u16_le(g->data.u8[0], g->data.u8[1]), reported_max)
      << "the reported cell voltage still follows the donor's own cells, so a non-LFP pack is back to "
         "handing Ferroamp a curve it does not recognise";
  EXPECT_EQ(u16_le(g->data.u8[2], g->data.u8[3]), reported_min);
}

TEST_F(FerroampCanInverterTest, TemperatureFrameEncodesPerCellMaxAndMin) {
  // 0x4241 - temperature_max/min_dC carried with the protocol's +1000 dC offset
  // (TEMPERATURE_OFFSET_dC), so the field is unsigned on the wire and 0 dC is 1000.
  datalayer.battery.status.temperature_max_dC = 350;  // 35.0 °C
  datalayer.battery.status.temperature_min_dC = 50;   //  5.0 °C

  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* f = find_frame_with_id(0x4241);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 1350u) << "35.0 C + the 100.0 C offset";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 1050u) << "5.0 C + the 100.0 C offset";
}

TEST_F(FerroampCanInverterTest, StatusByteReflectsChargingCurrent) {
  // 0x4251 byte 0: 1 = Charge (current < 0), 2 = Discharge (current > 0), 3 = Idle
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-50);
  ferro->update_values();
  send_inverter_request(0x00);
  const CAN_frame* f = find_frame_with_id(0x4251);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x01u) << "Negative current => Charge";
}

TEST_F(FerroampCanInverterTest, StatusByteReflectsDischargingCurrent) {
  datalayer.battery.status.reported_current_dA = 50;
  ferro->update_values();
  send_inverter_request(0x00);
  const CAN_frame* f = find_frame_with_id(0x4251);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x02u) << "Positive current => Discharge";
}

TEST_F(FerroampCanInverterTest, StatusByteReflectsIdle) {
  datalayer.battery.status.reported_current_dA = 0;
  ferro->update_values();
  send_inverter_request(0x00);
  const CAN_frame* f = find_frame_with_id(0x4251);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x03u) << "Zero current => Idle";
}

TEST_F(FerroampCanInverterTest, FaultModeSetsStatusByteSleepAndForbidenBytes) {
  datalayer.system.status.system_status = FAULT;
  // A distinctive SOC, because byte 3 of the same frame carries it: leaving the
  // fixture's default here makes "the fault blanked the SOC byte" and "the SOC
  // was zero anyway" the same observation, and the assertion below stops biting.
  datalayer.battery.status.reported_soc = 4200;
  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* status = find_frame_with_id(0x4251);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->data.u8[0], 0x00u) << "FAULT => Sleep";

  const CAN_frame* prot = find_frame_with_id(0x4281);
  ASSERT_NE(prot, nullptr);
  // Bytes 0 and 1 are the protection flags. Bytes 2 and 3 are NOT: 2 is the
  // heartbeat counter stamped at transmit time and 3 is the reported SOC, so
  // asserting 0xAA across all four would be asserting over two unrelated
  // fields - and would red on nothing but the heartbeat's own value.
  EXPECT_EQ(prot->data.u8[0], 0xAAu);
  EXPECT_EQ(prot->data.u8[1], 0xAAu);
  EXPECT_EQ(prot->data.u8[3], datalayer.battery.status.reported_soc / 100)
      << "byte 3 of 0x4281 is the SOC, and a fault must not blank it";
}

TEST_F(FerroampCanInverterTest, NormalModeProtectionBytesAreClear) {
  datalayer.system.status.system_status = ACTIVE;
  ferro->update_values();
  send_inverter_request(0x00);

  const CAN_frame* prot = find_frame_with_id(0x4281);
  ASSERT_NE(prot, nullptr);
  EXPECT_EQ(prot->data.u8[0], 0x00u);
  EXPECT_EQ(prot->data.u8[1], 0x00u);
}

TEST_F(FerroampCanInverterTest, UserSelectedCellsOverrides7321) {
  user_selected_inverter_cells = 240;  // non-zero -> override

  ferro->update_values();
  send_inverter_request(0x02);

  const CAN_frame* f = find_frame_with_id(0x7321);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 240u);
}

TEST_F(FerroampCanInverterTest, UserSelectedModulesAndCellsPerModuleOverride7321) {
  user_selected_inverter_modules = 8;
  user_selected_inverter_cells_per_module = 15;

  ferro->update_values();
  send_inverter_request(0x02);

  const CAN_frame* f = find_frame_with_id(0x7321);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[2], 8u);
  EXPECT_EQ(f->data.u8[3], 15u);
}

TEST_F(FerroampCanInverterTest, UserSelectedVoltageLevelAndAhCapacityOverride7321) {
  user_selected_inverter_voltage_level = 512;
  user_selected_inverter_ah_capacity = 50;

  ferro->update_values();
  send_inverter_request(0x02);

  const CAN_frame* f = find_frame_with_id(0x7321);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 512u);
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 50u);
}

TEST_F(FerroampCanInverterTest, ZeroUserSelectedDoesNotOverrideDefaults) {
  // All user_selected_inverter_* are 0 (default from TearDown) -> static defaults remain
  ferro->update_values();
  send_inverter_request(0x02);

  const CAN_frame* f = find_frame_with_id(0x7321);
  ASSERT_NE(f, nullptr);
  // Defaults describe a Force-H3-like pack: TOTAL_CELL_AMOUNT = 576 (3 modules
  // x 32 cells x 6), MODULES_IN_SERIES = 3.
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 576u);
  EXPECT_EQ(f->data.u8[2], 3u);
}
