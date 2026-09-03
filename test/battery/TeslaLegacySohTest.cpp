#include <gtest/gtest.h>

#include <initializer_list>

#include "../../Software/src/battery/TESLA-LEGACY-BATTERY.h"
#include "../../Software/src/datalayer/datalayer.h"

#include "Arduino.h"

/* Reported SOH on TESLA-LEGACY.
 *
 * The driver divides the measured minimum CAC by 231.6 Ah, the CAC-at-new of an
 * 85 kWh pack, because that is the only reference in the tree. Legacy packs run
 * 60 to 100 kWh, so a larger pack drives the quotient past 10000 and the
 * battery reports more than 100 % state of health - a number that does not stay
 * inside the box, since soh_pptt is what the inverter protocols publish.
 *
 * The clamp is the whole fix available today: it stops the over-100 % report.
 * It cannot correct a SMALLER pack, which reads too low for the same reason and
 * needs the per-hwID CAC table that does not exist yet. The last test below
 * pins that limitation so nobody reads the clamp as more than it is.
 */

namespace {

/* 0x7E2 carries CACmin as a 12-bit field split across bytes 2 and 3, and only
 * when byte 0 is zero. The driver multiplies it by 10000, so raw 2316 is
 * exactly the 231.6 Ah reference and exactly 100 %.
 */
constexpr uint16_t kCacAtNew85kWhRaw = 2316;

CAN_frame tesla_cacmin_frame(uint16_t raw_cac, uint8_t byte0 = 0) {
  CAN_frame frame = {};
  frame.DLC = 8;
  frame.ID = 0x7E2;
  frame.data.u8[0] = byte0;
  frame.data.u8[2] = static_cast<uint8_t>((raw_cac & 0x0F) << 4);
  frame.data.u8[3] = static_cast<uint8_t>((raw_cac >> 4) & 0xFF);
  return frame;
}

/* Feeds one CACmin reading through the real receive path and returns what the
 * driver published. Reaching into the member would skip the decode, which is
 * half of what makes a raw value mean an SOH.
 */
uint16_t reported_soh_pptt(uint16_t raw_cac) {
  datalayer.battery.status.soh_pptt = 0;
  TeslaLegacyBattery battery;
  battery.setup();
  battery.handle_incoming_can_frame(tesla_cacmin_frame(raw_cac));
  battery.update_values();
  return datalayer.battery.status.soh_pptt;
}

/* Same, but for a sequence of frames, so a rejected one can be shown not to
 * displace an accepted one.
 */
uint16_t reported_soh_pptt_after(std::initializer_list<CAN_frame> frames) {
  datalayer.battery.status.soh_pptt = 0;
  TeslaLegacyBattery battery;
  battery.setup();
  for (CAN_frame frame : frames) {
    battery.handle_incoming_can_frame(frame);
  }
  battery.update_values();
  return datalayer.battery.status.soh_pptt;
}

}  // namespace

TEST(TeslaLegacySoh, TheReferencePackReportsExactlyOneHundredPercent) {
  EXPECT_EQ(10000, reported_soh_pptt(kCacAtNew85kWhRaw));
}

TEST(TeslaLegacySoh, AWornPackReportsBelowOneHundredPercentUnclamped) {
  /* 1737 is three quarters of the reference and divides exactly (the quotient
   * is raw * 2500 / 579, so only multiples of 579 land on a whole pptt), which
   * keeps this an assertion about the clamp and not about rounding.
   */
  EXPECT_EQ(7500, reported_soh_pptt(1737));
}

TEST(TeslaLegacySoh, ALargerPackIsClampedInsteadOfReportingOverOneHundredPercent) {
  /* A pack whose CAC-at-new is above 231.6 Ah reads 116.58 % unclamped. */
  EXPECT_EQ(10000, reported_soh_pptt(2700));
}

TEST(TeslaLegacySoh, TheLargestEncodableCacIsStillClamped) {
  /* The field is 12 bits, so 4095 is the most the bus can say: 176.81 %
   * unclamped, and still inside uint16_t - which is why nothing else caught it.
   */
  EXPECT_EQ(10000, reported_soh_pptt(0x0FFF));
}

TEST(TeslaLegacySoh, OneCountAboveTheReferenceIsAlreadyClamped) {
  /* The boundary itself: 2317 is the first raw value that exceeds 100 %. */
  EXPECT_EQ(10000, reported_soh_pptt(kCacAtNew85kWhRaw + 1));
}

/* What the clamp does NOT fix, asserted so the limitation is on the record and
 * not just in a comment: a 60 kWh pack's CAC-at-new is well under 231.6 Ah, so
 * a healthy one reports far below 100 % and the clamp is silent about it.
 */
TEST(TeslaLegacySoh, ASmallerPackStillUnderReportsAndTheClampCannotHelp) {
  /* A brand-new 60 kWh pack, whose CAC-at-new is about half the reference. */
  const uint16_t reported = reported_soh_pptt(1158);

  EXPECT_EQ(5000, reported);
  EXPECT_LT(reported, 10000);
}

/* The driver takes CACmin off 0x7E2 only when byte 0 is zero, and nothing in
 * the tree documents what that byte is - so this pins the behaviour, not a
 * reading of its meaning. It is here because a mutation earned it: opening the
 * gate to every 0x7E2 frame passed the whole suite, which means a change to
 * that condition could not be told apart from no change at all.
 */
TEST(TeslaLegacySoh, A7E2FrameWithANonZeroFirstByteDoesNotDisplaceTheCac) {
  const uint16_t reported = reported_soh_pptt_after({tesla_cacmin_frame(1737), tesla_cacmin_frame(2700, 0x01)});

  EXPECT_EQ(7500, reported);
}
