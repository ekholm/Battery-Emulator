// Regression tests for three defects that CORRUPTED a value rather than losing
// it: two instances of a signed quantity carried unsigned, and one clamp that
// assigned its limit to the wrong variable.  All three were live and silent —
// nothing logged, nothing refused, the wrong number simply went out.
//
//   1. datalayer.shunt.measured_amperage_dA was uint16_t while the mA field it
//      is derived from is int32_t, so every discharge current wrapped to ~65k.
//      Both writers are covered here: BMW-SBOX (shunt) and BYD-CAN (shunt mode).
//   2. BMW-SBOX's 1-second rolling average summed and divided uint32_t, so one
//      discharge sample became ~+429 kA in the int32_t datalayer field.
//   3. CHEVY-VOLT-CHARGER's over-current branch assigned the max-amp constant
//      to the VOLTAGE setpoint, which both transmitted a nonsense voltage and
//      left the current it meant to clamp unclamped.
//
// None of the three is reproducible here on hardware: the bench has no SBOX,
// no BYD shunt and no Volt charger.  These are host tests over the decode and
// clamp arithmetic, which is where the defects live.

#include <gtest/gtest.h>

#include <cstring>
#include <new>
#include <vector>

#include <Arduino.h>

#include "../Software/src/charger/CHARGERS.h"
#include "../Software/src/charger/CHEVY-VOLT-CHARGER.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/inverter/BYD-CAN.h"
#include "../Software/src/inverter/INVERTERS.h"
#include "../Software/src/shunt/BMW-SBOX.h"
#include "../Software/src/shunt/Shunt.h"

// TX frame capture injected by the emulated CAN layer (see emul/can.cpp).
void clear_transmitted_frames();
const std::vector<CAN_frame>& get_transmitted_frames();

namespace {

const CAN_frame* last_frame_with_id(uint32_t id) {
  const CAN_frame* found = nullptr;
  for (const auto& f : get_transmitted_frames()) {
    if (f.ID == id) {
      found = &f;
    }
  }
  return found;
}

// ── BMW-SBOX: the shunt that derives the deci-amp field and averages it ──

// 0x200 carries the current as three little-endian bytes.  The driver reads
// them shifted one byte HIGH and divides by 256, which is how a 24-bit value
// sign-extends into int32_t — the reason a discharge frame decodes negative at
// all, and so the premise of every test below.
CAN_frame sbox_current_frame(int32_t mA) {
  uint32_t raw = static_cast<uint32_t>(mA);
  return CAN_frame{.FD = false,
                   .ext_ID = false,
                   .DLC = 3,
                   .ID = 0x200,
                   .data = {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
                            static_cast<uint8_t>((raw >> 16) & 0xFF)}};
}

class SboxSignednessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    delete shunt;
    shunt = nullptr;
    sbox = new BmwSbox();
    shunt = sbox;
    sbox->setup();
    clear_transmitted_frames();
    set_millis64(0);
  }

  void TearDown() override {
    delete shunt;
    shunt = nullptr;
    sbox = nullptr;
  }

  void inject_current_frame(int32_t mA) {
    CAN_frame f = sbox_current_frame(mA);
    sbox->handle_incoming_can_frame(f);
  }

  BmwSbox* sbox = nullptr;
};

// -500 mA discharge must stay negative in BOTH fields.  With the deci-amp
// field unsigned this stored 65531 instead of -5.
TEST_F(SboxSignednessTest, DischargeCurrentKeepsItsSignInTheDeciampField) {
  inject_current_frame(-500);
  EXPECT_EQ(datalayer.shunt.measured_amperage_mA, -500);
  EXPECT_EQ(datalayer.shunt.measured_amperage_dA, -5) << "a discharge current must not wrap in the deci-amp field";
}

// Charge direction still works — the fix must not cost the positive range.
TEST_F(SboxSignednessTest, ChargeCurrentIsUnaffectedByTheSignedField) {
  inject_current_frame(1500);
  EXPECT_EQ(datalayer.shunt.measured_amperage_dA, 15);
}

// The averaging path is a second, independent instance of the same class:
// the sample array and its sum were uint32_t, so one -500 mA sample averaged
// to (2^32-500)/10 = 429,496,679 mA and landed in the int32_t datalayer field
// as roughly +429 kA — a discharge reported as a colossal charge.
// millis must pass 100 for the averaging branch to run at all.
TEST_F(SboxSignednessTest, OneDischargeSampleAveragesToItsOwnTenth) {
  set_millis64(200);
  inject_current_frame(-500);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -50) << "the 1 s average must not wrap on a discharge sample";
}

// The same averaging pass reads all ten slots from the first sample onward, so
// the nine not yet written are live inputs, and `k` selects a slot before
// anything has written it.  Neither had an initialiser.  It is not live garbage
// today: `new BmwSbox()` in Shunts.cpp value-initializes, because BmwSbox
// declares no constructor of its own — only its CanShunt base does.  That is an
// accident of the allocation expression, and adding one constructor to this
// class ends it — which is exactly how ECMP's identical cell array became live
// garbage.  The sweep that zeroed this shape across the battery drivers never
// reached the shunts, so this one is fixed here, on the same two declarations
// the signedness fix was already rewriting.
//
// Default-init placement-new on a poisoned buffer is the house pattern
// (test/battery/memory_init_tests.cpp): it removes the value-initialisation
// accident, leaving the NSDMIs as the only thing that can zero these members.
TEST(SboxMemoryInitTest, RollingAverageStartsZeroedNotHeapGarbage) {
  alignas(BmwSbox) static unsigned char buf[sizeof(BmwSbox)];
  memset(buf, 0x42, sizeof(buf));
  BmwSbox* sbox = new (buf) BmwSbox;  // no parentheses: default-init, NSDMIs only

  datalayer.shunt.measured_avg1S_amperage_mA = 12345;  // so a no-op cannot pass
  set_millis64(200);
  CAN_frame f = sbox_current_frame(-500);
  sbox->handle_incoming_can_frame(f);

  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -50)
      << "the nine slots not yet written must be zero, not whatever the allocation left behind";
  sbox->~BmwSbox();
}

// ── BYD-CAN in shunt mode: the other writer of the same field ──

class BydShuntSignednessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_inverter_protocol = InverterProtocolType::BydCan;
    user_selected_inverter_deye_workaround = false;
    setup_inverter();
    ASSERT_NE(inverter, nullptr);
    byd = static_cast<BydCanInverter*>(inverter);
    byd->enable_shunt();
    clear_transmitted_frames();
  }

  BydCanInverter* byd = nullptr;
};

// 0x091 carries a SIGNED 16-bit current in bytes 2-3: 0xFFCE = -50, which the
// driver stores as -5 dA and -500 mA.  Unsigned, the dA field held 65531.
TEST_F(BydShuntSignednessTest, DischargeCurrentKeepsItsSignInTheDeciampField) {
  CAN_frame meas = {
      .FD = false, .ext_ID = false, .DLC = 8, .ID = 0x091, .data = {0x0E, 0x74, 0xFF, 0xCE, 0x00, 0xFA, 0x00, 0x00}};
  byd->map_can_frame_to_variable(meas);

  EXPECT_TRUE(datalayer.shunt.available);
  EXPECT_EQ(datalayer.shunt.measured_amperage_dA, -5) << "a discharge current must not wrap in the deci-amp field";
  EXPECT_EQ(datalayer.shunt.measured_amperage_mA, -500);
}

// ── CHEVY-VOLT-CHARGER: the clamp that assigned to the wrong variable ──

class ChevyVoltClampTest : public ::testing::Test {
 protected:
  void SetUp() override {
    user_selected_charger_type = ChargerType::ChevyVolt;
    setup_charger();
    ASSERT_NE(charger, nullptr);
    chevy = static_cast<ChevyVoltCharger*>(charger);

    // Set explicitly: the Nissan Leaf driver writes different values into these
    // globals, so a shuffled run must not inherit them.
    CHARGER_MAX_HV = 420.0f;
    CHARGER_MIN_HV = 200.0f;
    CHARGER_MAX_A = 11.5f;
    CHARGER_MAX_POWER = 3300.0f;

    clear_transmitted_frames();
    set_millis64(0);
  }

  void TearDown() override { user_selected_charger_type = ChargerType::None; }

  ChevyVoltCharger* chevy = nullptr;
};

// An over-limit current must clamp the CURRENT and leave the voltage alone.
//
// 250 V is deliberate, and it is what makes the current assertion mean
// anything: at 300 V the POWER clamp (12*300 > 3300) lands on the same 11 A by
// itself, so a build with the current clamp deleted entirely still transmits
// the expected byte.  At 250 V neither the min-voltage clamp (250 > 200) nor
// the power clamp (12*250 = 3000, not > 3300) fires, leaving the current clamp
// as the only thing that can produce 11 A.
//
// Encoding: data[1] = amps*20, data[2..3] = volts*2 big-endian.
TEST_F(ChevyVoltClampTest, OverLimitCurrentIsClampedAndTheVoltageSurvives) {
  datalayer.charger.charger_setpoint_HV_IDC = 12.0f;  // > CHARGER_MAX_A (11.5)
  datalayer.charger.charger_setpoint_HV_VDC = 250.0f;
  chevy->transmit_can(INTERVAL_200_MS);

  const CAN_frame* f = last_frame_with_id(0x304);
  ASSERT_NE(f, nullptr);
  uint16_t encoded_volts = static_cast<uint16_t>((f->data.u8[2] << 8) | f->data.u8[3]);
  EXPECT_EQ(encoded_volts, 500u) << "the requested voltage must survive a current clamp";
  // 220, not 230: setpoint_HV_IDC is a uint16_t, so the 11.5 A ceiling lands as
  // 11 A. Pre-existing, and it errs downward — noted, not changed here.
  EXPECT_EQ(f->data.u8[1], 220) << "the over-limit current must actually be clamped";
}

// The power clamp still works after the current clamp stopped corrupting the
// voltage it depends on: 11 A at 400 V is 4400 W, over the 3300 W ceiling, so
// the current backs down to floor(3300/400) = 8 A -> data[1] = 160.
TEST_F(ChevyVoltClampTest, PowerClampStillBacksCurrentDownAgainstAnIntactVoltage) {
  datalayer.charger.charger_setpoint_HV_IDC = 11.0f;
  datalayer.charger.charger_setpoint_HV_VDC = 400.0f;
  chevy->transmit_can(INTERVAL_200_MS);

  const CAN_frame* f = last_frame_with_id(0x304);
  ASSERT_NE(f, nullptr);
  uint16_t encoded_volts = static_cast<uint16_t>((f->data.u8[2] << 8) | f->data.u8[3]);
  EXPECT_EQ(encoded_volts, 800u);
  EXPECT_EQ(f->data.u8[1], 160) << "over-power must still back the current down";
}

}  // namespace
