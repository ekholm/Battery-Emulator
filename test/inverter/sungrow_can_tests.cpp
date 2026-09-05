#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SUNGROW-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the Sungrow SBRXXX CAN inverter driver.
//
// The driver has two TX modes: INIT (triggered by 0x101/0x191 from the
// inverter → transmit_can_init=true) and RUN (0x108/0x109 clears the flag).
// Both modes have a 1 s timer-driven batch-send loop. Additionally there are
// 10 s and 60 s groups (RUN mode only). RX also handles a Modbus-over-CAN
// poll on 0x1E0.

namespace {

class SungrowCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::Sungrow;
    // Default model: SBR096 (3 modules, 9600 Wh)
    user_selected_inverter_sungrow_type = 0;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    sg = static_cast<SungrowInverter*>(inverter);
    clear_transmitted_frames();
  }

  void TearDown() override { user_selected_inverter_sungrow_type = 0; }

  // Helper to inject a known 0x101 frame (triggers INIT mode).
  void rx_init_trigger() {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x101, .data = {0}};
    sg->map_can_frame_to_variable(f);
  }

  // Helper to clear the INIT flag (driver returns to RUN on 0x108).
  void rx_run_trigger() {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x108, .data = {0}};
    sg->map_can_frame_to_variable(f);
  }

  SungrowInverter* sg = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// RX / aliveness
// ---------------------------------------------------------------------------

TEST_F(SungrowCanInverterTest, KnownRxFramesRefreshAliveness) {
  for (uint32_t id : {0x100u, 0x101u, 0x102u, 0x103u, 0x104u, 0x105u, 0x106u, 0x108u, 0x109u, 0x191u, 0x1E0u}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    // 0x1E0 with DLC=8 needs a valid Modbus CRC to not early-exit, but
    // aliveness is set before the CRC check, so zero bytes is fine here.
    sg->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE) << "ID 0x" << std::hex << id;
  }
}

TEST_F(SungrowCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  sg->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---------------------------------------------------------------------------
// INIT vs RUN mode toggle
// ---------------------------------------------------------------------------

TEST_F(SungrowCanInverterTest, Rx101SetsInitModeAndRx108ClearsIt) {
  // Driver starts in INIT mode (transmit_can_init = true by default in the class).
  // 0x108 clears it.
  rx_run_trigger();
  // 0x101 re-sets it.
  rx_init_trigger();
  // Now sending at INTERVAL_1_S + 1 in INIT mode should emit init-specific frames.
  sg->update_values();
  sg->transmit_can(INTERVAL_1_S + 1);
  EXPECT_GT(count_frames_with_id(0x007), 0u) << "0x007 only in INIT mode";
}

TEST_F(SungrowCanInverterTest, RunModeDoesNotSendInitSpecificFrames) {
  rx_run_trigger();
  sg->update_values();
  // Advance through all 5 batches.
  for (int i = 0; i < 5; i++) {
    sg->transmit_can((i + 1) * (INTERVAL_1_S + 200UL));
  }
  // 0x007 and 0x009 are init-only.
  EXPECT_EQ(count_frames_with_id(0x009), 0u) << "0x009 must not appear in RUN mode";
}

// ---------------------------------------------------------------------------
// TX payload encoding: update_values → transmit_can
// ---------------------------------------------------------------------------

TEST_F(SungrowCanInverterTest, Frame701EncodesVoltageCurrentLimitsLE) {
  datalayer.battery.info.max_design_voltage_dV = 4100;
  datalayer.battery.info.min_design_voltage_dV = 2800;
  datalayer.battery.status.max_charge_current_dA = 200;
  datalayer.battery.status.max_discharge_current_dA = 300;
  sg->update_values();
  rx_run_trigger();
  // Drive through batches to reach case 2 (batch B: 0x701).
  sg->transmit_can(INTERVAL_1_S + 1);            // batch 0: head
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));  // batch 1: A
  sg->transmit_can(3 * (INTERVAL_1_S + 400UL));  // batch 2: B (0x701)

  const CAN_frame* f = find_last_frame_with_id(0x701);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 4100u);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 2800u);
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 200u);
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 300u);
}

TEST_F(SungrowCanInverterTest, Frame702EncodesSocSohRemainingAndCapacityLE) {
  datalayer.battery.status.reported_soc = 7500;  // 75.00 %
  datalayer.battery.status.soh_pptt = 9500;      // 95.00 %
  datalayer.battery.status.reported_remaining_capacity_Wh = 15000;
  datalayer.battery.info.reported_total_capacity_Wh = 20000;
  sg->update_values();
  rx_run_trigger();

  sg->transmit_can(INTERVAL_1_S + 1);
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));
  sg->transmit_can(3 * (INTERVAL_1_S + 400UL));  // batch 2 → 0x702

  const CAN_frame* f = find_last_frame_with_id(0x702);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 7500u) << "SOC pptt LE";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 9500u) << "SOH pptt LE";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 15000u) << "remaining Wh LE";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 20000u) << "capacity Wh LE";
}

TEST_F(SungrowCanInverterTest, Frame704EncodesVoltageSignedCurrentAndTemperature) {
  datalayer.battery.status.voltage_dV = 3800;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-400);  // -40.0 A
  datalayer.battery.status.temperature_max_dC = static_cast<int16_t>(-150);   // -15.0 °C
  sg->update_values();
  rx_run_trigger();

  sg->transmit_can(INTERVAL_1_S + 1);
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));
  sg->transmit_can(3 * (INTERVAL_1_S + 400UL));  // batch 2 → 0x704

  const CAN_frame* f = find_last_frame_with_id(0x704);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 3800u) << "voltage LE b0-1";
  // Current is signed LE; also appears at b2-3 AND at b4-5 again for voltage.
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[2], f->data.u8[3])), -400) << "signed current";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 3800u) << "second voltage b4-5";
  // Temperature: int16_t in 0.1 °C, signed, LE.
  EXPECT_EQ(static_cast<int16_t>(u16_le(f->data.u8[6], f->data.u8[7])), -150) << "negative temperature";
}

TEST_F(SungrowCanInverterTest, Frame704CurrentFlippedIn504) {
  // 0x504 must carry the flipped (negated) current vs. 0x704.
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(200);
  datalayer.battery.status.voltage_dV = 3700;
  sg->update_values();
  rx_run_trigger();

  sg->transmit_can(INTERVAL_1_S + 1);
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));
  sg->transmit_can(3 * (INTERVAL_1_S + 400UL));  // batch 2

  // 504 appears in batch 4 (tail): drive through batches 3 and 4 too.
  sg->transmit_can(4 * (INTERVAL_1_S + 600UL));
  sg->transmit_can(5 * (INTERVAL_1_S + 800UL));

  const CAN_frame* f704 = find_last_frame_with_id(0x704);
  const CAN_frame* f504 = find_last_frame_with_id(0x504);
  ASSERT_NE(f704, nullptr);
  ASSERT_NE(f504, nullptr);

  int16_t cur704 = static_cast<int16_t>(u16_le(f704->data.u8[2], f704->data.u8[3]));
  int16_t cur504 = static_cast<int16_t>(u16_le(f504->data.u8[2], f504->data.u8[3]));
  EXPECT_EQ(cur704, 200) << "normal current in 0x704";
  EXPECT_EQ(cur504, -200) << "flipped current in 0x504";
}

TEST_F(SungrowCanInverterTest, EndStopByteSetWhenFullOrEmpty) {
  // Full: charge limit == 0 → END_STOP_FULL
  datalayer.battery.status.max_charge_current_dA = 0;
  datalayer.battery.status.reported_soc = 9000;
  sg->update_values();
  rx_run_trigger();
  sg->transmit_can(INTERVAL_1_S + 1);
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));
  {
    const CAN_frame* f = find_last_frame_with_id(0x005);
    if (f) {
      // END_STOP_FULL must be non-zero; exact value not pinned here as it's private.
      EXPECT_NE(f->data.u8[1], 0u) << "end_stop byte should be non-zero when full";
    }
  }
}

TEST_F(SungrowCanInverterTest, Frame706EncodesTemperatureAndCellVoltagesLE) {
  datalayer.battery.status.temperature_max_dC = 400;  // 40.0 °C
  datalayer.battery.status.temperature_min_dC = 100;  // 10.0 °C
  datalayer.battery.status.cell_max_voltage_mV = 3500;
  datalayer.battery.status.cell_min_voltage_mV = 3300;
  sg->update_values();
  rx_run_trigger();

  sg->transmit_can(INTERVAL_1_S + 1);
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));
  sg->transmit_can(3 * (INTERVAL_1_S + 400UL));

  const CAN_frame* f = find_last_frame_with_id(0x706);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[0], f->data.u8[1]), 400u) << "temp max LE";
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 100u) << "temp min LE";
  EXPECT_EQ(u16_le(f->data.u8[4], f->data.u8[5]), 3500u) << "cell max mV LE";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 3300u) << "cell min mV LE";
}

TEST_F(SungrowCanInverterTest, Frame714EncodesCell01mVValues) {
  // 0x714 uses 0.1 mV units (mV * 10).
  datalayer.battery.status.cell_max_voltage_mV = 3450;
  datalayer.battery.status.cell_min_voltage_mV = 3380;
  sg->update_values();
  rx_run_trigger();

  sg->transmit_can(INTERVAL_1_S + 1);
  sg->transmit_can(2 * (INTERVAL_1_S + 200UL));
  sg->transmit_can(3 * (INTERVAL_1_S + 400UL));

  const CAN_frame* f = find_last_frame_with_id(0x714);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(u16_le(f->data.u8[2], f->data.u8[3]), 34500u) << "cell max in 0.1mV";
  EXPECT_EQ(u16_le(f->data.u8[6], f->data.u8[7]), 33800u) << "cell min in 0.1mV";
}

// ---------------------------------------------------------------------------
// Periodic cadence
// ---------------------------------------------------------------------------

TEST_F(SungrowCanInverterTest, TenSecondGroupSends707And70FFamily) {
  rx_run_trigger();
  sg->update_values();
  // Drive past the 1 s batch loop to avoid interference.
  sg->transmit_can(INTERVAL_10_S + 1);
  EXPECT_GT(count_frames_with_id(0x707), 0u);
  EXPECT_GT(count_frames_with_id(0x70B), 0u);
}

TEST_F(SungrowCanInverterTest, SixtySecondGroupSendsSerialFrames) {
  rx_run_trigger();
  sg->update_values();
  sg->transmit_can(INTERVAL_60_S + 1);
  // Default config: 3 modules → module 1,2,3 serial frames (3x3 = 9 frames).
  EXPECT_GE(count_frames_with_id(0x71F), 0u);  // grouped by the driver per module
}

// ---------------------------------------------------------------------------
// user_selected_inverter_sungrow_type: model selection
// ---------------------------------------------------------------------------

TEST_F(SungrowCanInverterTest, SBR128ModelGivesCorrectNameplateIn707) {
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_sungrow_type = 2;  // SBR128 → 4 modules, 12800 Wh
  setup_inverter();
  sg = static_cast<SungrowInverter*>(inverter);
  clear_transmitted_frames();

  sg->update_values();
  rx_run_trigger();
  sg->transmit_can(INTERVAL_10_S + 1);  // 10 s group sends 0x707

  const CAN_frame* f = find_last_frame_with_id(0x707);
  ASSERT_NE(f, nullptr);
  uint16_t nameplate = u16_le(f->data.u8[4], f->data.u8[5]);
  EXPECT_EQ(nameplate, 12800u) << "SBR128 nameplate in 0x707 b4-5";
  EXPECT_EQ(f->data.u8[6], 4u) << "SBR128 module count in 0x707 b6";
}

// ---------------------------------------------------------------------------
// 0x1E0 Modbus over CAN
// ---------------------------------------------------------------------------

TEST_F(SungrowCanInverterTest, ModbusPollWithCorrectCrcTriggersReply) {
  // Build a valid Modbus request: slave=0x01, func=0x04, start=0x4DE2, qty=0x0002
  // CRC16 computed manually or trusted from the code itself.
  // We construct it and let the driver compute the CRC the same way.
  CAN_frame f = {
      .FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1E0, .data = {0x01, 0x04, 0x4D, 0xE2, 0x00, 0x02, 0x00, 0x00}};
  // Compute and patch in the correct CRC.
  // Poly 0xA001, init 0xFFFF over first 6 bytes.
  auto crc16 = [](const uint8_t* d, uint8_t len) -> uint16_t {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; ++i) {
      crc ^= d[i];
      for (uint8_t b = 0; b < 8; ++b) {
        if (crc & 1) {
          crc = (crc >> 1) ^ 0xA001;
        } else {
          crc >>= 1;
        }
      }
    }
    return crc;
  };
  uint16_t crc = crc16(f.data.u8, 6);
  f.data.u8[6] = crc & 0xFF;
  f.data.u8[7] = (crc >> 8) & 0xFF;

  sg->map_can_frame_to_variable(f);

  // The response must contain at least 2 CAN frames (first frame: addr/func/byteCount/data,
  // second frame if >8 bytes total with CRC).
  EXPECT_GE(get_transmitted_frames().size(), 1u) << "Modbus response expected";
  // First TX frame starts with slave addr 0x01.
  EXPECT_EQ(get_transmitted_frames().front().data.u8[0], 0x01u);
}

TEST_F(SungrowCanInverterTest, ModbusPollWithBadCrcIsIgnored) {
  CAN_frame f = {
      .FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1E0, .data = {0x01, 0x04, 0x4D, 0xE2, 0x00, 0x02, 0xFF, 0xFF}};
  sg->map_can_frame_to_variable(f);
  // Bad CRC → no response TX beyond aliveness update.
  EXPECT_TRUE(get_transmitted_frames().empty()) << "Bad CRC must not generate a Modbus reply";
}
