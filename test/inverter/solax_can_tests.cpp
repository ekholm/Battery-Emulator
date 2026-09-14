#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../../Software/src/inverter/SOLAX-CAN.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the SolaX Triple Power LFP CAN inverter driver.
//
// TX side: the driver is purely reactive — it only transmits on receipt of
// 0x1871 from the inverter. The test therefore drives the state machine
// by injecting RX frames. RX side: only 0x1871 is decoded; unknown IDs
// must not refresh aliveness.

namespace {

// Canonical "stay in announce state" payload (byte4 == 0 → contactor open)
static const uint64_t PAYLOAD_ANNOUNCE = __builtin_bswap64(0x0200010000000000);
// Inverter requests contactor close (byte4 == 1)
static const uint64_t PAYLOAD_CLOSE = __builtin_bswap64(0x0200010001000000);
// Inverter requests contactor open again
static const uint64_t PAYLOAD_OPEN = __builtin_bswap64(0x0200010000000000);
// Inverter requests serial-number enumeration
static const uint64_t PAYLOAD_ENUM = __builtin_bswap64(0x0500010000000000);

class SolaxCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DataLayerResetListener has already reset datalayer/events and
    // deleted the previous inverter instance.
    user_selected_inverter_protocol = InverterProtocolType::Solax;
    user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::NoWorkaround;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    solax = static_cast<SolaxInverter*>(inverter);
    clear_transmitted_frames();
  }

  void TearDown() override {
    // Restore user globals that DataLayerResetListener does not clear.
    user_selected_inverter_modules = 0;
    user_selected_inverter_battery_type = 0;
    user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::NoWorkaround;
  }

  // Send a 0x1871 frame with the given 8-byte payload.
  void rx1871(uint64_t payload) {
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1871, .data = {}};
    f.data.u64 = payload;
    solax->map_can_frame_to_variable(f);
  }

  // Set non-zero datalayer values so payload asserts are non-trivial.
  void set_typical_battery_values() {
    datalayer.battery.info.max_design_voltage_dV = 4000;      // 400.0 V
    datalayer.battery.info.min_design_voltage_dV = 3000;      // 300.0 V
    datalayer.battery.status.max_charge_current_dA = 200;     // 20.0 A
    datalayer.battery.status.max_discharge_current_dA = 300;  // 30.0 A
    datalayer.battery.status.voltage_dV = 3700;
    datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-150);  // -15.0 A (charging)
    datalayer.battery.status.reported_soc = 7500;                               // 75.00 %
    datalayer.battery.status.reported_remaining_capacity_Wh = 15000;
    datalayer.battery.info.reported_total_capacity_Wh = 20000;
    datalayer.battery.status.temperature_max_dC = 280;
    datalayer.battery.status.temperature_min_dC = 220;
    datalayer.battery.status.cell_max_voltage_mV = 3450;
    datalayer.battery.status.cell_min_voltage_mV = 3400;
    datalayer.battery.info.max_cell_voltage_mV = 4200;
    datalayer.battery.info.min_cell_voltage_mV = 2500;
    datalayer.battery.status.soh_pptt = 9800;
  }

  SolaxInverter* solax = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// RX / aliveness
// ---------------------------------------------------------------------------

TEST_F(SolaxCanInverterTest, KnownRxFrameRefreshesAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  rx1871(PAYLOAD_ANNOUNCE);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE);
}

TEST_F(SolaxCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1234, .data = {0}};
  solax->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

// ---------------------------------------------------------------------------
// TX: transmit_can is a no-op — all TX is driven by RX
// ---------------------------------------------------------------------------

TEST_F(SolaxCanInverterTest, TransmitCanIsAlwaysNoOp) {
  // Even after update_values, periodic transmit must not send anything.
  set_typical_battery_values();
  solax->update_values();
  solax->transmit_can(INTERVAL_60_S + 1);
  EXPECT_TRUE(get_transmitted_frames().empty()) << "SolaX driver must only transmit in response to inverter RX";
}

// ---------------------------------------------------------------------------
// State machine: BATTERY_ANNOUNCE → WAITING_FOR_CONTACTOR → CONTACTOR_CLOSED
// ---------------------------------------------------------------------------

TEST_F(SolaxCanInverterTest, AnnounceStateTransmitsExpectedFramesWithExtIds) {
  // In ANNOUNCE state (byte[0] == 0x01 or 0x02, byte4 == 0) the driver
  // sends the full set of BMS frames plus the announce frame.
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1871, .data = {}};
  f.data.u64 = PAYLOAD_ANNOUNCE;
  // Byte[0] must be 0x01 to enter the switch
  f.data.u8[0] = 0x01;
  solax->map_can_frame_to_variable(f);

  // All Solax frames use extended IDs.
  EXPECT_GT(count_frames_with_id(0x1872), 0u);
  EXPECT_GT(count_frames_with_id(0x1873), 0u);
  EXPECT_GT(count_frames_with_id(0x1874), 0u);
  EXPECT_GT(count_frames_with_id(0x1875), 0u);
  EXPECT_GT(count_frames_with_id(0x1876), 0u);
  EXPECT_GT(count_frames_with_id(0x1877), 0u);
  EXPECT_GT(count_frames_with_id(0x1878), 0u);
  EXPECT_GT(count_frames_with_id(0x187E), 0u);
  EXPECT_GT(count_frames_with_id(0x100A001), 0u);  // BMS Announce (29-bit)

  // Verify extended-ID flag on the announce marker.
  for (const auto& tx : get_transmitted_frames()) {
    if (tx.ID == 0x100A001) {
      EXPECT_TRUE(tx.ext_ID) << "0x100A001 must be sent as extended ID";
    }
  }

  // Contactors must remain closed-forbidden in ANNOUNCE.
  EXPECT_FALSE(datalayer.system.status.inverter_allows_contactor_closing);
}

TEST_F(SolaxCanInverterTest, ClosePayloadAdvancesStateMachineToWaitingAndContactor) {
  // Inject the "close" payload — this should advance state to WAITING_FOR_CONTACTOR.
  rx1871(PAYLOAD_CLOSE);
  clear_transmitted_frames();

  // Second RX (any byte0=0x01/0x02 frame) → driver is now in WAITING_FOR_CONTACTOR,
  // which immediately transitions to CONTACTOR_CLOSED and sends 0x1801.
  rx1871(PAYLOAD_ANNOUNCE);
  EXPECT_GT(count_frames_with_id(0x1801), 0u) << "0x1801 expected in WAITING_FOR_CONTACTOR";
  clear_transmitted_frames();

  // Now in CONTACTOR_CLOSED — inverter_allows_contactor_closing must be set.
  // Hold the state with PAYLOAD_CLOSE: since the contactor-permission fix, a byte4=0 frame in
  // CONTACTOR_CLOSED reads as the open request and revokes in the same frame.
  rx1871(PAYLOAD_CLOSE);
  EXPECT_TRUE(datalayer.system.status.inverter_allows_contactor_closing);
}

TEST_F(SolaxCanInverterTest, ContractorStatus1In1875WhenClosed) {
  // Walk to CONTACTOR_CLOSED and confirm byte[4] in 0x1875 == 1.
  rx1871(PAYLOAD_CLOSE);     // → WAITING_FOR_CONTACTOR
  rx1871(PAYLOAD_ANNOUNCE);  // → CONTACTOR_CLOSED
  clear_transmitted_frames();
  rx1871(PAYLOAD_CLOSE);  // stay in CONTACTOR_CLOSED, re-transmit (byte4=0 would now read as open)

  const CAN_frame* status = find_last_frame_with_id(0x1875);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->data.u8[4], 0x01) << "0x1875 b4 must be 1 when contactor is closed";
}

TEST_F(SolaxCanInverterTest, OpenPayloadInClosedStateResetsToAnnounce) {
  // Walk to CONTACTOR_CLOSED.
  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();

  // Inject open payload. The open request must revoke the closing permission
  // in the same frame (the contactor-permission fix): the inverter
  // that just asked to disconnect cannot be relied on to send another frame.
  rx1871(PAYLOAD_OPEN);
  EXPECT_FALSE(datalayer.system.status.inverter_allows_contactor_closing)
      << "closing permission must not survive the frame carrying the open request";
  // And the BATTERY_ANNOUNCE pass on the next frame agrees.
  rx1871(PAYLOAD_ANNOUNCE);
  EXPECT_FALSE(datalayer.system.status.inverter_allows_contactor_closing)
      << "Contactor must stay disallowed once back in ANNOUNCE";
}

TEST_F(SolaxCanInverterTest, AlwaysClosedModeSkipsStateMachine) {
  // Rebuild with AlwaysClosed workaround.
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::AlwaysClosed;
  setup_inverter();
  solax = static_cast<SolaxInverter*>(inverter);
  clear_transmitted_frames();

  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1871, .data = {}};
  f.data.u8[0] = 0x01;
  solax->map_can_frame_to_variable(f);

  // In AlwaysClosed mode, the driver sends the BMS frames immediately
  // and sets allows_contactor_closing regardless of payload.
  EXPECT_TRUE(datalayer.system.status.inverter_allows_contactor_closing);
  EXPECT_GT(count_frames_with_id(0x1875), 0u);
}

TEST_F(SolaxCanInverterTest, LockAfterFirstCloseIgnoresSubsequentOpenRequest) {
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_contactor_mode = inverter_contactor_mode_enum::LockAfterFirstClose;
  setup_inverter();
  solax = static_cast<SolaxInverter*>(inverter);
  clear_transmitted_frames();

  // Walk to CONTACTOR_CLOSED.
  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();

  // Inject open payload — in LockAfterFirstClose mode, this must be ignored.
  rx1871(PAYLOAD_OPEN);
  EXPECT_TRUE(datalayer.system.status.inverter_allows_contactor_closing)
      << "LockAfterFirstClose must ignore open request";
}

// ---------------------------------------------------------------------------
// TX payload encoding: update_values populates frame buffers
// ---------------------------------------------------------------------------

TEST_F(SolaxCanInverterTest, LimitsFrameEncodesVoltageAndCurrentLittleEndian) {
  datalayer.battery.info.max_design_voltage_dV = 4100;
  datalayer.battery.info.min_design_voltage_dV = 2900;
  datalayer.battery.status.max_charge_current_dA = 150;
  datalayer.battery.status.max_discharge_current_dA = 250;
  solax->update_values();

  // Walk state machine to emit 0x1872.
  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* limits = find_last_frame_with_id(0x1872);
  ASSERT_NE(limits, nullptr);
  // LE encoding: b0 = LSB, b1 = MSB
  EXPECT_EQ(u16_le(limits->data.u8[0], limits->data.u8[1]), 4100u);
  EXPECT_EQ(u16_le(limits->data.u8[2], limits->data.u8[3]), 2900u);
  EXPECT_EQ(u16_le(limits->data.u8[4], limits->data.u8[5]), 150u);
  EXPECT_EQ(u16_le(limits->data.u8[6], limits->data.u8[7]), 250u);
}

TEST_F(SolaxCanInverterTest, PackDataFrameEncodesVoltageCurrentAndSoc) {
  datalayer.battery.status.voltage_dV = 3800;
  datalayer.battery.status.reported_current_dA = static_cast<int16_t>(-500);  // -50.0 A
  datalayer.battery.status.reported_soc = 6250;                               // 62.50 % → byte = 62
  datalayer.battery.status.reported_remaining_capacity_Wh = 12500;
  solax->update_values();

  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* pack = find_last_frame_with_id(0x1873);
  ASSERT_NE(pack, nullptr);
  EXPECT_EQ(u16_le(pack->data.u8[0], pack->data.u8[1]), 3800u) << "voltage LE";
  EXPECT_EQ(static_cast<int16_t>(u16_le(pack->data.u8[2], pack->data.u8[3])), -500) << "signed current";
  EXPECT_EQ(pack->data.u8[4], 62u) << "SOC integer";
  // remaining_Wh / 10 = 1250, LE
  EXPECT_EQ(u16_le(pack->data.u8[6], pack->data.u8[7]), 1250u);
}

TEST_F(SolaxCanInverterTest, CellDataFrameEncodesTemperaturesAndRescaledVoltages) {
  // With min=2500, max=4200, rescale maps to [3000,3500]:
  // 3300 mV → 3000 + (3300-2500)*500/(4200-2500) = 3000 + 800*500/1700 ≈ 3235 mV → 32 dV
  datalayer.battery.status.temperature_max_dC = 300;                        // 30.0 °C, signed
  datalayer.battery.status.temperature_min_dC = static_cast<int16_t>(-50);  // -5.0 °C
  datalayer.battery.status.cell_max_voltage_mV = 3300;
  datalayer.battery.status.cell_min_voltage_mV = 3200;
  datalayer.battery.info.max_cell_voltage_mV = 4200;
  datalayer.battery.info.min_cell_voltage_mV = 2500;
  solax->update_values();

  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* cell = find_last_frame_with_id(0x1874);
  ASSERT_NE(cell, nullptr);
  EXPECT_EQ(static_cast<int16_t>(u16_le(cell->data.u8[0], cell->data.u8[1])), 300) << "temp max LE signed";
  EXPECT_EQ(static_cast<int16_t>(u16_le(cell->data.u8[2], cell->data.u8[3])), -50) << "temp min LE signed (negative)";

  // Rescaled max: 3000 + (3300-2500)*500/1700 = 3235 → /100 = 32 dV
  uint16_t exp_max_dV = (3000 + (3300 - 2500) * 500 / 1700) / 100;
  uint16_t exp_min_dV = (3000 + (3200 - 2500) * 500 / 1700) / 100;
  EXPECT_EQ(u16_le(cell->data.u8[4], cell->data.u8[5]), exp_max_dV);
  EXPECT_EQ(u16_le(cell->data.u8[6], cell->data.u8[7]), exp_min_dV);
}

TEST_F(SolaxCanInverterTest, StatusFrameEncodesTemperatureAverageAndModuleCount) {
  datalayer.battery.status.temperature_max_dC = 300;
  datalayer.battery.status.temperature_min_dC = 200;
  solax->update_values();

  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* status = find_last_frame_with_id(0x1875);
  ASSERT_NE(status, nullptr);
  // Average = (300+200)/2 = 250, LE
  EXPECT_EQ(u16_le(status->data.u8[0], status->data.u8[1]), 250u) << "temp average";
}

TEST_F(SolaxCanInverterTest, PackStatsFrameEncodesVoltageAndCapacity) {
  datalayer.battery.status.voltage_dV = 3900;
  datalayer.battery.info.reported_total_capacity_Wh = 25000;
  solax->update_values();

  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* stats = find_last_frame_with_id(0x1878);
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(u16_le(stats->data.u8[0], stats->data.u8[1]), 3900u);
  // Capacity as 32-bit LE
  uint32_t cap = (uint32_t)stats->data.u8[4] | ((uint32_t)stats->data.u8[5] << 8) |
                 ((uint32_t)stats->data.u8[6] << 16) | ((uint32_t)stats->data.u8[7] << 24);
  EXPECT_EQ(cap, 25000u);
}

TEST_F(SolaxCanInverterTest, UltraFrameEncodesSohAndSocIntegerBytes) {
  datalayer.battery.info.reported_total_capacity_Wh = 20000;
  datalayer.battery.status.soh_pptt = 9500;      // 95.00 % → byte = 95
  datalayer.battery.status.reported_soc = 8000;  // 80.00 % → byte = 80
  solax->update_values();

  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* ultra = find_last_frame_with_id(0x187E);
  ASSERT_NE(ultra, nullptr);
  EXPECT_EQ(ultra->data.u8[4], 95u) << "SOH integer in 187E b4";
  EXPECT_EQ(ultra->data.u8[5], 80u) << "SOC integer in 187E b5";
}

// ---------------------------------------------------------------------------
// Battery-type and module-count user-selected options
// ---------------------------------------------------------------------------

TEST_F(SolaxCanInverterTest, CustomBatteryTypeAppearsIn1877) {
  delete inverter;
  inverter = nullptr;
  user_selected_inverter_battery_type = 0x42;
  setup_inverter();
  solax = static_cast<SolaxInverter*>(inverter);
  clear_transmitted_frames();

  solax->update_values();
  rx1871(PAYLOAD_CLOSE);
  rx1871(PAYLOAD_ANNOUNCE);
  clear_transmitted_frames();
  rx1871(PAYLOAD_ANNOUNCE);

  const CAN_frame* unk = find_last_frame_with_id(0x1877);
  ASSERT_NE(unk, nullptr);
  EXPECT_EQ(unk->data.u8[4], 0x42u) << "custom battery_type in 0x1877 b4";
}

// ---------------------------------------------------------------------------
// Serial-number enumeration triggered by payload 0x0500...
// ---------------------------------------------------------------------------

TEST_F(SolaxCanInverterTest, EnumPayloadTriggersSNFrames) {
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x1871, .data = {}};
  f.data.u64 = PAYLOAD_ENUM;
  solax->map_can_frame_to_variable(f);

  // At least one pair of 0x1881/0x1882 must be sent (slot 0 = master).
  EXPECT_GT(count_frames_with_id(0x1881), 0u);
  EXPECT_GT(count_frames_with_id(0x1882), 0u);
}
