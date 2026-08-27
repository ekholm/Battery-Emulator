#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SCHNEIDER-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Schneider V2 SE BMS CAN inverter driver.
//
// TX side: three cadence groups — 500 ms (SE_321..SE_325), 2 s (SE_320,
// SE_326, SE_327), 10 s (SE_328, SE_330..SE_333).  All frames use extended
// 29-bit IDs.  Payload encoding is big-endian, voltages ×10 as 32-bit values.
// RX side: only 0x310 (ext ID) refreshes aliveness.

namespace {

class SchneiderCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::Schneider;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    schneider = static_cast<SchneiderInverter*>(inverter);
    clear_transmitted_frames();
  }

  SchneiderInverter* schneider = nullptr;
};

}  // namespace

// Helper: read a 32-bit big-endian value from four consecutive bytes.
static uint32_t u32_be(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
  return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
}

// ---- Periodic cadence -------------------------------------------------------

TEST_F(SchneiderCanInverterTest, DoesNotSendBefore500ms) {
  schneider->update_values();
  schneider->transmit_can(499);
  EXPECT_TRUE(get_transmitted_frames().empty());
}

TEST_F(SchneiderCanInverterTest, Sends500msGroupAt500ms) {
  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);
  EXPECT_NE(find_frame_with_id(0x321), nullptr);
  EXPECT_NE(find_frame_with_id(0x322), nullptr);
  EXPECT_NE(find_frame_with_id(0x323), nullptr);
  EXPECT_NE(find_frame_with_id(0x324), nullptr);
  EXPECT_NE(find_frame_with_id(0x325), nullptr);
  // 2s and 10s groups must not appear yet
  EXPECT_EQ(find_frame_with_id(0x320), nullptr);
  EXPECT_EQ(find_frame_with_id(0x330), nullptr);
}

TEST_F(SchneiderCanInverterTest, Sends2sGroupAt2s) {
  schneider->update_values();
  schneider->transmit_can(INTERVAL_2_S + 1);
  EXPECT_NE(find_frame_with_id(0x320), nullptr);
  EXPECT_NE(find_frame_with_id(0x326), nullptr);
  EXPECT_NE(find_frame_with_id(0x327), nullptr);
  EXPECT_EQ(find_frame_with_id(0x330), nullptr);
}

TEST_F(SchneiderCanInverterTest, Sends10sGroupAt10s) {
  schneider->update_values();
  schneider->transmit_can(INTERVAL_10_S + 1);
  EXPECT_NE(find_frame_with_id(0x328), nullptr);
  EXPECT_NE(find_frame_with_id(0x330), nullptr);
  EXPECT_NE(find_frame_with_id(0x331), nullptr);
  EXPECT_NE(find_frame_with_id(0x332), nullptr);
  EXPECT_NE(find_frame_with_id(0x333), nullptr);
}

// ---- RX aliveness -----------------------------------------------------------

TEST_F(SchneiderCanInverterTest, Frame0x310RefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x310, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  schneider->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(SchneiderCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x311, .data = {0, 0, 0, 0, 0, 0, 0, 0}};
  schneider->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---- Payload: SE_321 (max/min design voltage as 32-bit ×10 BE) --------------

TEST_F(SchneiderCanInverterTest, Se321EncodesDesignVoltagesAs32BitTimes10BE) {
  datalayer.battery.info.max_design_voltage_dV = 4000;  // × 10 = 40000
  datalayer.battery.info.min_design_voltage_dV = 3000;  // × 10 = 30000

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x321);
  ASSERT_NE(f, nullptr);
  // 40000 = 0x9C40 → 32-bit: 0x00, 0x00, 0x9C, 0x40
  EXPECT_EQ(u32_be(f->data.u8[0], f->data.u8[1], f->data.u8[2], f->data.u8[3]), 40000u);
  // 30000 = 0x7530 → 32-bit: 0x00, 0x00, 0x75, 0x30
  EXPECT_EQ(u32_be(f->data.u8[4], f->data.u8[5], f->data.u8[6], f->data.u8[7]), 30000u);
}

// ---- Payload: SE_322 (charge/discharge current limits as 32-bit ×10 BE) ----

TEST_F(SchneiderCanInverterTest, Se322EncodesCurrentLimitsAs32BitTimes10BE) {
  datalayer.battery.status.max_charge_current_dA = 250;    // × 10 = 2500
  datalayer.battery.status.max_discharge_current_dA = 300; // × 10 = 3000

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x322);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u32_be(f->data.u8[0], f->data.u8[1], f->data.u8[2], f->data.u8[3]), 2500u);
  EXPECT_EQ(u32_be(f->data.u8[4], f->data.u8[5], f->data.u8[6], f->data.u8[7]), 3000u);
}

// ---- Payload: SE_323 (pack voltage and current as 32-bit ×10 BE) -----------

TEST_F(SchneiderCanInverterTest, Se323EncodesVoltageAs32BitTimes10BE) {
  datalayer.battery.status.voltage_dV = 3700;  // × 10 = 37000 = 0x9088

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x323);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u32_be(f->data.u8[0], f->data.u8[1], f->data.u8[2], f->data.u8[3]), 37000u);
}

TEST_F(SchneiderCanInverterTest, Se323EncodesSignedCurrentAs32BitTimes10BE) {
  // Positive current in 0.01A units: 100 dA × 10 = 1000 centAmps
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(100);

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x323);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u32_be(f->data.u8[4], f->data.u8[5], f->data.u8[6], f->data.u8[7]), 1000u);
}

// ---- Payload: SE_324 (avg temp BE, SOC÷10 BE) ------------------------------

TEST_F(SchneiderCanInverterTest, Se324EncodesAvgTempAndSocBE) {
  datalayer.battery.status.temperature_max_dC = 250;
  datalayer.battery.status.temperature_min_dC = 150;  // avg = (250+150)/2 = 200
  datalayer.battery.status.reported_soc = 7550;        // / 10 = 755 = 0x02F3

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x324);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[0], f->data.u8[1])), 200);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 755u);
}

// ---- Payload: SE_325 (commands, warnings, faults BE) -----------------------

TEST_F(SchneiderCanInverterTest, Se325CommandsAllowBothDirectionsAtMidSoc) {
  // STATE_ONLINE, soc not 0 or 10000 → COMMAND_CHARGE_AND_DISCHARGE_ALLOWED = 6
  datalayer.battery.status.reported_soc = 5000;
  datalayer.system.status.system_status = ACTIVE;

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x325);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 0x0006u);  // CHARGE_AND_DISCHARGE_ALLOWED
}

TEST_F(SchneiderCanInverterTest, Se325CommandsOnlyDischargeWhenSocFull) {
  datalayer.battery.status.reported_soc = 10000;  // 100% → discharge only

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x325);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 0x0004u);  // ONLY_DISCHARGE_ALLOWED
}

TEST_F(SchneiderCanInverterTest, Se325CommandsOnlyChargeWhenSocEmpty) {
  datalayer.battery.status.reported_soc = 0;  // 0% → charge only

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x325);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 0x0002u);  // ONLY_CHARGE_ALLOWED
}

TEST_F(SchneiderCanInverterTest, Se325CommandsStopOnFault) {
  datalayer.system.status.system_status = FAULT;

  schneider->update_values();
  schneider->transmit_can(INTERVAL_500_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x325);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 0x0008u);  // COMMAND_STOP
}

// ---- Payload: SE_326 (state, SOH, full capacity) ---------------------------

TEST_F(SchneiderCanInverterTest, Se326EncodesOnlineStateAndSoh) {
  // STATE_ONLINE = 3
  datalayer.system.status.system_status = ACTIVE;
  datalayer.battery.status.soh_pptt = 9900;  // 99 integer

  schneider->update_values();
  schneider->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* f = find_frame_with_id(0x326);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3u);  // STATE_ONLINE
  // SOH byte: (soh_pptt / 100 >> 8) = (99 >> 8) = 0; (99 & 0xFF) = 99
  EXPECT_EQ(f->data.u8[4], 0u);
  EXPECT_EQ(f->data.u8[5], 99u);
}

TEST_F(SchneiderCanInverterTest, Se326StateIsFaultedOnSystemFault) {
  datalayer.system.status.system_status = FAULT;

  schneider->update_values();
  schneider->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* f = find_frame_with_id(0x326);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 4u);  // STATE_FAULTED
}

TEST_F(SchneiderCanInverterTest, Se326EncodesFullCapacityInAh) {
  // fully_charged_capacity_ah = (total_Wh / voltage_dV) * 100
  // = (37000 / 3700) * 100 = 10 * 100 = 1000
  datalayer.battery.status.voltage_dV = 3700;
  datalayer.battery.info.reported_total_capacity_Wh = 37000;

  schneider->update_values();
  schneider->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* f = find_frame_with_id(0x326);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 1000u);
}

// ---- Payload: SE_327 (temperature extremes and cell voltages) ---------------

TEST_F(SchneiderCanInverterTest, Se327EncodesTemperaturesAndCellVoltages) {
  datalayer.battery.status.temperature_max_dC = 320;
  datalayer.battery.status.temperature_min_dC = -50;  // signed
  datalayer.battery.status.cell_max_voltage_mV = 4200;
  datalayer.battery.status.cell_min_voltage_mV = 3600;

  schneider->update_values();
  schneider->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* f = find_frame_with_id(0x327);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[0], f->data.u8[1])), 320);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -50);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 4200u);
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 3600u);
}

// ---- Payload: SE_333 (unique identifier "SEBMS") ----------------------------

TEST_F(SchneiderCanInverterTest, Se333ContainsUniqueIdentifierSEBMS) {
  schneider->update_values();
  schneider->transmit_can(INTERVAL_10_S + 1);

  const CAN_frame* f = find_frame_with_id(0x333);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 'S');
  EXPECT_EQ(f->data.u8[1], 'E');
  EXPECT_EQ(f->data.u8[2], 'B');
  EXPECT_EQ(f->data.u8[3], 'M');
  EXPECT_EQ(f->data.u8[4], 'S');
}

// ---- Payload: SE_320 (protocol version) -------------------------------------

TEST_F(SchneiderCanInverterTest, Se320ContainsProtocolVersion0x0002) {
  schneider->update_values();
  schneider->transmit_can(INTERVAL_2_S + 1);

  const CAN_frame* f = find_frame_with_id(0x320);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u);
  EXPECT_EQ(f->data.u8[1], 0x02u);
}
