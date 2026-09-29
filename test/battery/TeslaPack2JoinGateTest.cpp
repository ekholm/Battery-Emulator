#include <gtest/gtest.h>

#include <vector>

#include "../../Software/src/battery/TESLA-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"

// TX frame capture injected by the emulated CAN layer (see emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

// ===========================================================================
// Tesla as pack 2 waits for its join gate before CAR_DRIVE
// ===========================================================================

// Tesla as pack 2 used to go to CAR_DRIVE whenever the inverter allowed it, without checking
// battery2_allowed_contactor_closing, so it broadcast 0x221 VCFRONT_LVPowerState DRIVE frames even when
// parallel safety had not permitted pack 2 onto the DC link. The v8.0.0 double-Tesla code had its own gate.
//
// 0x221 DRIVE frames are identified by byte 0: Mux0 starts at 0x60, Mux1 at 0x61. All other power-state
// variants use a different upper nibble (0x00 for OFF/GOING_DOWN, 0x40 for ACCESSORY). The
// generateMuxFrameCounterChecksum call writes only the counter (bits 52-55) and checksum (bits 56-63),
// leaving the upper nibble of byte 0 intact.
TEST(TeslaPack2JoinGate, Pack2SendsNoDriveFrameWhenJoinIsDisallowed) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  clear_transmitted_frames();

  // Inverter is ready, but parallel safety has not permitted pack 2 to join.
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.status.battery2_allowed_contactor_closing = false;
  datalayer.system.status.system_status = ACTIVE;

  TeslaBattery tb2(&datalayer.battery2, &datalayer_extended.tesla_2, CAN_ADDON_MCP2515);

  // update_values() decides vehicleState.
  tb2.update_values();

  // transmit_can() cycles through five phases (0-4). Phase 0 fires on the
  // first call (transmitPhase -1 -> 0), phase 1 on the second (the 50 ms
  // block that sends 0x221). previousMillis50 starts at 0, so any
  // currentMillis >= 50 satisfies the interval condition.
  tb2.transmit_can(1000);  // phase 0
  tb2.transmit_can(2000);  // phase 1 → 0x221 if vehicleState == CAR_DRIVE

  bool found_drive_frame = false;
  for (const CAN_frame& f : get_transmitted_frames()) {
    // byte 0 upper nibble == 0x6 identifies a VCFRONT_LVPowerState DRIVE frame.
    if (f.ID == 0x221 && (f.data.u8[0] & 0xF0) == 0x60) {
      found_drive_frame = true;
      break;
    }
  }

  // No DRIVE frame while the join gate says pack 2 is not on the link.
  EXPECT_FALSE(found_drive_frame) << "Tesla pack 2 must not broadcast a VCFRONT_LVPowerState DRIVE frame "
                                     "when battery2_allowed_contactor_closing is false";

  // The other half: once parallel safety lets pack 2 join, it must drive. Without
  // this, a fix that never lets a second Tesla close passes the case above.
  datalayer.system.status.battery2_allowed_contactor_closing = true;
  tb2.update_values();
  clear_transmitted_frames();
  for (unsigned long now = 3000; now < 13000; now += 1000) {
    tb2.transmit_can(now);  // two full rotations of the five transmit phases
  }
  bool drives_when_allowed = false;
  for (const CAN_frame& f : get_transmitted_frames()) {
    if (f.ID == 0x221 && (f.data.u8[0] & 0xF0) == 0x60) {
      drives_when_allowed = true;
      break;
    }
  }
  EXPECT_TRUE(drives_when_allowed) << "Tesla pack 2 must send DRIVE once battery2_allowed_contactor_closing is true";
}

// ===========================================================================

// Pack 1 has no join gate (parallel safety gates packs 2 and 3 only): the gate must not change it. Without
// this, dereferencing pack 1's absent gate would pass every pack-2 case and crash a single-Tesla install.
TEST(TeslaPack2JoinGate, Pack1HasNoJoinGateAndStillDrives) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  clear_transmitted_frames();
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.status.battery2_allowed_contactor_closing = false;  // pack 2's gate must not reach pack 1
  datalayer.system.status.system_status = ACTIVE;

  TeslaBattery tb1;
  tb1.update_values();
  for (unsigned long now = 1000; now < 11000; now += 1000) {
    tb1.transmit_can(now);  // two full rotations of the five transmit phases
  }
  bool drives = false;
  for (const CAN_frame& f : get_transmitted_frames()) {
    if (f.ID == 0x221 && (f.data.u8[0] & 0xF0) == 0x60) {
      drives = true;
      break;
    }
  }
  EXPECT_TRUE(drives) << "Tesla pack 1 must still go to DRIVE: the join gate is for packs 2 and 3";
}
