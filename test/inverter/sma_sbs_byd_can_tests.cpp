#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/SMA-SBS-BYD-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the SMA SBS compatible BYD Battery-Box HVS CAN driver.
//
// This driver shares the SmaInverterBase family shape but differs from the H
// variant in the following ways:
//   - Pairing fires an immediate burst of ALL 13 frames (no batching delay).
//   - The 100ms periodic path is gated on contactors_engaged == 1, not on
//     inverter_allows_contactor_closing.
//   - 0x4D8 bytes 2:3 encode datalayer.battery.status.current_dA (not
//     reported_current_dA) – this differs from both the H and HVS variants
//     and is likely a copy-paste defect (SMA-SBS-BYD-CAN.cpp:51-52 vs the
//     equivalent lines in SMA-BYD-H-CAN.cpp / SMA-BYD-HVS-CAN.cpp).

namespace {

class SmaSbsBydCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already reset datalayer/events and destroyed
    // the previous inverter instance before this runs.
    user_selected_inverter_protocol = InverterProtocolType::SmaSBSByd;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    sbs = static_cast<SmaSBSBydHvsInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Trigger the pairing handshake (which immediately fires all init frames).
  void send_pairing_frame(uint32_t id = 0x5E7) {
    CAN_frame pair = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    sbs->map_can_frame_to_variable(pair);
  }

  SmaSBSBydHvsInverter* sbs = nullptr;
};

}  // namespace

// ── TX gating ────────────────────────────────────────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, SilentBeforePairingWithContactorsOpen) {
  // No pairing → transmit_can_init stays false; contactors_engaged = 0 (default).
  sbs->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Driver must not transmit without a pairing event and open contactors";
}

TEST_F(SmaSbsBydCanInverterTest, HundredMsPathFiresWithContactorsEngagedEvenWithoutPairing) {
  // The SBS 100ms periodic path is gated ONLY on contactors_engaged, unlike
  // the H variant which additionally requires a pairing event.  This is pinned
  // as current production behaviour; whether it is intentional or a copy-paste
  // difference from SMA-BYD-H-CAN is left for the upstream author to clarify.
  datalayer.system.status.contactors_engaged = 1;
  sbs->transmit_can(INTERVAL_100_MS + 1);
  // At least one of the 100ms payload frames must have been sent.
  EXPECT_GE(count_frames_with_id(0x358), 1u)
      << "100ms path must fire when contactors_engaged == 1 even without prior pairing";
}

// ── Pairing handshake ─────────────────────────────────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, PairingRequest5E7ImmediatelySendsAllInitFrames) {
  send_pairing_frame(0x5E7);
  sbs->transmit_can(1);  // Just needs to be called once; init fires immediately.
  // 13 frames: 558, 598, 5D8, 618×4, 158, 358, 3D8, 458, 518, 4D8.
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x598), 1u);
  EXPECT_EQ(count_frames_with_id(0x5D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x618), 4u);
  EXPECT_EQ(count_frames_with_id(0x158), 1u);
  EXPECT_EQ(count_frames_with_id(0x358), 1u);
  EXPECT_EQ(count_frames_with_id(0x3D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x458), 1u);
  EXPECT_EQ(count_frames_with_id(0x518), 1u);
  EXPECT_EQ(count_frames_with_id(0x4D8), 1u);
}

TEST_F(SmaSbsBydCanInverterTest, PairingRequest660AlsoWorks) {
  send_pairing_frame(0x660);
  sbs->transmit_can(1);
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x618), 4u);
}

TEST_F(SmaSbsBydCanInverterTest, PairingFlagsResetsAfterFirstTransmitCall) {
  send_pairing_frame();
  sbs->transmit_can(1);  // drains the init burst and resets transmit_can_init
  clear_transmitted_frames();

  sbs->transmit_can(2);  // second call with no new pairing → no init frames
  EXPECT_EQ(count_frames_with_id(0x558), 0u)
      << "Init burst must only fire once per pairing event";
}

// ── Periodic cadence (contactors_engaged == 1) ────────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, HundredMsCadenceFiresWhenContactorsEngaged) {
  send_pairing_frame();
  sbs->transmit_can(1);  // consume the init burst
  clear_transmitted_frames();

  datalayer.system.status.contactors_engaged = 1;
  sbs->transmit_can(INTERVAL_100_MS + 1);
  EXPECT_GE(count_frames_with_id(0x358), 1u);
  EXPECT_GE(count_frames_with_id(0x3D8), 1u);
  EXPECT_GE(count_frames_with_id(0x4D8), 1u);
}

TEST_F(SmaSbsBydCanInverterTest, HundredMsCadenceBlockedWithContactorsOpen) {
  send_pairing_frame();
  sbs->transmit_can(1);
  clear_transmitted_frames();

  datalayer.system.status.contactors_engaged = 0;
  sbs->transmit_can(INTERVAL_100_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x358), 0u)
      << "100ms cadence must be gated on contactors_engaged == 1";
}

// ── TX payload – frame 0x358 (limits) ────────────────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, LimitsFrameEncodesVoltagesAndCurrents) {
  datalayer.battery.info.max_design_voltage_dV = 4300;
  datalayer.battery.info.min_design_voltage_dV = 3100;
  datalayer.battery.status.max_discharge_current_dA = 600;
  datalayer.battery.status.max_charge_current_dA = 200;

  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x358);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 4300u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 3100u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 600u);
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 200u);
}

// ── TX payload – frame 0x3D8 (SoC / SoH / Ah remaining) ─────────────────────

TEST_F(SmaSbsBydCanInverterTest, SocSohAhFrameEncodesCorrectly) {
  datalayer.battery.status.reported_soc = 6000;  // 60.00 %
  datalayer.battery.status.soh_pptt = 9500;      // 95.00 %
  // Ah = (18000 / 3600) * 100 = 500
  datalayer.battery.status.reported_remaining_capacity_Wh = 18000;
  datalayer.battery.status.voltage_dV = 3600;

  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x3D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 6000u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 9500u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 500u);
}

// ── TX payload – frame 0x4D8 (voltage / current / temp / ready) ──────────────
//
// NOTE: this driver encodes current_dA (not reported_current_dA) in bytes 2:3,
// unlike the H and HVS variants.  This is pinned as current production
// behaviour; it is suspected to be a copy-paste defect (see file header).

TEST_F(SmaSbsBydCanInverterTest, BatteryInfoFrameEncodesCurrentDaNotReportedCurrentDa) {
  datalayer.battery.status.voltage_dV = 3900;
  // Set both fields to different values so we can distinguish which one is sent.
  datalayer.battery.status.current_dA = static_cast<int16_t>(-300);
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-400);
  datalayer.battery.status.temperature_max_dC = 260;
  datalayer.battery.status.temperature_min_dC = 240;  // avg = 250

  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3900u);
  // Pinned behaviour: current_dA (-300), NOT reported_current_dA (-400).
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -300);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[4], f->data.u8[5])), 250);
}

TEST_F(SmaSbsBydCanInverterTest, FaultStatusSetsByte6ToStopState) {
  datalayer.system.status.system_status = FAULT;
  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x02u);  // STOP_STATE
}

TEST_F(SmaSbsBydCanInverterTest, NormalStatusSetsByte6ToReadyState) {
  datalayer.system.status.system_status = ACTIVE;
  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x03u);  // READY_STATE
}

// ── TX payload – frame 0x518 (temperatures / voltage / cell voltages) ─────────

TEST_F(SmaSbsBydCanInverterTest, TemperatureFrameEncodesMinMaxCellsAndVoltage) {
  datalayer.battery.status.temperature_max_dC = 290;
  datalayer.battery.status.temperature_min_dC = -80;  // signed negative
  datalayer.battery.status.voltage_dV = 4000;
  datalayer.battery.status.cell_min_voltage_mV = 3750;  // 3750 / 25 = 150
  datalayer.battery.status.cell_max_voltage_mV = 3900;  // 3900 / 25 = 156

  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x518);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[0], f->data.u8[1])), 290);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -80);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 4000u);
  EXPECT_EQ(f->data.u8[6], 150u);
  EXPECT_EQ(f->data.u8[7], 156u);
}

// ── TX payload – frame 0x458 (lifetime energy counters) ──────────────────────

TEST_F(SmaSbsBydCanInverterTest, EnergyCounterFrameEncodesChargedAndDischarged) {
  datalayer.battery.status.total_charged_battery_Wh = 0x00FEDCBA;
  datalayer.battery.status.total_discharged_battery_Wh = 0x00009876;

  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x458);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u);
  EXPECT_EQ(f->data.u8[1], 0xFEu);
  EXPECT_EQ(f->data.u8[2], 0xDCu);
  EXPECT_EQ(f->data.u8[3], 0xBAu);
  EXPECT_EQ(f->data.u8[4], 0x00u);
  EXPECT_EQ(f->data.u8[5], 0x00u);
  EXPECT_EQ(f->data.u8[6], 0x98u);
  EXPECT_EQ(f->data.u8[7], 0x76u);
}

// ── TX payload – frame 0x158 (error flag byte 2) ─────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, ErrorFlagByte2Is0xAAWhenBatteryAllows) {
  datalayer.system.status.battery_allows_contactor_closing = true;
  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x158);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[2], 0xAAu);
}

TEST_F(SmaSbsBydCanInverterTest, ErrorFlagByte2Is0x6AWhenBatteryForbids) {
  datalayer.system.status.battery_allows_contactor_closing = false;
  sbs->update_values();
  send_pairing_frame();
  sbs->transmit_can(1);

  const CAN_frame* f = find_frame_with_id(0x158);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[2], 0x6Au);
}

// ── RX – aliveness ────────────────────────────────────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, KnownRxFramesRefreshAliveness) {
  for (uint32_t id : {0x360u, 0x3E0u, 0x420u,
                      0x560u, 0x561u, 0x562u, 0x563u,
                      0x564u, 0x565u, 0x566u, 0x567u,
                      0x5E0u, 0x5E1u, 0x5E2u, 0x5E3u,
                      0x5E4u, 0x5E5u, 0x5E6u, 0x5E7u,
                      0x62Cu, 0x660u}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    sbs->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 3u)
        << "ID 0x" << std::hex << id << " should refresh aliveness";
  }
}

TEST_F(SmaSbsBydCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x400, .data = {0}};
  sbs->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0u);
}

// ── Safety-critical: contactor control ───────────────────────────────────────

TEST_F(SmaSbsBydCanInverterTest, AllowsContactorClosingReadsGpioReturnsFalse) {
  // digitalRead always returns 0 in the test emulation.
  EXPECT_FALSE(sbs->allows_contactor_closing());
}

TEST_F(SmaSbsBydCanInverterTest, ControlsContactorReturnsTrue) {
  EXPECT_TRUE(sbs->controls_contactor());
}

TEST_F(SmaSbsBydCanInverterTest, NeedsCanStartupGraceReturnsTrue) {
  EXPECT_TRUE(sbs->needs_can_startup_grace());
}
