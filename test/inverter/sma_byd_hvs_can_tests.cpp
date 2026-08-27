#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/SMA-BYD-HVS-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the SMA compatible BYD Battery-Box HVS CAN inverter driver.
//
// This driver uses a queue-based sequencer: frames are pushed into framesToSend[]
// and drained one-per-250ms window.  Pairing (0x5E7 / 0x660) calls
// transmit_can_init() which clears the queue and pushes 12 init frames; the
// last one carries a callback that sets pairing_completed = true.  Only after
// pairing_completed are the 2s/10s/60s periodic paths active.
//
// The driver is gated hard by inverter_allows_contactor_closing: if that flag
// is false, transmit_can() returns immediately.
//
// Unlike the H variant, HVS has no 0x158 error-byte frame.

namespace {

class SmaBydHvsCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already reset datalayer/events and destroyed
    // the previous inverter instance before this runs.
    user_selected_inverter_protocol = InverterProtocolType::SmaBydHvs;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    hvs = static_cast<SmaBydHvsInverter*>(inverter);
    // SmaInverterBase::setup() resets inverter_allows_contactor_closing to
    // false.  The HVS transmit_can() hard-returns when it is false, so set it
    // true here to let all tests exercise the real frame-sequencing logic.
    // SilentWhenContactorEnableIsLow overrides this to false explicitly.
    datalayer.system.status.inverter_allows_contactor_closing = true;
    clear_transmitted_frames();
  }

  // Trigger the pairing handshake.
  void send_pairing_frame(uint32_t id = 0x5E7) {
    CAN_frame pair = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    hvs->map_can_frame_to_variable(pair);
  }

  // Drain all 12 init frames from the queue (one per 250ms window).
  // Returns the millis timestamp just after the last drain call.
  unsigned long drain_init_queue(unsigned long start_ms = INTERVAL_250_MS + 1) {
    unsigned long t = start_ms;
    for (int i = 0; i < 12; i++) {
      hvs->transmit_can(t);
      t += INTERVAL_250_MS;
    }
    return t;
  }

  SmaBydHvsInverter* hvs = nullptr;
};

}  // namespace

// ── TX gating ────────────────────────────────────────────────────────────────

TEST_F(SmaBydHvsCanInverterTest, SilentWhenContactorEnableIsLow) {
  datalayer.system.status.inverter_allows_contactor_closing = false;
  send_pairing_frame();
  hvs->transmit_can(INTERVAL_250_MS + 1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Driver must not transmit while inverter_allows_contactor_closing is false";
}

TEST_F(SmaBydHvsCanInverterTest, SilentBeforePairingEvenWhenContactorEnabled) {
  // inverter_allows_contactor_closing = true (default), but no pairing yet
  // and pairing_completed = false → 2s/10s/60s paths are blocked.
  // The queue is also empty (no pairing), so nothing is sent.
  hvs->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty())
      << "Driver must not transmit before a pairing frame is received";
}

// ── Pairing handshake ─────────────────────────────────────────────────────────

TEST_F(SmaBydHvsCanInverterTest, PairingRequest5E7DrainsTwelveInitFrames) {
  send_pairing_frame(0x5E7);
  drain_init_queue();
  // The 12 queued frames: 558, 598, 5D8, 618×4, 358, 3D8, 458, 4D8, 518.
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x598), 1u);
  EXPECT_EQ(count_frames_with_id(0x5D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x618), 4u);
  EXPECT_EQ(count_frames_with_id(0x358), 1u);
  EXPECT_EQ(count_frames_with_id(0x3D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x458), 1u);
  EXPECT_EQ(count_frames_with_id(0x4D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x518), 1u);
}

TEST_F(SmaBydHvsCanInverterTest, PairingRequest660AlsoWorks) {
  send_pairing_frame(0x660);
  drain_init_queue();
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x618), 4u);
}

TEST_F(SmaBydHvsCanInverterTest, SecondPairingRestartsQueue) {
  send_pairing_frame();
  // Drain only partially.
  hvs->transmit_can(INTERVAL_250_MS + 1);
  clear_transmitted_frames();

  // Second pairing clears the queue and restarts with fresh 12 frames.
  send_pairing_frame(0x660);
  drain_init_queue();
  // Should still produce exactly one each of the pairing frames.
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
}

TEST_F(SmaBydHvsCanInverterTest, OnlyOneFrameSentPerTwoFiftyMsWindow) {
  send_pairing_frame();
  // First window → only frame 0 (558) sent.
  hvs->transmit_can(INTERVAL_250_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x598), 0u)
      << "Queue must emit at most one frame per 250ms window";
}

TEST_F(SmaBydHvsCanInverterTest, PairingCompletedAfterQueueDrained) {
  send_pairing_frame();
  unsigned long t = drain_init_queue();

  // After queue drain, pairing_completed = true → periodic paths fire.
  clear_transmitted_frames();
  hvs->transmit_can(t + INTERVAL_2_S);
  // 2s path pushes a 358 frame into the (now empty) queue; next 250ms drains it.
  hvs->transmit_can(t + INTERVAL_2_S + INTERVAL_250_MS + 1);
  EXPECT_GE(count_frames_with_id(0x358), 1u)
      << "2s periodic frame should appear after pairing_completed";
}

// ── TX payload – frame 0x358 (limits) ────────────────────────────────────────

TEST_F(SmaBydHvsCanInverterTest, LimitsFrameEncodesVoltagesAndCurrents) {
  datalayer.battery.info.max_design_voltage_dV = 4100;
  datalayer.battery.info.min_design_voltage_dV = 2900;
  datalayer.battery.status.max_discharge_current_dA = 400;
  datalayer.battery.status.max_charge_current_dA = 150;

  hvs->update_values();
  send_pairing_frame();
  // Drain the queue; 0x358 is frame 8 (index 7) in the queue.
  drain_init_queue();

  const CAN_frame* f = find_last_frame_with_id(0x358);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 4100u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 2900u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 400u);
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 150u);
}

// ── TX payload – frame 0x3D8 (SoC / SoH / Ah remaining) ─────────────────────

TEST_F(SmaBydHvsCanInverterTest, SocSohAhFrameEncodesCorrectly) {
  datalayer.battery.status.reported_soc = 8500;  // 85.00 %
  datalayer.battery.status.soh_pptt = 9750;      // 97.50 %
  // Ah = (25000 / 3500) * 100: integer division → 7 * 100 = 700
  datalayer.battery.status.reported_remaining_capacity_Wh = 25000;
  datalayer.battery.status.voltage_dV = 3500;

  hvs->update_values();
  send_pairing_frame();
  drain_init_queue();

  const CAN_frame* f = find_last_frame_with_id(0x3D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 8500u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 9750u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 700u);
}

// ── TX payload – frame 0x4D8 (voltage / current / temp / ready) ──────────────

TEST_F(SmaBydHvsCanInverterTest, BatteryInfoFrameEncodesVoltageSignedCurrentAndTemp) {
  datalayer.battery.status.voltage_dV = 3850;
  // Negative current (discharge): -500 = 0xFE0C
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-500);
  datalayer.battery.status.temperature_max_dC = 280;  // 28.0 °C
  datalayer.battery.status.temperature_min_dC = 220;  // 22.0 °C average = 250

  hvs->update_values();
  send_pairing_frame();
  drain_init_queue();

  const CAN_frame* f = find_last_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3850u);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -500);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[4], f->data.u8[5])), 250);
}

TEST_F(SmaBydHvsCanInverterTest, FaultStatusSetsByte6ToStopState) {
  datalayer.system.status.system_status = FAULT;
  hvs->update_values();
  send_pairing_frame();
  drain_init_queue();

  const CAN_frame* f = find_last_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x02u);  // STOP_STATE
}

// ── TX payload – frame 0x518 (temperatures / voltage / cell voltages) ─────────

TEST_F(SmaBydHvsCanInverterTest, TemperatureFrameEncodesMinMaxAndCells) {
  datalayer.battery.status.temperature_max_dC = 320;
  datalayer.battery.status.temperature_min_dC = -100;  // signed negative
  datalayer.battery.status.voltage_dV = 3600;
  datalayer.battery.status.cell_min_voltage_mV = 3600;  // 3600 / 25 = 144
  datalayer.battery.status.cell_max_voltage_mV = 4000;  // 4000 / 25 = 160

  hvs->update_values();
  send_pairing_frame();
  drain_init_queue();

  const CAN_frame* f = find_last_frame_with_id(0x518);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[0], f->data.u8[1])), 320);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -100);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 3600u);
  EXPECT_EQ(f->data.u8[6], 144u);
  EXPECT_EQ(f->data.u8[7], 160u);
}

// ── TX payload – frame 0x458 (lifetime energy counters) ──────────────────────

TEST_F(SmaBydHvsCanInverterTest, EnergyCounterFrameEncodesChargedAndDischarged) {
  datalayer.battery.status.total_charged_battery_Wh = 0x00ABCDEF;
  datalayer.battery.status.total_discharged_battery_Wh = 0x00123456;

  hvs->update_values();
  send_pairing_frame();
  drain_init_queue();

  const CAN_frame* f = find_last_frame_with_id(0x458);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x00u);
  EXPECT_EQ(f->data.u8[1], 0xABu);
  EXPECT_EQ(f->data.u8[2], 0xCDu);
  EXPECT_EQ(f->data.u8[3], 0xEFu);
  EXPECT_EQ(f->data.u8[4], 0x00u);
  EXPECT_EQ(f->data.u8[5], 0x12u);
  EXPECT_EQ(f->data.u8[6], 0x34u);
  EXPECT_EQ(f->data.u8[7], 0x56u);
}

// ── Periodic cadence after pairing ───────────────────────────────────────────

TEST_F(SmaBydHvsCanInverterTest, TwoSecondPeriodicPushesLimitsFrame) {
  send_pairing_frame();
  unsigned long t = drain_init_queue();
  clear_transmitted_frames();

  // 2s periodic pushes 0x358 into queue; drain it.
  hvs->transmit_can(t + INTERVAL_2_S + 1);
  hvs->transmit_can(t + INTERVAL_2_S + 1 + INTERVAL_250_MS + 1);
  EXPECT_GE(count_frames_with_id(0x358), 1u);
  // 10s and 60s not yet due.
  EXPECT_EQ(count_frames_with_id(0x518), 0u);
  EXPECT_EQ(count_frames_with_id(0x458), 0u);
}

TEST_F(SmaBydHvsCanInverterTest, TenSecondPeriodicPushesThreeFrames) {
  send_pairing_frame();
  unsigned long t = drain_init_queue();
  clear_transmitted_frames();

  // At t10 the 2s path also fires (previously-zero timer), so the queue
  // receives 4 frames: [358, 518, 4D8, 3D8].  5 transmit_can calls are
  // needed: the first pushes, then each subsequent drains one.
  unsigned long t10 = t + INTERVAL_10_S + 1;
  hvs->transmit_can(t10);                                         // push 4
  hvs->transmit_can(t10 + 1 * (INTERVAL_250_MS + 1));            // drain 358
  hvs->transmit_can(t10 + 2 * (INTERVAL_250_MS + 1));            // drain 518
  hvs->transmit_can(t10 + 3 * (INTERVAL_250_MS + 1));            // drain 4D8
  hvs->transmit_can(t10 + 4 * (INTERVAL_250_MS + 1));            // drain 3D8
  EXPECT_GE(count_frames_with_id(0x518), 1u);
  EXPECT_GE(count_frames_with_id(0x4D8), 1u);
  EXPECT_GE(count_frames_with_id(0x3D8), 1u);
}

// ── RX – aliveness ────────────────────────────────────────────────────────────

TEST_F(SmaBydHvsCanInverterTest, KnownRxFramesRefreshAliveness) {
  for (uint32_t id : {0x360u, 0x3E0u, 0x420u, 0x560u,
                      0x5E0u, 0x5E7u, 0x660u}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    hvs->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 3u)
        << "ID 0x" << std::hex << id << " should refresh aliveness";
  }
}

TEST_F(SmaBydHvsCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  hvs->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0u);
}

// ── Safety-critical: contactor control ───────────────────────────────────────

TEST_F(SmaBydHvsCanInverterTest, AllowsContactorClosingReadsGpioReturnsFalse) {
  // digitalRead always returns 0 in the emulation.
  EXPECT_FALSE(hvs->allows_contactor_closing());
}

TEST_F(SmaBydHvsCanInverterTest, ControlsContactorReturnsTrue) {
  EXPECT_TRUE(hvs->controls_contactor());
}

TEST_F(SmaBydHvsCanInverterTest, NeedsCanStartupGraceReturnsTrue) {
  EXPECT_TRUE(hvs->needs_can_startup_grace());
}
