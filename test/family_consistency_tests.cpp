#include <gtest/gtest.h>

#include <vector>

#include "../Software/src/battery/MG-5-BATTERY.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/inverter/CanInverterProtocol.h"
#include "../Software/src/inverter/INVERTERS.h"

// wq312: drivers diverging from what their own family or their own comment
// says they do. Each test pins the repaired family behaviour; each failed (or
// was impossible) against the defect.

void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

const CAN_frame* last_frame_with_id(uint32_t id) {
  const CAN_frame* found = nullptr;
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == id) {
      found = &f;
    }
  }
  return found;
}

class FamilyConsistencyInverterTest : public ::testing::Test {
 protected:
  void use_inverter(InverterProtocolType type) {
    user_selected_inverter_protocol = type;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    can_inverter = static_cast<CanInverterProtocol*>(inverter);
    clear_transmitted_frames();
  }

  // GROWATT-WIT refuses to transmit before the PCS heartbeat (FSN 0xB5).
  void wit_heartbeat() {
    CAN_frame hb = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x1AB5FFF3, .data = {0}};
    can_inverter->map_can_frame_to_variable(hb);
    clear_transmitted_frames();
  }

  CanInverterProtocol* can_inverter = nullptr;
};

// ── FERROAMP: a user-tightened voltage window must reach the inverter ──────

TEST_F(FamilyConsistencyInverterTest, FerroampSendsUserVoltageLimitsWhenActive) {
  use_inverter(InverterProtocolType::FerroampCan);
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.info.min_design_voltage_dV = 3000;
  datalayer.battery.settings.user_set_voltage_limits_active = true;
  datalayer.battery.settings.max_user_set_charge_voltage_dV = 3900;
  datalayer.battery.settings.max_user_set_discharge_voltage_dV = 3100;
  can_inverter->update_values();

  CAN_frame poll = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x00}};
  can_inverter->map_can_frame_to_variable(poll);

  const CAN_frame* f = last_frame_with_id(0x4221);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ((f->data.u8[1] << 8) | f->data.u8[0], 3900)
      << "charge cutoff must carry the user limit, like PYLON and SOLXPOW";
  EXPECT_EQ((f->data.u8[3] << 8) | f->data.u8[2], 3100) << "discharge cutoff must carry the user limit";
}

TEST_F(FamilyConsistencyInverterTest, FerroampStillSendsDesignLimitsWithoutUserLimits) {
  use_inverter(InverterProtocolType::FerroampCan);
  datalayer.battery.info.max_design_voltage_dV = 4000;
  datalayer.battery.info.min_design_voltage_dV = 3000;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  can_inverter->update_values();

  CAN_frame poll = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x4200, .data = {0x00}};
  can_inverter->map_can_frame_to_variable(poll);

  const CAN_frame* f = last_frame_with_id(0x4221);
  ASSERT_NE(f, nullptr);
  // The raw design limits this driver has always sent - deliberately NOT the
  // siblings' +-2.0 V offset (that would change every existing installation).
  EXPECT_EQ((f->data.u8[1] << 8) | f->data.u8[0], 4000);
  EXPECT_EQ((f->data.u8[3] << 8) | f->data.u8[2], 3000);
}

// ── GROWATT-WIT: refuse the division below 1.0 V like HV and LV do ─────────

TEST_F(FamilyConsistencyInverterTest, GrowattWitRefusesCapacityDivisionAtStartupVoltage) {
  use_inverter(InverterProtocolType::GrowattWit);
  wit_heartbeat();
  datalayer.battery.info.reported_total_capacity_Wh = 30000;
  datalayer.battery.status.voltage_dV = 5;  // 0.5 V: passes the old >0 guard
  can_inverter->update_values();
  can_inverter->transmit_can(INTERVAL_500_MS);

  const CAN_frame* f = last_frame_with_id(0x1AC6FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ((f->data.u8[3] << 8) | f->data.u8[2], 0)
      << "0.5 V is a startup reading; dividing by it turned 30 kWh into fiction (old guard sent 50000, clamped)";
}

TEST_F(FamilyConsistencyInverterTest, GrowattWitStillSendsCapacityAtRealVoltage) {
  use_inverter(InverterProtocolType::GrowattWit);
  wit_heartbeat();
  datalayer.battery.info.reported_total_capacity_Wh = 30000;
  datalayer.battery.status.voltage_dV = 3000;  // 300.0 V -> 100 Ah -> 1000 dAh
  can_inverter->update_values();
  can_inverter->transmit_can(INTERVAL_500_MS);

  const CAN_frame* f = last_frame_with_id(0x1AC6FFF3);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ((f->data.u8[3] << 8) | f->data.u8[2], 1000);  // little-endian dAh
}

// ── SOFAR: the consent ladder follows the real SoC, not the display spoof ──

TEST_F(FamilyConsistencyInverterTest, SofarFullPackRevokesChargeConsentWhileDisplayReads99) {
  use_inverter(InverterProtocolType::Sofar);
  datalayer.battery.status.reported_soc = 10000;  // truly full
  datalayer.battery.status.soh_pptt = 9900;
  can_inverter->update_values();
  can_inverter->transmit_can(INTERVAL_1_S);

  const CAN_frame* soc_frame = last_frame_with_id(0x355);
  ASSERT_NE(soc_frame, nullptr);
  EXPECT_EQ(soc_frame->data.u8[0], 99) << "the display spoof stays: 0x355 caps at 99 %";

  const CAN_frame* consent = last_frame_with_id(0x30F);
  ASSERT_NE(consent, nullptr);
  EXPECT_EQ(consent->data.u8[1], 0x01) << "a truly full pack is discharge-only; the spoof made this branch unreachable";
}

TEST_F(FamilyConsistencyInverterTest, SofarMidSocAllowsBothDirections) {
  use_inverter(InverterProtocolType::Sofar);
  datalayer.battery.status.reported_soc = 5000;
  can_inverter->update_values();
  can_inverter->transmit_can(INTERVAL_1_S);

  const CAN_frame* consent = last_frame_with_id(0x30F);
  ASSERT_NE(consent, nullptr);
  EXPECT_EQ(consent->data.u8[1], 0x03);
}

// ── MG-5: the never-compiled voltage-extended SoC branch stays deleted ─────

// MG5_USE_FULL_CAPACITY was defined nowhere, so no shipped build ever ran the
// 92 %-rescale; this pins that deleting the dead branch changed nothing - and
// that nobody quietly re-enables the untraced rescale.
TEST(Mg5FamilyConsistencyTest, SocIsPlainScalingEvenWithHighCellVoltage) {
  Mg5Battery battery;
  datalayer.battery.status.cell_max_voltage_mV = 4150;  // would trigger the deleted branch
  datalayer.battery.info.total_capacity_Wh = 50000;

  battery.update_soc(1000);  // 100.0 %

  EXPECT_EQ(datalayer.battery.status.real_soc, 10000) << "100.0 % stays 100.0 % - no 92 % compression, no extension";

  battery.update_soc(500);
  EXPECT_EQ(datalayer.battery.status.real_soc, 5000);
}

}  // namespace
