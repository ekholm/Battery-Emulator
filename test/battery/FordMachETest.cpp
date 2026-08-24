#include <gtest/gtest.h>

#include "../../Software/src/battery/FORD-MACH-E-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"

#include "Arduino.h"

// TX capture from the CAN emulation layer (test/emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

// Builds a 0x7EC reply frame from up to 8 raw bytes.
CAN_frame ford_7ec_frame(std::initializer_list<uint8_t> bytes) {
  CAN_frame frame = {};
  frame.DLC = 8;
  frame.ID = 0x7EC;
  uint8_t i = 0;
  for (uint8_t b : bytes) {
    if (i >= 8) {
      break;
    }
    frame.data.u8[i++] = b;
  }
  return frame;
}

class FordMachEDtcTests : public ::testing::Test {
 protected:
  void SetUp() override {
    clear_transmitted_frames();
    datalayer = DataLayer();
    datalayer_extended = DataLayerExtended();
    set_millis64(50000);  // Non-zero, so a completed read is distinguishable from "never read"
    battery = new FordMachEBattery();
    battery->setup();
  }

  void TearDown() override { delete battery; }

  // Advance the emulated clock and run the battery's transmit path (which
  // drives the UDS superclass: scan, sequences, ISO-TP).
  void tick() {
    now_ms += 100;
    set_millis64(now_ms);
    battery->transmit_can(now_ms);
  }

  // The last diagnostic request the battery put on the wire (requests go to 0x7E4).
  const CAN_frame* last_uds_tx() {
    for (auto it = get_transmitted_frames().rbegin(); it != get_transmitted_frames().rend(); ++it) {
      if (it->ID == 0x7E4) {
        return &*it;
      }
    }
    return nullptr;
  }

  // Ticks until the battery emits a fresh 0x7E4 frame; returns it.
  const CAN_frame& next_uds_tx() {
    size_t before = count_uds_tx();
    for (int i = 0; i < 50 && count_uds_tx() == before; i++) {
      tick();
    }
    EXPECT_GT(count_uds_tx(), before) << "battery never transmitted a diagnostic request";
    return *last_uds_tx();
  }

  size_t count_uds_tx() {
    size_t n = 0;
    for (const auto& f : get_transmitted_frames()) {
      if (f.ID == 0x7E4) {
        n++;
      }
    }
    return n;
  }

  FordMachEBattery* battery = nullptr;
  unsigned long now_ms = 50000;
};

}  // namespace

/*Example of DTC read
  //(21.90) RX0 7E4 [8] 03 19 02 8F 00 00 00 00
  //(21.92) RX0 7EC [8] 10 2F 59 02 FF C1 9B 00
  //(21.92) RX0 7E4 [8] 30 00 00 00 00 00 00 00
  //(21.92) RX0 7EC [8] 21 AF C1 00 00 2F C2 93
  //(21.93) RX0 7EC [8] 22 00 AF C2 98 00 AF 1A
  */

// Real BMS capture holding four codes: U019B, U0100, U0293 and U0298. The announced ISO-TP length of
// 0x013 (19 bytes) covers the 59 02 FF header plus four 4-byte records. Wire behavior must match the
// pre-superclass readout: the request is 19 02 with Ford's wide status mask 0x8F, the first frame is
// answered with an ISO-TP flow control on 0x7E4, and the parsed codes land in the datalayer.
TEST_F(FordMachEDtcTests, ShouldParseMultiFrameReply) {
  battery->read_DTC();
  const CAN_frame& req = next_uds_tx();

  // Same bytes the hand-rolled FORD_READ_DTC frame carried: 03 19 02 8F.
  EXPECT_EQ(req.data.u8[0], 0x03);
  EXPECT_EQ(req.data.u8[1], 0x19);
  EXPECT_EQ(req.data.u8[2], 0x02);
  EXPECT_EQ(req.data.u8[3], 0x8F) << "Ford's BECM needs the wide status mask the original code sent";

  size_t tx_before_ff = count_uds_tx();
  battery->handle_incoming_can_frame(ford_7ec_frame({0x10, 0x13, 0x59, 0x02, 0xFF, 0xC1, 0x9B, 0x00}));

  // The superclass must answer the first frame with flow control, like FORD_ACK_FRAME did.
  ASSERT_GT(count_uds_tx(), tx_before_ff) << "no flow control sent for the multi-frame reply";
  EXPECT_EQ(last_uds_tx()->data.u8[0] & 0xF0, 0x30);

  battery->handle_incoming_can_frame(ford_7ec_frame({0x21, 0xAF, 0xC1, 0x00, 0x00, 0xAF, 0xC2, 0x93}));
  battery->handle_incoming_can_frame(ford_7ec_frame({0x22, 0x00, 0xAF, 0xC2, 0x98, 0x00, 0xAF, 0xFF}));

  EXPECT_FALSE(datalayer.battery.dtc.dtc_read_failed);
  ASSERT_EQ(datalayer.battery.dtc.dtc_count, 4);
  EXPECT_EQ(datalayer.battery.dtc.dtc_codes[0], 0xC19B00u);  // U019B
  EXPECT_EQ(datalayer.battery.dtc.dtc_codes[1], 0xC10000u);  // U0100
  EXPECT_EQ(datalayer.battery.dtc.dtc_codes[2], 0xC29300u);  // U0293
  EXPECT_EQ(datalayer.battery.dtc.dtc_codes[3], 0xC29800u);  // U0298
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(datalayer.battery.dtc.dtc_status[i], 0xAF);
  }
  EXPECT_NE(datalayer.battery.dtc.dtc_last_read_millis, 0u);
}

// A confirmed erase (54 ack) puts the page back to "not read yet" - and ONLY a confirmed erase:
// this preserves the pre-superclass semantics, where an unacknowledged 14 FF FF FF left the
// previously read list untouched.
TEST_F(FordMachEDtcTests, ClearAckResetsTheStoredList) {
  // Seed a completed read.
  datalayer.battery.dtc.dtc_count = 2;
  datalayer.battery.dtc.dtc_codes[0] = 0xC19B00u;
  datalayer.battery.dtc.dtc_last_read_millis = 42;

  battery->reset_DTC();
  const CAN_frame& req = next_uds_tx();
  EXPECT_EQ(req.data.u8[0], 0x04);  // Same bytes as the hand-rolled FORD_DTC_RESET
  EXPECT_EQ(req.data.u8[1], 0x14);
  EXPECT_EQ(req.data.u8[2], 0xFF);
  EXPECT_EQ(req.data.u8[3], 0xFF);
  EXPECT_EQ(req.data.u8[4], 0xFF);

  battery->handle_incoming_can_frame(ford_7ec_frame({0x01, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

  EXPECT_EQ(datalayer.battery.dtc.dtc_count, 0);
  EXPECT_FALSE(datalayer.battery.dtc.dtc_read_failed);
  EXPECT_EQ(datalayer.battery.dtc.dtc_last_read_millis, 0u);  // Back to "not read yet"
}

// An erase nobody acknowledges must NOT touch the stored list (the codes are not known gone).
TEST_F(FordMachEDtcTests, UnconfirmedClearLeavesTheListAlone) {
  datalayer.battery.dtc.dtc_count = 2;
  datalayer.battery.dtc.dtc_last_read_millis = 42;

  battery->reset_DTC();
  next_uds_tx();
  for (int i = 0; i < 120; i++) {  // Run the retries and their timeouts out
    tick();
  }

  EXPECT_EQ(datalayer.battery.dtc.dtc_count, 2);
  EXPECT_EQ(datalayer.battery.dtc.dtc_last_read_millis, 42u);
}

// A readout against a silent BMS must resolve as a failed read, so the web page does not show a
// pending read forever (the hand-rolled code had a 2 s deadline for this).
TEST_F(FordMachEDtcTests, SilentBmsResolvesAsFailedRead) {
  battery->read_DTC();
  next_uds_tx();
  for (int i = 0; i < 120; i++) {  // Run the retries and their timeouts out
    tick();
  }

  EXPECT_TRUE(datalayer.battery.dtc.dtc_read_failed);
  EXPECT_NE(datalayer.battery.dtc.dtc_last_read_millis, 0u);
}

// ---------------------------------------------------------------------------
// PID decode parity: golden vectors derived from the pre-superclass parser.
// ---------------------------------------------------------------------------

namespace {

// One scan-list PID with a synthesized reply payload (the bytes after 62 <DID>)
// and the value the ORIGINAL 0x7EC switch stored for it. Original expressions
// quoted verbatim from main @ b53ef90b, where u8[4]/u8[5] are the first two
// payload bytes.
struct PidGolden {
  uint16_t pid;
  std::initializer_list<uint8_t> payload;
  const char* original_expression;
  int32_t expected;
};

}  // namespace

TEST_F(FordMachEDtcTests, PidDecodeMatchesTheOriginalParser) {
  const PidGolden golden[] = {
      {0x4800, {0x50, 0x00}, "u8[4] - 50", 30},                                    // pid_hvb_temp
      {0x4801, {0x12, 0x34}, "((u8[4] << 8) | u8[5]) * 2", 0x1234 * 2},            // pid_hvb_soc
      {0x4802, {0xA0, 0x0A, 0x84, 0x00}, "u8[4..7] big-endian", (int32_t)0xA00A8400},  // contactor status
      {0x4803, {0x01, 0x02}, "(u8[4] << 8) | u8[5]", 0x0102},                      // pos leak voltage
      {0x4804, {0x03, 0x04}, "(u8[4] << 8) | u8[5]", 0x0304},                      // neg leak voltage
      {0x4805, {0x05, 0x06}, "(u8[4] << 8) | u8[5]", 0x0506},                      // pos voltage
      {0x4806, {0x07, 0x08}, "(u8[4] << 8) | u8[5]", 0x0708},                      // neg voltage
      {0x4811, {0x09, 0x0A}, "(u8[4] << 8) | u8[5]", 0x090A},                      // pos bus leak R
      {0x4812, {0x0B, 0x0C}, "(u8[4] << 8) | u8[5]", 0x0B0C},                      // neg bus leak R
      {0x4813, {0x0D, 0x0E}, "(u8[4] << 8) | u8[5]", 0x0D0E},                      // overall leak R
      {0x4814, {0x0F, 0x10}, "(u8[4] << 8) | u8[5]", 0x0F10},                      // open leak R
      {0x4848, {0x11, 0x12}, "(u8[4] << 8) | u8[5]", 0x1112},                      // ETE
      {0x490C, {0xC8, 0x00}, "u8[4] / 2 (guarded > 0)", 100},                      // SoH
      {0x480D, {0x13, 0x14}, "(u8[4] << 8) | u8[5]", 0x1314},                      // voltage
      {0x48BC, {0x15, 0x16}, "(u8[4] << 8) | u8[5]", 0x1516},                      // max charge current
      {0x4810, {0x17, 0x18}, "((u8[4] << 8) | u8[5]) / 2", 0x1718 / 2},            // calendar age
      {0x485C, {0x19, 0x1A}, "(u8[4] << 8) | u8[5]", 0x191A},                      // capacity Ah
      {0x4818, {0x02, 0x00}, "u8[4]", 2},                                          // rebalance status
  };

  // Answer every scan request as it arrives - golden payload where one is
  // defined, a dummy positive reply otherwise so the scan keeps moving. Two
  // full passes over the 36-PID list cover every golden vector.
  for (int step = 0; step < 76; step++) {
    const CAN_frame& req = next_uds_tx();
    ASSERT_EQ(req.data.u8[1], 0x22) << "scan request is ReadDataByIdentifier";
    uint16_t requested = (req.data.u8[2] << 8) | req.data.u8[3];
    CAN_frame reply = {};
    reply.ID = 0x7EC;
    reply.DLC = 8;
    reply.data.u8[1] = 0x62;
    reply.data.u8[2] = requested >> 8;
    reply.data.u8[3] = requested & 0xFF;
    uint8_t len = 3;
    uint8_t idx = 4;
    for (const auto& g : golden) {
      if (g.pid == requested) {
        for (uint8_t b : g.payload) {
          reply.data.u8[idx++] = b;
          len++;
        }
        break;
      }
    }
    if (len == 3) {
      reply.data.u8[idx++] = 0x00;  // Dummy value byte for undecoded PIDs
      len++;
    }
    reply.data.u8[0] = len;
    battery->handle_incoming_can_frame(reply);
  }
  battery->update_values();

  const auto& x = datalayer_extended.fordMachE;
  EXPECT_EQ(x.pid_hvb_temp, 30) << "original: u8[4] - 50";
  EXPECT_EQ(x.pid_hvb_soc, (uint32_t)(0x1234 * 2));
  EXPECT_EQ(x.pid_hvb_contactor_status, 0xA00A8400u);
  EXPECT_EQ(x.pid_hvb_contactor_positive_leak_voltage, 0x0102);
  EXPECT_EQ(x.pid_hvb_contactor_negative_leak_voltage, 0x0304);
  EXPECT_EQ(x.pid_hvb_contactor_positive_voltage, 0x0506);
  EXPECT_EQ(x.pid_hvb_contactor_negative_voltage, 0x0708);
  EXPECT_EQ(x.pid_hvb_contactor_positive_bus_leak_resistance, 0x090A);
  EXPECT_EQ(x.pid_hvb_contactor_negative_bus_leak_resistance, 0x0B0C);
  EXPECT_EQ(x.pid_hvb_contactor_overall_leak_resistance, 0x0D0E);
  EXPECT_EQ(x.pid_hvb_contactor_open_leak_resistance, 0x0F10);
  EXPECT_EQ(x.pid_hvb_soh, 100);
  EXPECT_EQ(x.pid_hvb_voltage, 0x1314);
  EXPECT_EQ(x.pid_hvb_max_charge_current, 0x1516);
  EXPECT_EQ(x.pid_hvb_calendar_age_months, 0x1718 / 2);
  EXPECT_EQ(x.pid_battery_capacity_ah, 0x191A);
  EXPECT_EQ(x.pid_maintenance_rebalance_status, 2);
  (void)golden[0].original_expression;
}

// The wire requests must walk the same 36-PID list in the same order the hand-rolled
// round-robin used (03 22 <DID> to 0x7E4). The pacing moved from one PID per 250 ms to the
// superclass's one per 100 ms tick - deliberate, and stated in the PR body.
TEST_F(FordMachEDtcTests, ScanWalksTheOriginalPollListInOrder) {
  const uint16_t original_order[6] = {0x4800, 0x4801, 0x4802, 0x4803, 0x4804, 0x4805};
  for (uint16_t expected : original_order) {
    const CAN_frame& req = next_uds_tx();
    EXPECT_EQ(req.data.u8[0], 0x03);
    EXPECT_EQ(req.data.u8[1], 0x22);
    EXPECT_EQ((req.data.u8[2] << 8) | req.data.u8[3], expected);
    // Answer so the scan advances immediately instead of timing out.
    battery->handle_incoming_can_frame(
        ford_7ec_frame({0x04, 0x62, req.data.u8[2], req.data.u8[3], 0x00, 0x55, 0x55, 0x55}));
  }
}
