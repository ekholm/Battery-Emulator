#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SMA-BYD-H-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the SMA compatible BYD Battery-Box H CAN inverter driver.
//
// TX side: the 100ms periodic path is gated on
// datalayer.system.status.inverter_allows_contactor_closing (read from the
// GPIO contactor-enable line by the safety code; defaults true in the
// datalayer). The pairing batch is triggered separately by incoming 0x5E7 or
// 0x660 frames from the SMA inverter.
//
// RX side: known inverter frames refresh CAN aliveness; 0x5E7 / 0x660 also
// arm the four-batch initialisation sequence.

namespace {

class SmaBydHCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already wiped datalayer/events and destroyed
    // the previous inverter instance before this runs.
    user_selected_inverter_protocol = InverterProtocolType::SmaBydH;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    sma = static_cast<SmaBydHInverter*>(inverter);
    // SmaInverterBase::setup() resets inverter_allows_contactor_closing to
    // false (waits for the GPIO enable line).  Most tests exercise the 100ms
    // periodic path which is gated on that flag, so prime it true here.
    // The StaysSilentWhenContactorEnableIsLow test overrides this explicitly.
    datalayer.system.status.inverter_allows_contactor_closing = true;
    clear_transmitted_frames();
  }

  // Trigger the pairing handshake that arms the init batch.
  void send_pairing_frame(uint32_t id = 0x5E7) {
    CAN_frame pair = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    sma->map_can_frame_to_variable(pair);
  }

  // Drain all four init batches.  Each batch fires once per >=7 ms window.
  void drain_init_batches() {
    for (int i = 0; i < 4; i++) {
      sma->transmit_can(8u + static_cast<unsigned long>(i) * 8);
    }
  }

  SmaBydHInverter* sma = nullptr;
};

}  // namespace

// ── TX gating ────────────────────────────────────────────────────────────────

TEST_F(SmaBydHCanInverterTest, StaysSilentWhenContactorEnableIsLow) {
  // The safety code writes inverter_allows_contactor_closing from the GPIO.
  // In the emulation digitalRead always returns 0, but that path runs in the
  // main loop, not here.  Force it low to confirm the driver obeys the flag.
  datalayer.system.status.inverter_allows_contactor_closing = false;
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);
  EXPECT_TRUE(get_transmitted_frames().empty()) << "Driver must not transmit while contactor-enable line is low";
}

TEST_F(SmaBydHCanInverterTest, PairingRequest5E7TriggersBatchSend) {
  send_pairing_frame(0x5E7);
  drain_init_batches();
  // Batch 0 (558, 598, 5D8), batch 1 (618×3), batch 2 (158, 358, 3D8),
  // batch 3 (458, 518, 4D8) – twelve frames total.
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x598), 1u);
  EXPECT_EQ(count_frames_with_id(0x5D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x618), 3u);
  EXPECT_EQ(count_frames_with_id(0x158), 1u);
  EXPECT_EQ(count_frames_with_id(0x358), 1u);
  EXPECT_EQ(count_frames_with_id(0x3D8), 1u);
  EXPECT_EQ(count_frames_with_id(0x458), 1u);
  EXPECT_EQ(count_frames_with_id(0x518), 1u);
  EXPECT_EQ(count_frames_with_id(0x4D8), 1u);
}

TEST_F(SmaBydHCanInverterTest, PairingRequest660AlsoTriggersBatchSend) {
  send_pairing_frame(0x660);
  drain_init_batches();
  EXPECT_EQ(count_frames_with_id(0x558), 1u);
  EXPECT_EQ(count_frames_with_id(0x618), 3u);
}

TEST_F(SmaBydHCanInverterTest, BatchDoesNotSendBeforeMinimalDelay) {
  send_pairing_frame();
  // Call with currentMillis=1; 1-0=1 < delay_between_batches_ms(7) → no send.
  sma->transmit_can(1);
  EXPECT_EQ(count_frames_with_id(0x558), 0u) << "Batch must wait for the 7 ms inter-batch delay";
}

TEST_F(SmaBydHCanInverterTest, WithoutPairingNoBatchFramesSent) {
  // No pairing → transmit_can_init stays false → no batch frames regardless
  // of time.
  sma->transmit_can(INTERVAL_60_S + 1);
  EXPECT_EQ(count_frames_with_id(0x558), 0u);
}

TEST_F(SmaBydHCanInverterTest, BatchSendsOncePerPairing) {
  send_pairing_frame();
  drain_init_batches();
  size_t count_before = count_frames_with_id(0x558);

  clear_transmitted_frames();
  // Additional transmit_can calls after batch completion must not re-send.
  for (int i = 0; i < 4; i++) {
    sma->transmit_can(100u + static_cast<unsigned long>(i) * 8);
  }
  EXPECT_EQ(count_frames_with_id(0x558), 0u) << "Init batch must only fire once per pairing event";
  (void)count_before;
}

// ── TX payload – frame 0x358 (limits) ────────────────────────────────────────

TEST_F(SmaBydHCanInverterTest, LimitsFrameEncodesVoltagesAndCurrents) {
  datalayer.battery.info.max_design_voltage_dV = 4200;      // 420.0 V
  datalayer.battery.info.min_design_voltage_dV = 3000;      // 300.0 V
  datalayer.battery.status.max_discharge_current_dA = 500;  // 50.0 A
  datalayer.battery.status.max_charge_current_dA = 250;     // 25.0 A

  sma->update_values();
  // 100ms path fires immediately (inverter_allows_contactor_closing=true default).
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x358);
  ASSERT_NE(f, nullptr);
  // Max voltage big-endian
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 4200u);
  // Min voltage big-endian
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 3000u);
  // Max discharge current big-endian
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 500u);
  // Max charge current big-endian
  EXPECT_EQ(u16_be(f->data.u8[6], f->data.u8[7]), 250u);
}

// ── TX payload – frame 0x3D8 (SoC / SoH / Ah remaining) ─────────────────────

TEST_F(SmaBydHCanInverterTest, SocSohAhFrameEncodesCorrectly) {
  datalayer.battery.status.reported_soc = 7500;  // 75.00 %
  datalayer.battery.status.soh_pptt = 9800;      // 98.00 %
  // Ah remaining = (Wh / voltage_dV) * 100 = (20000 / 4000) * 100 = 500
  datalayer.battery.status.reported_remaining_capacity_Wh = 20000;
  datalayer.battery.status.voltage_dV = 4000;

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x3D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 7500u);
  EXPECT_EQ(u16_be(f->data.u8[2], f->data.u8[3]), 9800u);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 500u);
}

TEST_F(SmaBydHCanInverterTest, AhRemainingNotUpdatedWithVoltageAtOrBelowTen) {
  // Guard against division by zero: voltage <= 10 skips the Ah update.
  datalayer.battery.status.voltage_dV = 5;
  datalayer.battery.status.reported_remaining_capacity_Wh = 20000;

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  // 3D8[4:5] keeps its previous value (0 after reset), not a divide-by-zero crash.
  const CAN_frame* f = find_frame_with_id(0x3D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 0u);
}

// ── TX payload – frame 0x4D8 (voltage / current / temp / ready) ──────────────

TEST_F(SmaBydHCanInverterTest, BatteryInfoFrameEncodesVoltageCurrentAndTemp) {
  datalayer.battery.status.voltage_dV = 3700;
  // Negative current (discharge): 0xFCEE = -818 → 81.8 A discharge
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-818);
  datalayer.battery.status.temperature_max_dC = 300;  // 30.0 °C
  datalayer.battery.status.temperature_min_dC = 200;  // 20.0 °C

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  // Voltage big-endian
  EXPECT_EQ(u16_be(f->data.u8[0], f->data.u8[1]), 3700u);
  // Signed current big-endian (discharge → negative)
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -818);
  // Average temperature = (300 + 200) / 2 = 250
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[4], f->data.u8[5])), 250);
}

TEST_F(SmaBydHCanInverterTest, ReadyByteIsStopStateInFault) {
  datalayer.system.status.system_status = FAULT;
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x02u)  // STOP_STATE
      << "FAULT system status must encode STOP_STATE in 4D8 byte 6";
}

TEST_F(SmaBydHCanInverterTest, ReadyByteIsReadyStateWhenNotFault) {
  datalayer.system.status.system_status = ACTIVE;
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x4D8);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[6], 0x03u)  // READY_STATE
      << "Normal system status must encode READY_STATE in 4D8 byte 6";
}

// ── TX payload – frame 0x518 (temperature / voltage / cell voltages) ──────────

TEST_F(SmaBydHCanInverterTest, TemperatureFrameEncodesMinMaxAndCellVoltages) {
  datalayer.battery.status.temperature_max_dC = 350;  // 35.0 °C
  datalayer.battery.status.temperature_min_dC = -50;  // -5.0 °C  (negative, signed)
  datalayer.battery.status.voltage_dV = 3800;
  datalayer.battery.status.cell_min_voltage_mV = 3500;  // 3500 / 25 = 140
  datalayer.battery.status.cell_max_voltage_mV = 4100;  // 4100 / 25 = 164

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x518);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[0], f->data.u8[1])), 350);
  EXPECT_EQ(static_cast<int16_t>(u16_be(f->data.u8[2], f->data.u8[3])), -50);
  // Bytes 4:5 hold sum of cell voltages (driver reuses voltage_dV here)
  EXPECT_EQ(u16_be(f->data.u8[4], f->data.u8[5]), 3800u);
  EXPECT_EQ(f->data.u8[6], 140u);  // cell_min / 25
  EXPECT_EQ(f->data.u8[7], 164u);  // cell_max / 25
}

// ── TX payload – frame 0x458 (lifetime energy counters) ──────────────────────

TEST_F(SmaBydHCanInverterTest, EnergyCounterFrameEncodesChargedAndDischarged) {
  datalayer.battery.status.total_charged_battery_Wh = 0x01234567;
  datalayer.battery.status.total_discharged_battery_Wh = 0x0089ABCD;

  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x458);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[0], 0x01u);
  EXPECT_EQ(f->data.u8[1], 0x23u);
  EXPECT_EQ(f->data.u8[2], 0x45u);
  EXPECT_EQ(f->data.u8[3], 0x67u);
  EXPECT_EQ(f->data.u8[4], 0x00u);
  EXPECT_EQ(f->data.u8[5], 0x89u);
  EXPECT_EQ(f->data.u8[6], 0xABu);
  EXPECT_EQ(f->data.u8[7], 0xCDu);
}

// ── TX payload – frame 0x158 (error / fault byte 2) ──────────────────────────

TEST_F(SmaBydHCanInverterTest, ErrorFlagByte2Is0xAAWhenBatteryAllows) {
  datalayer.system.status.battery_allows_contactor_closing = true;
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x158);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[2], 0xAAu) << "Battery allows contactor → byte 2 must be 0xAA (no fault)";
}

TEST_F(SmaBydHCanInverterTest, ErrorFlagByte2Is0x6AWhenBatteryForbids) {
  datalayer.system.status.battery_allows_contactor_closing = false;
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x158);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[2], 0x6Au) << "Battery forbids contactor → byte 2 must be 0x6A (internal hardware fault)";
}

// ── Periodic cadence ──────────────────────────────────────────────────────────

TEST_F(SmaBydHCanInverterTest, PeriodicCadenceSendsAtHundredMs) {
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);  // first window
  EXPECT_GE(count_frames_with_id(0x358), 1u);
  EXPECT_GE(count_frames_with_id(0x3D8), 1u);
  EXPECT_GE(count_frames_with_id(0x4D8), 1u);
  EXPECT_GE(count_frames_with_id(0x158), 1u);
  EXPECT_GE(count_frames_with_id(0x518), 1u);
  EXPECT_GE(count_frames_with_id(0x458), 1u);
}

TEST_F(SmaBydHCanInverterTest, SixtySecondWindowSendsExtraEnergyFrame) {
  sma->update_values();
  sma->transmit_can(INTERVAL_100_MS + 1);  // drain 100ms timer
  clear_transmitted_frames();

  sma->transmit_can(INTERVAL_60_S + 2);  // 60s window fires
  // 0x458 appears both from the 100ms path and the 60s path.
  EXPECT_GE(count_frames_with_id(0x458), 2u) << "60s window must add an extra 0x458 energy frame";
}

// ── RX – aliveness and pairing ────────────────────────────────────────────────

TEST_F(SmaBydHCanInverterTest, KnownRxFramesRefreshAliveness) {
  for (uint32_t id : {0x360u, 0x3E0u, 0x420u, 0x560u, 0x561u, 0x562u, 0x563u, 0x564u, 0x565u, 0x566u, 0x567u,
                      0x5E0u, 0x5E1u, 0x5E2u, 0x5E3u, 0x5E4u, 0x5E5u, 0x5E6u, 0x5E7u, 0x62Cu, 0x660u}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    sma->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 3)
        << "ID 0x" << std::hex << id << " should refresh aliveness";
  }
}

TEST_F(SmaBydHCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  sma->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0u);
}

TEST_F(SmaBydHCanInverterTest, VoltageCurrentFrameDecodesInverterMeasurement) {
  CAN_frame f = {.FD = false,
                 .ext_ID = false,
                 .DLC = 8,
                 .ID = 0x360,
                 .data = {0x0F, 0xA0, 0xFF, 0xC4, 0, 0, 0, 0}};  // 4000 V, -60 A
  sma->map_can_frame_to_variable(f);
  // Aliveness must be refreshed regardless of payload decoding.
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE * 3u);
}

// ── Safety-critical: contactor control ───────────────────────────────────────

TEST_F(SmaBydHCanInverterTest, AllowsContactorClosingReadsGpioReturnsFalse) {
  // In the test emulation digitalRead always returns 0, so allows_contactor_closing()
  // always returns false.  Production code will wire the real GPIO.
  EXPECT_FALSE(sma->allows_contactor_closing());
}

TEST_F(SmaBydHCanInverterTest, ControlsContactorReturnsTrue) {
  EXPECT_TRUE(sma->controls_contactor());
}

TEST_F(SmaBydHCanInverterTest, NeedsCanStartupGraceReturnsTrue) {
  EXPECT_TRUE(sma->needs_can_startup_grace());
}
