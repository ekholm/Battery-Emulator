#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SOFAR-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Sofar BMS (Extended) via CAN inverter driver.
//
// This driver has no inverter-speaks-first handshake and no contactor-enable
// GPIO gate.  It self-feeds CAN_inverter_still_alive in update_values().
//
// TX side: a 1s periodic sends 0x351, 0x355, 0x356, 0x359, 0x35E, 0x35F.
// 0x30F is sent at 1s when its payload is non-zero, which every SoC produces:
// byte1 = enable_flags is 0x02 charging-only at the bottom, 0x01 discharge-only
// at the cap, and 0x03 in between.
// 0x35A is edge-triggered: only re-sent when its payload differs from the
// last-sent snapshot. The snapshot lives in the instance, so it dies with the
// driver.
//
// RX side: 0x605 and 0x705 address-filtered on
// sofar_user_specified_battery_id; matching frames set aliveness and trigger
// on-demand responses.  Unknown IDs are silently ignored.
//
// Two defects this file used to document as present are FIXED, and the cases
// below pin the fixed behaviour:
//
//   - The 0x30F "full, discharge only" branch compared soc_percent against 100
//     while spoofed_soc is capped at 99%, so it could never be taken and the
//     inverter was never told to stop charging by this flag. Both ends now use
//     SOFAR_MAX_REPORTED_SOC_PPTT.
//   - `last_35A_payload` / `have_last_35A` were function-local statics and so
//     outlived the driver, which suppressed the first 0x35A after a driver
//     restart whenever the payload matched. They are members now, which is why
//     the change-edge and fresh-instance cases below can exist at all - this
//     file previously recorded that they could not be written.

namespace {

class SofarCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already reset datalayer/events and destroyed
    // the previous inverter instance before this runs.
    user_selected_inverter_protocol = InverterProtocolType::Sofar;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    sofar = static_cast<SofarInverter*>(inverter);
    clear_transmitted_frames();
  }

  // Send an addressed 0x605 query frame to the driver.
  void send_605(uint8_t target_id, uint8_t inquiry, uint8_t sub_k = 0) {
    CAN_frame f = {
        .FD = false, .ext_ID = true, .DLC = 8, .ID = 0x605, .data = {target_id, inquiry, sub_k, 0, 0, 0, 0, 0}};
    sofar->map_can_frame_to_variable(f);
  }

  // Send an addressed 0x705 history query frame.
  void send_705(uint8_t target_id, uint8_t inquiry, uint8_t record_n = 0, uint8_t sub_k = 0) {
    CAN_frame f = {
        .FD = false, .ext_ID = true, .DLC = 8, .ID = 0x705, .data = {target_id, inquiry, record_n, sub_k, 0, 0, 0, 0}};
    sofar->map_can_frame_to_variable(f);
  }

  SofarInverter* sofar = nullptr;
};

}  // namespace

// ── 0x35A – edge-triggered, and the edge state belongs to the INSTANCE ───────
//
// This used to be untestable. `last_35A_payload` and `have_last_35A` were
// function-local statics in transmit_can(), so they outlived the driver: the
// case below could only assert that nothing crashed, and it had to run first
// alphabetically to have any meaning at all. They are members now, which is
// both the production fix and what lets the behaviour be asserted.
//
// The production defect the statics caused: destroy the driver and build a new
// one - a protocol switch, or a battery-id change - and the fresh instance
// inherited the old one's idea of what it had already sent, so the first 0x35A
// after the restart was SUPPRESSED whenever the payload happened to match.
// 0x35A carries the alarm and protection flags, so the frame that went missing
// is the one that matters most.

TEST_F(SofarCanInverterTest, FirstTransmitAlwaysSends35A) {
  sofar->update_values();
  sofar->transmit_can(INTERVAL_200_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x35A), 1u) << "a fresh driver has sent nothing, so the first 0x35A is an edge";
}

TEST_F(SofarCanInverterTest, UnchangedPayloadIsNotResent) {
  sofar->update_values();
  sofar->transmit_can(INTERVAL_200_MS + 1);
  clear_transmitted_frames();

  // Same payload, enough time elapsed: the edge is what gates it, not the clock.
  sofar->update_values();
  sofar->transmit_can(2 * (INTERVAL_200_MS + 1));
  EXPECT_EQ(count_frames_with_id(0x35A), 0u) << "0x35A is edge-triggered, not periodic";
}

TEST_F(SofarCanInverterTest, ANewInstanceDoesNotInheritTheOldEdgeState) {
  // THE DEFECT, in the shape it takes on a device: send once, then replace the
  // driver the way a protocol or battery-id change does. With the state in
  // statics the new instance stayed quiet - it believed the old instance's
  // transmission was its own - and the alarm frame never reached the inverter.
  sofar->update_values();
  sofar->transmit_can(INTERVAL_200_MS + 1);
  ASSERT_EQ(count_frames_with_id(0x35A), 1u);

  delete inverter;
  inverter = nullptr;
  setup_inverter();
  ASSERT_NE(inverter, nullptr);
  sofar = static_cast<SofarInverter*>(inverter);
  clear_transmitted_frames();

  sofar->update_values();
  sofar->transmit_can(3 * (INTERVAL_200_MS + 1));
  EXPECT_EQ(count_frames_with_id(0x35A), 1u)
      << "a new driver has sent nothing yet, so its first 0x35A must go out even though "
         "the payload matches what the PREVIOUS instance sent";
}

// ── update_values feeds aliveness directly ───────────────────────────────────

TEST_F(SofarCanInverterTest, UpdateValuesSetsInverterAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  sofar->update_values();
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE)
      << "Sofar feeds its own aliveness in update_values()";
}

TEST_F(SofarCanInverterTest, NeedsCanStartupGraceReturnsFalse) {
  // Sofar inherits from CanInverterProtocol, not SmaInverterBase.
  EXPECT_FALSE(sofar->needs_can_startup_grace());
}

// ── 1s periodic: frame presence ──────────────────────────────────────────────

TEST_F(SofarCanInverterTest, OneSPeriodicSendsAllCyclicFrames) {
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);
  EXPECT_EQ(count_frames_with_id(0x351), 1u);
  EXPECT_EQ(count_frames_with_id(0x355), 1u);
  EXPECT_EQ(count_frames_with_id(0x356), 1u);
  EXPECT_EQ(count_frames_with_id(0x359), 1u);
  EXPECT_EQ(count_frames_with_id(0x35E), 1u);
  EXPECT_EQ(count_frames_with_id(0x35F), 1u);
}

TEST_F(SofarCanInverterTest, FramesNotSentBeforeOneSecond) {
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S - 1);
  EXPECT_EQ(count_frames_with_id(0x351), 0u) << "1s cyclic frames must not fire before the 1s boundary";
}

// ── TX payload – frame 0x351 (limits, little-endian) ─────────────────────────

TEST_F(SofarCanInverterTest, LimitsFrameEncodesVoltagesAndCurrentsLittleEndian) {
  datalayer.battery.info.max_design_voltage_dV = 4000;      // 400.0 V
  datalayer.battery.info.min_design_voltage_dV = 3000;      // 300.0 V
  datalayer.battery.status.max_charge_current_dA = 250;     // 25.0 A
  datalayer.battery.status.max_discharge_current_dA = 500;  // 50.0 A

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr);
  // All 0x351 fields are little-endian (u16_le).
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4000u)  // max design voltage
      << "0x351[0:1] charge cutoff voltage (LE)";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 250u)  // max charge current
      << "0x351[2:3] charge current (LE)";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 500u)  // max discharge current
      << "0x351[4:5] discharge current (LE)";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 3000u)  // min design voltage
      << "0x351[6:7] discharge cutoff voltage (LE)";
}

// ── TX payload – frame 0x355 (SoC / SoH as integer percent) ──────────────────

TEST_F(SofarCanInverterTest, SocReportedAsIntegerPercent) {
  datalayer.battery.status.reported_soc = 7500;  // 75.00 % → byte 0 = 75
  datalayer.battery.status.soh_pptt = 9800;      // 98.00 % → byte 2 = 98

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x355);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 75u) << "0x355 byte 0 must be integer SoC %";
  EXPECT_EQ(f->data.u8[2], 98u) << "0x355 byte 2 must be integer SoH %";
}

TEST_F(SofarCanInverterTest, SocCappedAt99PercentWhenReportedIs100) {
  // 10000 pptt = exactly 100%; driver caps spoofed_soc at 9900 → byte0 = 99.
  datalayer.battery.status.reported_soc = 10000;

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x355);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 99u) << "0x355 SoC byte must be capped at 99 to prevent the inverter treating 100% as 0";
}

// ── TX payload – frame 0x356 (voltage / current / temp, little-endian) ────────

TEST_F(SofarCanInverterTest, PackFrameEncodesVoltageSignedCurrentAndTemp) {
  datalayer.battery.status.voltage_dV = 3700;
  // Signed discharge current: -810 = 0xFCCA
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-810);
  datalayer.battery.status.temperature_max_dC = 330;  // 33.0 °C

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x356);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3700u) << "0x356[0:1] voltage (LE)";
  // Current is signed; cast to int16_t after LE assembly.
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[2], f->data.u8[3])), -810) << "0x356[2:3] signed current (LE)";
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[4], f->data.u8[5])), 330)
      << "0x356[4:5] max temperature (LE, signed)";
}

// ── TX payload – frame 0x35F (capacity AH) ───────────────────────────────────

TEST_F(SofarCanInverterTest, CapacityFrameEncodesAhFromWh) {
  // AH = total_capacity_Wh / (max_design_voltage_dV * 0.1)
  //    = 40000 / (4000 * 0.1) = 40000 / 400 = 100
  datalayer.battery.info.reported_total_capacity_Wh = 40000;
  datalayer.battery.info.max_design_voltage_dV = 4000;

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35F);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x01u) << "0x35F byte 0 = Li-ion type";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 100u) << "0x35F[4:5] nominal capacity in Ah (LE)";
}

TEST_F(SofarCanInverterTest, CapacityNotUpdatedWhenMaxVoltageAtOrBelowTwenty) {
  datalayer.battery.info.max_design_voltage_dV = 10;
  datalayer.battery.info.reported_total_capacity_Wh = 30000;

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x35F);
  ASSERT_NE(f, nullptr);
  // Guard: capacity stays at 0 when voltage is too low to avoid div0.
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 0u);
}

// ── TX – frame 0x30F (remote command / enable) ────────────────────────────────

TEST_F(SofarCanInverterTest, RemoteCommandFrameSentWhenEnableFlagsNonZero) {
  // At normal SoC (0 < soc_percent < 100), enable_flags = 0x03 → byte1 ≠ 0
  // → remote_cmd_active = true → 0x30F fires at 1s.
  datalayer.battery.status.reported_soc = 5000;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);
  EXPECT_EQ(count_frames_with_id(0x30F), 1u) << "0x30F must be sent at 1s when enable flags are non-zero";
}

TEST_F(SofarCanInverterTest, RemoteCommandByte1IsThreeForNormalSoc) {
  datalayer.battery.status.reported_soc = 5000;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x30F);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u) << "0x30F byte 0 = 0 (normal mode)";
  EXPECT_EQ(f->data.u8[1], 0x03u) << "0x30F byte 1 = 0x03 (both charge and discharge enabled)";
}

TEST_F(SofarCanInverterTest, RemoteCommandByte1IsTwoWhenSocAtZero) {
  // soc_percent <= 1 → only charging allowed (enable_flags = 0x02)
  datalayer.battery.status.reported_soc = 0;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x30F);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[1], 0x02u) << "0x30F byte 1 must be 0x02 (charge only) at SoC = 0%";
}

// The full end of the consent range. This branch was UNREACHABLE: it compared
// soc_percent against 100 while spoofed_soc is capped at 99% just above, so the
// inverter was never told to stop charging by this flag however full the pack
// got - it only ever saw 0x03. The cap is deliberate (a Sofar reads a literal
// 100 as 0), so the threshold is the cap, not 100. Both are now expressed
// against SOFAR_MAX_REPORTED_SOC_PPTT and cannot drift apart again.

TEST_F(SofarCanInverterTest, RemoteCommandByte1IsOneWhenTheReportedSocIsFull) {
  datalayer.battery.status.reported_soc = 10000;  // real 100%, reported as 99
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x30F);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[1], 0x01u) << "0x30F byte 1 must be 0x01 (discharge only) once the pack reads full";
}

TEST_F(SofarCanInverterTest, TheFullThresholdIsTheReportingCapNotOneHundred) {
  // The bug in one assertion: the reported SoC can never exceed the cap, so a
  // threshold above it can never be met. Pin them equal.
  datalayer.battery.status.reported_soc = SOFAR_MAX_REPORTED_SOC_PPTT;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f355 = find_frame_with_id(0x355);
  ASSERT_NE(f355, nullptr);
  EXPECT_EQ(f355->data.u8[0], SOFAR_MAX_REPORTED_SOC_PPTT / 100) << "the reported SoC tops out at the cap";

  const CAN_frame* f30F = find_frame_with_id(0x30F);
  ASSERT_NE(f30F, nullptr);
  EXPECT_EQ(f30F->data.u8[1], 0x01u)
      << "and the consent must treat that same value as full, or its branch is dead code";
}

TEST_F(SofarCanInverterTest, TheChargeLimitStillPermitsChargeAtTheSocTheConsentCallsFull) {
  // R382. Making the "full" branch reachable at the CAP - rather than at a true
  // 100.00% - means a pack whose real SoC is 99.00% is told discharge-only,
  // because the cap makes 99% and 100% indistinguishable on this frame. That
  // collides with the system-wide full policy, which fires only at an exact
  // 10000 pptt (safety.cpp zeroes max_charge_power_W there and raises
  // EVENT_BATTERY_FULL). So at the cap the driver emits a contradiction: 0x351
  // still advertises a charge current the pack is allowed to take, while 0x30F
  // says charging is not permitted at all.
  //
  // This test asserts the contradiction rather than blessing it - it is the
  // open question R382 could not settle from the tree, and the alternative fix
  // (judge consent on the UNCAPPED reported_soc, keeping the cap for 0x355
  // only) would make the two agree. Whichever way it is resolved, this test
  // must be looked at, which is the point of writing it down.
  datalayer.battery.status.reported_soc = SOFAR_MAX_REPORTED_SOC_PPTT;  // 99.00%, below the system full cutoff
  datalayer.battery.status.max_charge_current_dA = 500;
  datalayer.battery.status.max_discharge_current_dA = 500;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f351 = find_frame_with_id(0x351);
  ASSERT_NE(f351, nullptr);
  EXPECT_EQ(u16_le(f351->data.u8[4], f351->data.u8[5]), 500u) << "the charge limit still permits charge at this SoC";

  const CAN_frame* f30F = find_frame_with_id(0x30F);
  ASSERT_NE(f30F, nullptr);
  EXPECT_EQ(f30F->data.u8[1], 0x01u) << "while the consent flag forbids it - the two disagree at the cap (R382, open)";
}

TEST_F(SofarCanInverterTest, TheChargeOnlyThresholdIsOnePercentNotTwo) {
  // The bottom branch is soc_percent <= 1, and only soc 0 was covered. Its
  // boundary is untested in both directions, which is how the top branch came
  // to be wrong for a year - nobody pinned where it changed. 1.99% is still
  // charge-only; 2.00% is not.
  datalayer.battery.status.reported_soc = 199;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);
  const CAN_frame* low = find_frame_with_id(0x30F);
  ASSERT_NE(low, nullptr);
  EXPECT_EQ(low->data.u8[1], 0x02u) << "1.99% must still be charge-only";

  clear_transmitted_frames();
  datalayer.battery.status.reported_soc = 200;
  sofar->update_values();
  sofar->transmit_can(2 * INTERVAL_1_S + 2);
  const CAN_frame* mid = find_frame_with_id(0x30F);
  ASSERT_NE(mid, nullptr);
  EXPECT_EQ(mid->data.u8[1], 0x03u) << "2.00% must permit both";
}

TEST_F(SofarCanInverterTest, JustBelowFullStillAllowsCharging) {
  datalayer.battery.status.reported_soc = SOFAR_MAX_REPORTED_SOC_PPTT - 100;
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x30F);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[1], 0x03u) << "one percent below the cap must still permit charge";
}

// ── RX – aliveness and address filtering ─────────────────────────────────────

TEST_F(SofarCanInverterTest, KnownRxFrames605And705SetAliveness) {
  for (uint32_t id : {0x605u, 0x705u}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    // Address frame to battery ID 0 (default).
    uint8_t tid = datalayer.battery.settings.sofar_user_specified_battery_id;
    CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = id, .data = {tid, 0, 0, 0, 0, 0, 0, 0}};
    sofar->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 2u)
        << "ID 0x" << std::hex << id << " must refresh aliveness";
  }
}

TEST_F(SofarCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = true, .DLC = 8, .ID = 0x400, .data = {0}};
  sofar->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0u);
}

// ── RX – 0x605 query-response routing ────────────────────────────────────────

TEST_F(SofarCanInverterTest, Query605Inquiry0ResponseIs670) {
  datalayer.battery.settings.sofar_user_specified_battery_id = 0;
  send_605(0, 0x00);
  EXPECT_EQ(count_frames_with_id(0x670), 1u);
}

TEST_F(SofarCanInverterTest, Query605Inquiry1ResponseIs671) {
  send_605(0, 0x01);
  EXPECT_EQ(count_frames_with_id(0x671), 1u);
}

TEST_F(SofarCanInverterTest, Query605Inquiry2ResponseIs680) {
  send_605(0, 0x02);
  EXPECT_EQ(count_frames_with_id(0x680), 1u);
}

TEST_F(SofarCanInverterTest, Query605Inquiry0AResponseIs690) {
  send_605(0, 0x0A);
  EXPECT_EQ(count_frames_with_id(0x690), 1u);
}

TEST_F(SofarCanInverterTest, Query605Inquiry0EResponseIs6C0) {
  send_605(0, 0x0E);
  EXPECT_EQ(count_frames_with_id(0x6C0), 1u);
}

TEST_F(SofarCanInverterTest, Query605WrongBatteryIdIgnored) {
  datalayer.battery.settings.sofar_user_specified_battery_id = 0;
  send_605(1 /*wrong id*/, 0x00);
  EXPECT_EQ(count_frames_with_id(0x670), 0u) << "Query addressed to wrong battery ID must produce no reply";
}

TEST_F(SofarCanInverterTest, Query605UnknownInquiryProducesNoReply) {
  send_605(0, 0xFF);
  // Total frame count should be 0 (only the TX frames we triggered matter).
  EXPECT_EQ(get_transmitted_frames().size(), 0u) << "Unsupported inquiry must not produce a reply";
}

// ── RX – 0x705 history query-response routing ────────────────────────────────

TEST_F(SofarCanInverterTest, Query705Inquiry0ResponseIs770) {
  send_705(0, 0x00);
  EXPECT_EQ(count_frames_with_id(0x770), 1u);
}

TEST_F(SofarCanInverterTest, Query705Inquiry4ResponseIs780) {
  send_705(0, 0x04);
  EXPECT_EQ(count_frames_with_id(0x780), 1u);
}

TEST_F(SofarCanInverterTest, Query705WrongBatteryIdIgnored) {
  datalayer.battery.settings.sofar_user_specified_battery_id = 2;
  send_705(3 /*wrong id*/, 0x00);
  EXPECT_EQ(count_frames_with_id(0x770), 0u);
}

// ── Battery ID / setup() – CAN ID offset ─────────────────────────────────────

TEST_F(SofarCanInverterTest, BatteryIdZeroUsesBaseCanIds) {
  datalayer.battery.settings.sofar_user_specified_battery_id = 0;
  // setup_inverter() was already called in SetUp().  Re-call with id=0
  // (default) to verify base IDs are applied.
  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  const CAN_frame* f = find_frame_with_id(0x351);
  ASSERT_NE(f, nullptr) << "With battery_id=0, frame ID must be 0x351 (no offset)";
}

TEST_F(SofarCanInverterTest, BatteryIdOneShiftsCanIdBy0x1000) {
  // Recreate driver with battery_id=1 so setup() applies the offset.
  datalayer.battery.settings.sofar_user_specified_battery_id = 1;
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_protocol = InverterProtocolType::Sofar;
  setup_inverter();
  ASSERT_NE(inverter, nullptr);
  sofar = static_cast<SofarInverter*>(inverter);
  clear_transmitted_frames();

  sofar->update_values();
  sofar->transmit_can(INTERVAL_1_S + 1);

  // base_offset = 1 << 12 = 0x1000; 0x351 + 0x1000 = 0x1351
  EXPECT_EQ(count_frames_with_id(0x1351), 1u) << "With battery_id=1, frame 0x351 must be sent as 0x1351";
  EXPECT_EQ(count_frames_with_id(0x351), 0u) << "Base ID 0x351 must not appear when offset is applied";

  // Restore to default battery_id for other tests.
  datalayer.battery.settings.sofar_user_specified_battery_id = 0;
}

// ── Response stamp: payload carries PACK ID in byte 0 ────────────────────────

TEST_F(SofarCanInverterTest, Query605ResponseStampsBatteryIdInByte0) {
  datalayer.battery.settings.sofar_user_specified_battery_id = 0;
  send_605(0, 0x00);
  const CAN_frame* f = find_frame_with_id(0x670);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0u) << "Response byte 0 must echo the PACK ID";
}
