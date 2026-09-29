#include <gtest/gtest.h>

#include <vector>

#include "../../Software/src/battery/TESLA-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"

// The Tesla driver's cell-voltage gate: no DRIVE (contactors closed) until the pack
// has reported its cell voltages.
//
// GH#716 added the gate so a damaged pack reporting partial data could not close its
// contactors on the 3300 mV defaults. Commit e15f28ec4 in GH#1314 removed it from
// TESLA-BATTERY.cpp (TESLA-LEGACY-BATTERY.cpp kept it) while the flag stayed set and
// unread; it is restored in the power-state decision.
//
// Pinned behaviour: no DRIVE command before the cell voltages have been read, and
// DRIVE once they have (so a fix that simply never closes does not pass).

// TX frame capture injected by the emulated CAN layer (see emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

// 0x221 VCFRONT_LVPowerState with upper nibble 0x6 in byte 0 is the DRIVE
// power state, which closes the pack's contactors.
bool drive_frame_sent() {
  for (const CAN_frame& f : get_transmitted_frames()) {
    if (f.ID == 0x221 && (f.data.u8[0] & 0xF0) == 0x60) {
      return true;
    }
  }
  return false;
}

// One update, then two full rotations of the driver's transmit phases (it
// sends one phase per call), so 0x221 goes out whatever phase it starts in.
bool cycle_sends_drive(TeslaBattery& tesla, unsigned long& now) {
  clear_transmitted_frames();
  tesla.update_values();
  for (int call = 0; call < 10; call++) {
    now += 1000;
    tesla.transmit_can(now);
  }
  return drive_frame_sent();
}

}  // namespace

TEST(TeslaCellVoltageGate, CommandsDriveOnlyAfterCellVoltagesAreRead) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.status.system_status = ACTIVE;

  TeslaBattery tesla;
  tesla.setup();
  unsigned long now = 0;

  // No frame from the pack at all: its cell voltages were never read.
  EXPECT_FALSE(cycle_sends_drive(tesla, now))
      << "the Tesla driver commanded DRIVE (contactors closed) before the pack reported any cell voltage";

  // 0x332 multiplexer 1 carries the brick max/min voltages (2 mV per count):
  // 3700 mV = 1850 counts in both 12-bit fields.
  CAN_frame minmax = {};
  minmax.ID = 0x332;
  minmax.DLC = 8;
  const uint16_t counts = 1850;
  minmax.data.u8[0] = static_cast<uint8_t>(((counts & 0x3F) << 2) | 0x01);
  minmax.data.u8[1] = static_cast<uint8_t>(counts >> 6);
  minmax.data.u8[2] = static_cast<uint8_t>(counts & 0xFF);
  minmax.data.u8[3] = static_cast<uint8_t>(counts >> 8);
  tesla.handle_incoming_can_frame(minmax);

  EXPECT_TRUE(cycle_sends_drive(tesla, now))
      << "the Tesla driver did not command DRIVE after the pack reported its cell voltages";
}

// The power state starts as DRIVE and update_values(), which applies the gate, runs once a second; the
// transmit path must not send DRIVE in that first window either.
TEST(TeslaCellVoltageGate, NoDriveBeforeTheFirstUpdateEither) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.status.system_status = ACTIVE;

  TeslaBattery tesla;
  tesla.setup();
  clear_transmitted_frames();
  unsigned long now = 0;
  for (int call = 0; call < 10; call++) {  // transmit before update_values() has ever run
    now += 100;
    tesla.transmit_can(now);
  }
  EXPECT_FALSE(drive_frame_sent()) << "the Tesla driver commanded DRIVE in the boot window, before any cell voltage";
}

// Until the cell voltages arrive the driver is on the shutdown path it takes when the inverter withholds
// permission (0x221 ACCESSORY first), not silent on 0x221 and not in DRIVE.
TEST(TeslaCellVoltageGate, WaitsOnTheShutdownPathUntilTheCellVoltagesArrive) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  datalayer.system.status.inverter_allows_contactor_closing = true;
  datalayer.system.status.system_status = ACTIVE;

  TeslaBattery tesla;
  tesla.setup();
  unsigned long now = 0;
  ASSERT_FALSE(cycle_sends_drive(tesla, now)) << "precondition: no DRIVE before any cell voltage";
  bool accessory = false;
  for (const CAN_frame& f : get_transmitted_frames()) {
    accessory = accessory || (f.ID == 0x221 && (f.data.u8[0] & 0xF0) == 0x40);
  }
  EXPECT_TRUE(accessory) << "no 0x221 ACCESSORY frame before the cell voltages: the power-state decision did not "
                            "take the shutdown path";
}
