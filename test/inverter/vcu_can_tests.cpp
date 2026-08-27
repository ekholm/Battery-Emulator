#include <gtest/gtest.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/devboard/hal/hal.h"
#include "../../Software/src/devboard/utils/events.h"
#include "../../Software/src/inverter/VCU-CAN.h"
#include "../../Software/src/inverter/INVERTERS.h"
#include "../utils/inverter_test_utils.h"

// Protocol tests for the VCU (Nissan LEAF battery emulation) CAN driver.
//
// Three periodic groups: 10 ms (0x1DC, 0x1DB), 100 ms (0x55B, 0x5BC),
// 500 ms (0x59E, 0x5C0). update_values() self-refreshes CAN_inverter_still_alive
// to CAN_STILL_ALIVE - 1 so the VCU never appears as "missing" to the safety code.
// RX: 0x1F2, 0x1D4, 0x50B, 0x50C refresh aliveness.
//
// Bit-packing conventions (Nissan LEAF traffic):
//   0x1DC bytes 0-2: discharge limit (10-bit, 0.25 kW/bit) | charge limit (10-bit)
//   0x1DB bytes 0-1: current signed 11-bit (0.5 A/bit, bias 2047 for negative)
//          bytes 2-3: voltage 10-bit (0.5 V/bit)
//   0x55B bytes 0-1: SOC 10-bit (real_soc/10 in 0.1%-per-bit)
// Byte 7 of 0x1DC, 0x1DB, 0x55B is a Nissan CRC over bytes 0-6.

namespace {

class VcuCanInverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::VCU;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    vcu = static_cast<VCUInverter*>(inverter);
    clear_transmitted_frames();
  }

  VCUInverter* vcu = nullptr;
};

}  // namespace

TEST_F(VcuCanInverterTest, KnownRxFramesRefreshAliveness) {
  for (uint32_t id : {0x1F2u, 0x1D4u, 0x50Bu, 0x50Cu}) {
    datalayer.system.status.CAN_inverter_still_alive = 0;
    CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {0}};
    vcu->map_can_frame_to_variable(f);
    EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE)
        << "ID 0x" << std::hex << id;
  }
}

TEST_F(VcuCanInverterTest, UnknownRxFrameDoesNotRefreshAliveness) {
  datalayer.system.status.CAN_inverter_still_alive = 0;
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = 0x7FF, .data = {0}};
  vcu->map_can_frame_to_variable(f);
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, 0);
}

TEST_F(VcuCanInverterTest, UpdateValuesSelfRefreshesAlivenessCounter) {
  // VCU self-reports CAN_STILL_ALIVE - 1 each update so the missing-inverter
  // event never fires (no physical inverter on the CAN bus for VCU mode).
  datalayer.system.status.CAN_inverter_still_alive = 0;
  vcu->update_values();
  EXPECT_EQ(datalayer.system.status.CAN_inverter_still_alive, CAN_STILL_ALIVE - 1);
}

TEST_F(VcuCanInverterTest, NoTransmitBefore10msInterval) {
  vcu->update_values();
  vcu->transmit_can(5);  // less than 10 ms
  EXPECT_TRUE(get_transmitted_frames().empty());
}

TEST_F(VcuCanInverterTest, TenMsGroupSends1DCAnd1DB) {
  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x1DC), 1u);
  EXPECT_EQ(count_frames_with_id(0x1DB), 1u);
  // 100 ms / 500 ms groups must not have fired yet.
  EXPECT_EQ(count_frames_with_id(0x55B), 0u);
  EXPECT_EQ(count_frames_with_id(0x5BC), 0u);
}

TEST_F(VcuCanInverterTest, HundredMsGroupSends55BAnd5BC) {
  vcu->update_values();
  vcu->transmit_can(INTERVAL_100_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x55B), 1u);
  EXPECT_EQ(count_frames_with_id(0x5BC), 1u);
  // 500 ms group must not have fired.
  EXPECT_EQ(count_frames_with_id(0x59E), 0u);
  EXPECT_EQ(count_frames_with_id(0x5C0), 0u);
}

TEST_F(VcuCanInverterTest, FiveHundredMsGroupSends59EAnd5C0) {
  vcu->update_values();
  vcu->transmit_can(INTERVAL_500_MS + 1);
  EXPECT_EQ(count_frames_with_id(0x59E), 1u);
  EXPECT_EQ(count_frames_with_id(0x5C0), 1u);
}

TEST_F(VcuCanInverterTest, DischargePowerEncodedIn1DCBytes0To1) {
  // dislimit_raw = max_discharge_power_W / 250 (10-bit)
  // byte 0 = raw >> 2; byte 1 bits 7:6 = raw & 0x03
  datalayer.battery.status.max_discharge_power_W = 50000;  // raw = 200 = 0xC8
  datalayer.battery.status.max_charge_power_W = 0;

  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1DC);
  ASSERT_NE(f, nullptr);
  uint16_t dislimit_raw = 50000u / 250u;  // 200 = 0xC8
  EXPECT_EQ(f->data.u8[0], static_cast<uint8_t>(dislimit_raw >> 2));
  EXPECT_EQ(f->data.u8[1] >> 6, static_cast<uint8_t>(dislimit_raw & 0x03));
}

TEST_F(VcuCanInverterTest, ChargePowerEncodedIn1DCBytes1To2) {
  // chglimit_raw = max_charge_power_W / 250 (10-bit)
  // byte 1 bits 5:0 = chglimit >> 4; byte 2 bits 7:4 = chglimit & 0x0F
  datalayer.battery.status.max_discharge_power_W = 0;
  datalayer.battery.status.max_charge_power_W = 25000;  // raw = 100 = 0x64

  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1DC);
  ASSERT_NE(f, nullptr);
  uint16_t chglimit_raw = 25000u / 250u;  // 100 = 0x64
  EXPECT_EQ(f->data.u8[1] & 0x3F, static_cast<uint8_t>(chglimit_raw >> 4));
  EXPECT_EQ(f->data.u8[2] >> 4, static_cast<uint8_t>(chglimit_raw & 0x0F));
}

TEST_F(VcuCanInverterTest, SocEncodedIn55BBytes0To1) {
  // soc_raw = real_soc / 10 (10-bit, 0.1%-per-bit)
  // byte 0 = raw >> 2; byte 1 bits 7:6 = raw & 0x03
  datalayer.battery.status.real_soc = 7550;  // 75.50% -> soc_raw = 755 = 0x2F3

  vcu->update_values();
  vcu->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x55B);
  ASSERT_NE(f, nullptr);
  uint16_t soc_raw = 7550u / 10u;  // 755
  EXPECT_EQ(f->data.u8[0], static_cast<uint8_t>(soc_raw >> 2));
  EXPECT_EQ(f->data.u8[1] >> 6, static_cast<uint8_t>(soc_raw & 0x03));
}

TEST_F(VcuCanInverterTest, PositiveCurrentEncodedIn1DBBytes0To1) {
  // Positive current (discharge): raw = (current_dA * 2) / 10; no bias
  // field is 11-bit: byte 0 = raw >> 3; byte 1 bits 7:5 = raw & 0x07
  datalayer.battery.status.current_dA = 100;  // 10 A -> raw = (100*2)/10 = 20

  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1DB);
  ASSERT_NE(f, nullptr);
  int32_t raw = 20;  // positive, no bias
  EXPECT_EQ(f->data.u8[0], static_cast<uint8_t>((raw >> 3) & 0xFF));
  EXPECT_EQ((f->data.u8[1] >> 5) & 0x07, static_cast<uint8_t>(raw & 0x07));
}

TEST_F(VcuCanInverterTest, NegativeCurrentBiasedIn1DBBytes0To1) {
  // Negative current (charge): raw = (current_dA*2)/10 + 2047 & 0x7FF
  datalayer.battery.status.current_dA = static_cast<int16_t>(-60);  // -6 A -> raw = (-60*2)/10 = -12 + 2047 = 2035
  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1DB);
  ASSERT_NE(f, nullptr);
  int32_t raw = 2035;
  EXPECT_EQ(f->data.u8[0], static_cast<uint8_t>((raw >> 3) & 0xFF));
  EXPECT_EQ((f->data.u8[1] >> 5) & 0x07, static_cast<uint8_t>(raw & 0x07));
}

TEST_F(VcuCanInverterTest, VoltageEncodedIn1DBBytes2To3) {
  // voltage_raw = (voltage_dV * 2) / 10 (10-bit, 0.5 V/bit)
  // byte 2 = raw >> 2; byte 3 bits 7:6 = raw & 0x03
  datalayer.battery.status.voltage_dV = 3700;  // raw = (3700*2)/10 = 740 = 0x2E4

  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x1DB);
  ASSERT_NE(f, nullptr);
  uint16_t vraw = static_cast<uint16_t>((static_cast<uint32_t>(3700) * 2) / 10) & 0x3FF;
  EXPECT_EQ(f->data.u8[2], static_cast<uint8_t>(vraw >> 2));
  EXPECT_EQ((f->data.u8[3] >> 6) & 0x03, static_cast<uint8_t>(vraw & 0x03));
}

TEST_F(VcuCanInverterTest, RemainingGidsEncodedIn5BC) {
  // remaining_gids = (real_soc / 10000.0) * 281; 10-bit, packed in bytes 0-1
  datalayer.battery.status.real_soc = 5000;  // 50.00% -> gids = 0.5 * 281 = 140

  vcu->update_values();
  vcu->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x5BC);
  ASSERT_NE(f, nullptr);
  uint16_t gids = static_cast<uint16_t>((5000 / 10000.0) * 281);  // 140
  EXPECT_EQ(f->data.u8[0], static_cast<uint8_t>(gids >> 2));
  EXPECT_EQ((f->data.u8[1] >> 6) & 0x03, static_cast<uint8_t>(gids & 0x03));
}

TEST_F(VcuCanInverterTest, SohEncodedIn5BCByte4) {
  // byte 4 = (soh_pptt / 100) << 1
  datalayer.battery.status.soh_pptt = 9500;  // 95% -> 95 << 1 = 190

  vcu->update_values();
  vcu->transmit_can(INTERVAL_100_MS + 1);

  const CAN_frame* f = find_frame_with_id(0x5BC);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->data.u8[4], static_cast<uint8_t>((9500u / 100u) << 1));
}

TEST_F(VcuCanInverterTest, Mprun10CounterIncrementsIn1DCByte6) {
  // mprun10 cycles 0-1-2-3 on every 10ms group; byte 6 carries the counter.
  vcu->update_values();
  vcu->transmit_can(INTERVAL_10_MS + 1);
  const CAN_frame* f0 = find_last_frame_with_id(0x1DC);
  ASSERT_NE(f0, nullptr);
  uint8_t first_mprun = f0->data.u8[6];

  clear_transmitted_frames();
  vcu->transmit_can(2 * (INTERVAL_10_MS + 1));
  const CAN_frame* f1 = find_last_frame_with_id(0x1DC);
  ASSERT_NE(f1, nullptr);
  EXPECT_EQ(f1->data.u8[6], (first_mprun + 1) % 4);
}

TEST_F(VcuCanInverterTest, FiveC0ByteCyclesOnEvery500msCall) {
  // 0x5C0 byte 0 walks through 0x40->0x80->0xC0->0x40 on each 500ms edge.
  // The initial value in the frame template is 0x80.
  vcu->update_values();
  vcu->transmit_can(INTERVAL_500_MS + 1);
  const CAN_frame* f0 = find_last_frame_with_id(0x5C0);
  ASSERT_NE(f0, nullptr);
  uint8_t b0 = f0->data.u8[0];

  clear_transmitted_frames();
  vcu->transmit_can(2 * (INTERVAL_500_MS + 1));
  const CAN_frame* f1 = find_last_frame_with_id(0x5C0);
  ASSERT_NE(f1, nullptr);

  // Each call advances through 0x40->0x80->0xC0->0x40 (the cycle map in the driver)
  uint8_t expected_next;
  if (b0 == 0x40) {
    expected_next = 0x80;
  } else if (b0 == 0x80) {
    expected_next = 0xC0;
  } else {
    expected_next = 0x40;
  }
  EXPECT_EQ(f1->data.u8[0], expected_next);
}
