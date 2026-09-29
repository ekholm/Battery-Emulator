// eCMP PID pack voltage (0xD815): the reply is twice the voltage in 0.1 V and must be halved before the
// +800 offset (voltage_dV = raw/2 + 800). Commit 5fc94d60a ("Add back reading values from UDS polls", #2939)
// stored the raw value, so a MysteryVan pack reported about twice its voltage (6024 dV). The driver must
// first send its own poll for the PID before it accepts the reply (UdsCanBattery's pending-PID guard).
#include <gtest/gtest.h>

#include "../../Software/src/battery/BATTERIES.h"
#include "../../Software/src/battery/ECMP-BATTERY.h"
#include "../../Software/src/battery/UdsCanBattery.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"
#include "../../Software/src/devboard/safety/safety.h"
#include "../../Software/src/devboard/utils/events.h"

#include "Arduino.h"

// Declarations from test/emul/can.cpp.
const std::vector<CAN_frame>& get_transmitted_frames();
void clear_transmitted_frames();

namespace {

CAN_frame ecmp_frame(uint32_t id, std::initializer_list<uint8_t> bytes) {
  CAN_frame frame = {};
  frame.DLC = 8;
  frame.ID = id;
  uint8_t i = 0;
  for (uint8_t b : bytes) {
    if (i >= 8)
      break;
    frame.data.u8[i++] = b;
  }
  return frame;
}

void reset_ecmp_state() {
  datalayer.battery.status = DATALAYER_BATTERY_STATUS_TYPE{};
  datalayer.battery.info = DATALAYER_BATTERY_INFO_TYPE{};
  datalayer.system.status.system_status = ACTIVE;
  init_events();
  reset_all_events();
}

}  // namespace

// eCMP 0x358 temp sentinel: A 0x358 frame with the temperature byte set to 0xFF must not trip
// EVENT_BATTERY_OVERHEAT. Without a sentinel guard, 0xFF-40 = 215 -> 2150 dC > 500 dC threshold.

// eCMP pack voltage halving: A UDS response for PID_PACK_VOLTAGE (0xD815) with raw value 0x12C0
// (4800) should give voltage_dV = 4800/2 + 800 = 3200.
// On main (UdsCanBattery), the driver must first transmit the poll request for 0xD815 before
// on_uds_receive() processes the reply (pending_pid guard). The pump loop advances emulated time
// until the request frame appears on 0x6B4, then injects the 0x694 response.
// On base (CanBattery), the 0x694 frame is parsed directly; dynamic_cast returns nullptr and the
// pump block is skipped.
// It fails on main before this fix (missing /2: 4800+800=5600).
TEST(EcmpPidDecode, PidPackVoltageMustBeHalvedBeforeStorage) {
  reset_ecmp_state();
  auto bat = new EcmpBattery();
  bat->setup();

  // Set MysteryVan so update_values() reads voltage from pid_pack_voltage (not battery_voltage).
  bat->handle_incoming_can_frame(ecmp_frame(0x2D4, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

  // On main, EcmpBattery extends UdsCanBattery and on_uds_receive() has an early return when
  // pending_pid == 0. Pump transmit_can() with 100ms emulated steps until the driver transmits
  // its own ReadDataByIdentifier request for PID 0xD815 (ID=0x6B4, {0x03,0x22,0xD8,0x15,...}).
  // On base, dynamic_cast returns nullptr and the pump is skipped — the frame is handled directly.
  UdsCanBattery* uds_bat = dynamic_cast<UdsCanBattery*>(bat);
  if (uds_bat != nullptr) {
    clear_transmitted_frames();
    const unsigned long deadline_ms = 100000;  // 100 PIDs × 10 retries × 2 ticks × 100ms
    unsigned long now = 0;
    bool poll_found = false;
    while (now < deadline_ms && !poll_found) {
      now += 100;
      set_millis64(now);
      bat->transmit_can(now);
      for (const auto& f : get_transmitted_frames()) {
        // ECMP UDS poll: SF 3 bytes (0x03), ReadDataByIdentifier (0x22), PID 0xD815.
        if (f.ID == 0x6B4 && f.data.u8[0] == 0x03 && f.data.u8[1] == 0x22 && f.data.u8[2] == 0xD8 &&
            f.data.u8[3] == 0x15) {
          poll_found = true;
          break;
        }
      }
    }
    ASSERT_TRUE(poll_found) << "EcmpBattery must transmit UDS poll for PID 0xD815 within 20 s emulated time";
  }

  // ISO-TP single-frame UDS ReadDataByIdentifier response for DID 0xD815, raw value 0x12C0=4800.
  // byte[0]=0x05 (SF, 5 payload bytes), byte[1]=0x62 (SID 0x22+0x40), byte[2:3]=DID, byte[4:5]=raw.
  bat->handle_incoming_can_frame(ecmp_frame(0x694, {0x05, 0x62, 0xD8, 0x15, 0x12, 0xC0, 0x00, 0x00}));
  bat->update_values();

  const uint16_t voltage_dV = datalayer.battery.status.voltage_dV;
  // Verify the reply was decoded (pid_pack_voltage still NOT_SAMPLED_YET gives voltage_dV == 0).
  ASSERT_GT(voltage_dV, 0u) << "PID 0xD815 reply was not decoded (voltage_dV still 0)";
  // Correct: pid_pack_voltage = 4800/2 = 2400, voltage_dV = 2400+800 = 3200.
  // Defect (main): pid_pack_voltage = 4800, voltage_dV = 4800+800 = 5600.
  EXPECT_EQ(voltage_dV, 3200u) << "PID_PACK_VOLTAGE raw value must be halved; expected 3200 dV, got " << voltage_dV;

  delete bat;
}
