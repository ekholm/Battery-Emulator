#include <gtest/gtest.h>

#include "../../Software/src/battery/TESLA-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/datalayer/datalayer_extended.h"

// 0x352 BMS_energyStatus comes in two layouts: packs from about 2021 multiplex it on byte 0's low two bits;
// older packs send one frame whose byte 0 is the low byte of BMS_nominalFullPackEnergy. The driver may only
// take a pack for muxed once it has seen index 0 and index 1 AND an index changing on three consecutive
// frames - an older pack's byte 0 reads as index 0 or 1 by its value alone, and taking such frames for
// "muxed" loses the old layout for good.

namespace {

CAN_frame f352(uint8_t b0, uint8_t b1, uint8_t b2 = 0, uint8_t b3 = 0) {
  CAN_frame f = {};
  f.ID = 0x352;
  f.DLC = 8;
  f.data.u8[0] = b0;
  f.data.u8[1] = b1;
  f.data.u8[2] = b2;
  f.data.u8[3] = b3;
  return f;
}

}  // namespace

TEST(TeslaEnergyStatusMux, AnOlderPackWhoseByte0LooksLikeIndex0KeepsItsLayout) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  TeslaBattery tesla;
  tesla.setup();
  // BMS_nominalFullPackEnergy 752 (75.2 kWh) = 0x2F0: byte 0 = 0xF0, whose low two bits read as index 0.
  for (int i = 0; i < 5; i++) {
    tesla.handle_incoming_can_frame(f352(0xF0, 0x02));
  }
  tesla.update_values();
  EXPECT_EQ(datalayer_extended.tesla.battery_nominal_full_pack_energy, 752)
      << "an older pack's energy frame was taken for the muxed layout and its own layout never decoded";
  EXPECT_FALSE(datalayer_extended.tesla.BMS352_mux);

  // Its value moving must keep reaching the page too - and crossing from "index 0" to "index 1" by value
  // (72.9 kWh = 0x2D9) must not make it a muxed pack.
  tesla.handle_incoming_can_frame(f352(0xD9, 0x02));
  tesla.update_values();
  EXPECT_EQ(datalayer_extended.tesla.battery_nominal_full_pack_energy, 729);
  EXPECT_FALSE(datalayer_extended.tesla.BMS352_mux) << "an energy drifting across an index boundary confirmed muxed";
}

TEST(TeslaEnergyStatusMux, AMuxedPackIsConfirmedByBothIndicesAndThenSkipsTheOldLayout) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  TeslaBattery tesla;
  tesla.setup();
  // A muxed pack cycles its index: 0, 1, 2, 0, 1. Index 0 carries nominal full pack energy in bytes 2-3
  // (0.02 kWh), here 3760 = 75.2 kWh.
  tesla.handle_incoming_can_frame(f352(0x00, 0x00, 0xB0, 0x0E));
  tesla.handle_incoming_can_frame(f352(0x01, 0x00, 0x10, 0x00));
  tesla.handle_incoming_can_frame(f352(0x02, 0x26, 0x02, 0x20));
  tesla.handle_incoming_can_frame(f352(0x00, 0x00, 0xB0, 0x0E));
  tesla.handle_incoming_can_frame(f352(0x01, 0x00, 0x10, 0x00));
  tesla.update_values();
  EXPECT_TRUE(datalayer_extended.tesla.BMS352_mux);
  EXPECT_EQ(datalayer_extended.tesla.battery_nominal_full_pack_energy_m0, 3760);

  // Once confirmed, a later frame must not also be decoded as the old layout. An index-2 frame, because it
  // does not re-trigger the confirmation: only the post-confirmation skip keeps it out (read as the old
  // layout it would give 0x502 = 1282).
  const auto before = datalayer_extended.tesla.battery_nominal_full_pack_energy;
  tesla.handle_incoming_can_frame(f352(0x02, 0x05, 0x02, 0x20));
  tesla.update_values();
  EXPECT_EQ(datalayer_extended.tesla.battery_nominal_full_pack_energy, before)
      << "a confirmed muxed pack's frame was decoded as the old layout";
}

TEST(TeslaEnergyStatusMux, AnOlderPackDriftingSlowlyAcrossSeveralIndicesIsNeverTakenForMuxed) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  TeslaBattery tesla;
  tesla.setup();
  // Over a long uptime the BMS re-estimates its capacity: 75.3, 75.2, 75.1, 75.0 kWh, each held for many
  // frames. Byte 0 reads as index 1, 0, 3, 2 - three changes of "index", both 0 and 1 seen, yet never two
  // in consecutive frames. A muxed pack changes its index on every frame.
  const uint8_t lows[] = {0xF1, 0xF0, 0xEF, 0xEE};
  for (uint8_t low : lows) {
    for (int i = 0; i < 50; i++) {
      tesla.handle_incoming_can_frame(f352(low, 0x02));
    }
  }
  tesla.update_values();
  EXPECT_FALSE(datalayer_extended.tesla.BMS352_mux) << "a slowly drifting older pack was taken for muxed";
  EXPECT_EQ(datalayer_extended.tesla.battery_nominal_full_pack_energy, 750);
}

TEST(TeslaEnergyStatusMux, TwoConsecutiveChangesDoNotConfirmMuxed) {
  datalayer = DataLayer();
  datalayer_extended = DataLayerExtended();
  TeslaBattery tesla;
  tesla.setup();
  // An older pack's value flickering 75.2, 75.3, 75.2 kWh on consecutive frames: index 0, 1, 0 - two
  // consecutive changes, both indices seen. A muxed pack is only confirmed on the third.
  for (int i = 0; i < 5; i++) {
    tesla.handle_incoming_can_frame(f352(0xF0, 0x02));
  }
  tesla.handle_incoming_can_frame(f352(0xF1, 0x02));
  for (int i = 0; i < 5; i++) {
    tesla.handle_incoming_can_frame(f352(0xF0, 0x02));
  }
  tesla.update_values();
  EXPECT_FALSE(datalayer_extended.tesla.BMS352_mux) << "two consecutive index changes confirmed muxed";
  EXPECT_EQ(datalayer_extended.tesla.battery_nominal_full_pack_energy, 752);
}
