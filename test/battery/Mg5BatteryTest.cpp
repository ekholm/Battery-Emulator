#include <gtest/gtest.h>

#include "../../Software/src/battery/MG-5-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"

#include "Arduino.h"

// TX capture from the CAN emulation layer (test/emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

CAN_frame mg5_frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
  CAN_frame frame = {};
  frame.DLC = 8;
  frame.ID = id;
  uint8_t i = 0;
  for (uint8_t b : bytes) {
    if (i >= 8) {
      break;
    }
    frame.data.u8[i++] = b;
  }
  return frame;
}

class Mg5BatteryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clear_transmitted_frames();
    datalayer = DataLayer();
    set_millis64(50000);
    battery = new Mg5Battery();
    battery->setup();
    // Run the 2 s boot grace out so diagnostics are live.
    for (int i = 0; i < 25; i++) {
      tick();
    }
  }

  void TearDown() override { delete battery; }

  void tick() {
    now_ms += 100;
    set_millis64(now_ms);
    battery->transmit_can(now_ms);
  }

  // Diagnostic requests can go to the broadcast or the detected address.
  const CAN_frame* last_diag_tx() {
    for (auto it = get_transmitted_frames().rbegin(); it != get_transmitted_frames().rend(); ++it) {
      if (it->ID == 0x7DF || it->ID == 0x781 || it->ID == 0x7E5) {
        return &*it;
      }
    }
    return nullptr;
  }

  size_t count_diag_tx(uint32_t id) {
    size_t n = 0;
    for (const auto& f : get_transmitted_frames()) {
      if (f.ID == id) {
        n++;
      }
    }
    return n;
  }

  const CAN_frame& next_diag_tx() {
    size_t before = total_diag_tx();
    for (int i = 0; i < 50 && total_diag_tx() == before; i++) {
      tick();
    }
    EXPECT_GT(total_diag_tx(), before) << "battery never transmitted a diagnostic request";
    return *last_diag_tx();
  }

  size_t total_diag_tx() { return count_diag_tx(0x7DF) + count_diag_tx(0x781) + count_diag_tx(0x7E5); }

  Mg5Battery* battery = nullptr;
  unsigned long now_ms = 50000;
};

}  // namespace

// The scan starts on the generic broadcast address with the first DID of the original list; the
// first answer (necessarily from a BMS-specific id) pins the address pair and restarts the scan,
// which then walks the same 15 DIDs the hand-rolled round-robin used, in the same order.
// (Detection costs one repeated B041 poll - the old code kept its position instead; harmless.)
TEST_F(Mg5BatteryTest, ScanStartsOnBroadcastAndWalksTheOriginalList) {
  const CAN_frame& first = next_diag_tx();
  EXPECT_EQ(first.ID, 0x7DFu) << "undetected BMS is addressed via the broadcast id";
  EXPECT_EQ(first.data.u8[1], 0x22);
  EXPECT_EQ((first.data.u8[2] << 8) | first.data.u8[3], 0xB041);
  battery->handle_incoming_can_frame(mg5_frame(0x789, {0x04, 0x62, 0xB0, 0x41, 0x00, 0x00, 0x00, 0x00}));

  const uint16_t original_order[5] = {0xB041, 0xB042, 0xB043, 0xB045, 0xB046};
  for (uint16_t expected : original_order) {
    const CAN_frame& req = next_diag_tx();
    EXPECT_EQ(req.ID, 0x781u) << "detected BMS is addressed directly";
    EXPECT_EQ(req.data.u8[0], 0x03);
    EXPECT_EQ(req.data.u8[1], 0x22);
    EXPECT_EQ((req.data.u8[2] << 8) | req.data.u8[3], expected);
    battery->handle_incoming_can_frame(
        mg5_frame(0x789, {0x04, 0x62, req.data.u8[2], req.data.u8[3], 0x00, 0x00, 0x00, 0x00}));
  }
}

// The first reply from 0x789 pins the BMS-specific request address 0x781 (and the response
// address), exactly like the pre-superclass detection did.
TEST_F(Mg5BatteryTest, FirstReplyFrom789PinsAddress781) {
  battery->handle_incoming_can_frame(mg5_frame(0x789, {0x04, 0x62, 0xB0, 0x41, 0x00, 0x00, 0x00, 0x00}));
  const CAN_frame& req = next_diag_tx();
  EXPECT_EQ(req.ID, 0x781u);
}

// ... and a reply from 0x7ED pins 0x7E5 (the second known MG5 wiring).
TEST_F(Mg5BatteryTest, FirstReplyFrom7EDPinsAddress7E5) {
  battery->handle_incoming_can_frame(mg5_frame(0x7ED, {0x04, 0x62, 0xB0, 0x41, 0x00, 0x00, 0x00, 0x00}));
  const CAN_frame& req = next_diag_tx();
  EXPECT_EQ(req.ID, 0x7E5u);
}

// SoH decode parity: DID 0xB061, original expression (u8[4] << 8) | u8[5] into soh_pptt.
TEST_F(Mg5BatteryTest, SohPidDecodeMatchesTheOriginalParser) {
  battery->handle_incoming_can_frame(mg5_frame(0x789, {0x04, 0x62, 0xB0, 0x41, 0x00, 0x00, 0x00, 0x00}));
  for (int guard = 0; guard < 60; guard++) {
    const CAN_frame& req = next_diag_tx();
    uint16_t requested = (req.data.u8[2] << 8) | req.data.u8[3];
    if (requested == 0xB061) {
      battery->handle_incoming_can_frame(mg5_frame(0x789, {0x05, 0x62, 0xB0, 0x61, 0x26, 0x48, 0x00, 0x00}));
      break;
    }
    battery->handle_incoming_can_frame(
        mg5_frame(0x789, {0x04, 0x62, req.data.u8[2], req.data.u8[3], 0x00, 0x00, 0x00, 0x00}));
  }
  EXPECT_EQ(datalayer.battery.status.soh_pptt, 0x2648);  // 98.00 %
}

// The DTC readout goes out as 19 02 FF (the hand-rolled MG5_781_RQ_DTCs bytes) and a multi-frame
// reply now lands in the datalayer - the pre-superclass code only printed it to the log, so the
// web page gains a real DTC list with this conversion.
TEST_F(Mg5BatteryTest, DtcReadUsesWideMaskAndFillsTheDatalayer) {
  battery->handle_incoming_can_frame(mg5_frame(0x789, {0x04, 0x62, 0xB0, 0x41, 0x00, 0x00, 0x00, 0x00}));
  battery->read_DTC();
  const CAN_frame* req = nullptr;
  for (int guard = 0; guard < 60; guard++) {
    const CAN_frame& r = next_diag_tx();
    if (r.data.u8[1] == 0x19) {
      req = &r;
      break;
    }
    battery->handle_incoming_can_frame(
        mg5_frame(0x789, {0x04, 0x62, r.data.u8[2], r.data.u8[3], 0x00, 0x00, 0x00, 0x00}));
  }
  ASSERT_NE(req, nullptr) << "no DTC readout request seen";
  EXPECT_EQ(req->data.u8[0], 0x03);
  EXPECT_EQ(req->data.u8[2], 0x02);
  EXPECT_EQ(req->data.u8[3], 0xFF) << "MG5 requested every status (19 02 FF) before the conversion";

  size_t before_fc = count_diag_tx(0x781);
  battery->handle_incoming_can_frame(mg5_frame(0x789, {0x10, 0x0B, 0x59, 0x02, 0xFF, 0x02, 0x93, 0x00}));
  EXPECT_GT(count_diag_tx(0x781), before_fc) << "first frame not answered with flow control";
  battery->handle_incoming_can_frame(mg5_frame(0x789, {0x21, 0xAF, 0xC1, 0x9B, 0x00, 0x08, 0x55, 0x55}));

  EXPECT_FALSE(datalayer.battery.dtc.dtc_read_failed);
  ASSERT_EQ(datalayer.battery.dtc.dtc_count, 2);
  EXPECT_EQ(datalayer.battery.dtc.dtc_codes[0], 0x029300u);  // The DTC that blocks contactor closing
  EXPECT_EQ(datalayer.battery.dtc.dtc_status[0], 0xAF);
  EXPECT_EQ(datalayer.battery.dtc.dtc_codes[1], 0xC19B00u);
  EXPECT_EQ(datalayer.battery.dtc.dtc_status[1], 0x08);
}

// When the diagnostic side stays quiet, the driver (re-)enters the extended session - the
// hand-rolled code sent 10 03 on every transaction timeout, the conversion does it on a quiet
// window instead.
TEST_F(Mg5BatteryTest, QuietDiagSideReentersExtendedSession) {
  bool session_seen = false;
  for (int i = 0; i < 80 && !session_seen; i++) {
    tick();
    battery->update_values();
    for (const auto& f : get_transmitted_frames()) {
      if ((f.ID == 0x7DF || f.ID == 0x781) && f.data.u8[1] == 0x10 && f.data.u8[2] == 0x03) {
        session_seen = true;
        break;
      }
    }
  }
  EXPECT_TRUE(session_seen) << "no 10 03 session control while the diag side was quiet";
}

// ---------------------------------------------------------------------------
// Broadcast decode parity: golden vectors from the pre-superclass switch
// (untouched by the conversion, pinned here for the first time).
// ---------------------------------------------------------------------------

// 0x3AC: voltage (((u8[4] & 0x0F) << 8 | u8[5]) * 5) / 2 dV, current -(raw - 20000) / 2 dA,
// SOC (u8[2] << 8 | u8[3]) * 10 pptt (via update_soc's 92 % rescale when full-capacity mode is on).
TEST_F(Mg5BatteryTest, BroadcastSummaryDecodeMatchesTheOriginal) {
  // voltage raw 0x320 = 800 -> 400.0 V -> 2000 dV; current raw 0x4E84 = 20100 -> -50 dA (-5 A)
  battery->handle_incoming_can_frame(mg5_frame(0x3AC, {0x00, 0x00, 0x01, 0xF4, 0x03, 0x20, 0x4E, 0x84}));

  EXPECT_EQ(datalayer.battery.status.voltage_dV, 2000);
  EXPECT_EQ(datalayer.battery.status.current_dA, -50);
  // SOC raw 0x1F4 = 500 (50.0 %) -> 5000 pptt (MG5_USE_FULL_CAPACITY is off
  // by default, so no 92 % rescale).
  EXPECT_EQ(datalayer.battery.status.real_soc, 5000);
}

// 0x173: cell max at (u8[4] << 8 | u8[5]) mV, cell min at (u8[6] << 8 | u8[7]) mV, both gated to
// (0, 0x2000).
TEST_F(Mg5BatteryTest, BroadcastCellExtremesDecodeMatchesTheOriginal) {
  battery->handle_incoming_can_frame(mg5_frame(0x173, {0x00, 0x00, 0x00, 0x00, 0x0F, 0xF6, 0x0F, 0x3C}));

  EXPECT_EQ(datalayer.battery.status.cell_max_voltage_mV, 4086u);
  EXPECT_EQ(datalayer.battery.status.cell_min_voltage_mV, 3900u);
}
