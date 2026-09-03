#include <gtest/gtest.h>

#include <cstring>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/inverter/BYD-CAN.h"

/* The 0x151 brand-identification filter had two independent defects, and together they
 * meant `inverter_brand` was never once correct.
 *
 *   if ((u8[i] > 0x40) && (u8[i] > 0x7B))   // both operators `>`
 *       inverter_brand[i] = u8[i + 1];      // tests one byte, copies another
 *
 * The doubled `>` collapses the condition to `> 0x7B`, so it admitted exactly the bytes
 * its own comment calls invalid and rejected the whole printable range it was written to
 * accept. The offset then meant the surviving guard governed a byte other than the one
 * stored. Byte 0 carries the identification-request flag, so bytes 1..7 are the string and
 * the copy's offset was right - it was the test that needed to move with it.
 *
 * These are characterization tests for the FIXED behaviour: the defect was found while
 * writing the driver test suite and deliberately left unpinned, which is why it survived.
 */
namespace {

CAN_frame identification_frame(const char* seven) {
  CAN_frame f{};
  f.ID = 0x151;
  f.DLC = 8;
  f.data.u8[0] = 0x00;  // NOT a request for identification: the branch that reads the name
  for (int i = 0; i < 7; i++) {
    f.data.u8[i + 1] = seven[i] ? (uint8_t)seven[i] : 0x00;
  }
  return f;
}

std::string brand_after(const CAN_frame& f) {
  std::memset(datalayer.system.info.inverter_brand, 0, sizeof(datalayer.system.info.inverter_brand));
  BydCanInverter inverter;
  inverter.map_can_frame_to_variable(f);
  return std::string(datalayer.system.info.inverter_brand);
}

}  // namespace

TEST(BydBrandFilter, APrintableNameIsStoredInOrderAndNotShifted) {
  // The whole point: the string that arrives is the string that is stored.
  EXPECT_EQ(brand_after(identification_frame("GoodWes")), "GoodWes");
}

/* THE RANGE REJECTS DIGITS AND SPACES, and that is pre-existing rather than introduced
 * here: `> 0x40 && < 0x7B` is 'A'..'z', so 0x30-0x39 and 0x20 never reach the string. This
 * fix corrects the operators and the index; WIDENING the range would be a different change
 * and is not made here.
 *
 * It matters for filing, though, and the item says so: a real brand string carrying a digit
 * or a space - "GoodWe1", "SMA 5000" - is silently truncated at that character. Nobody has
 * reported it because until now the filter admitted almost nothing at all. Pinned so the
 * behaviour is visible rather than discovered against a real inverter.
 */
TEST(BydBrandFilter, DigitsAndSpacesAreRejectedByTheInheritedRange) {
  EXPECT_EQ(brand_after(identification_frame("GoodWe1")), "GoodWe") << "a trailing digit is dropped";
  EXPECT_EQ(brand_after(identification_frame("SMAxx5000")), "SMAxx") << "and a digit mid-name truncates there";
}

TEST(BydBrandFilter, TheRangeAcceptsItsOwnBoundsAndRejectsJustOutsideThem) {
  // The guard is `> 0x40 && < 0x7B`, i.e. 'A'..'z' inclusive. Pin both edges and the two
  // bytes immediately outside them - the doubled-`>` defect got every one of these wrong.
  EXPECT_EQ(brand_after(identification_frame("AAAAAAA")), "AAAAAAA") << "0x41 is the first accepted byte";
  EXPECT_EQ(brand_after(identification_frame("zzzzzzz")), "zzzzzzz") << "0x7A is the last accepted byte";

  CAN_frame low = identification_frame("AAAAAAA");
  low.data.u8[1] = 0x40;  // one below 'A'
  EXPECT_EQ(brand_after(low), "") << "0x40 is rejected, so the string terminates immediately";

  CAN_frame high = identification_frame("AAAAAAA");
  high.data.u8[1] = 0x7B;  // one above 'z'
  EXPECT_EQ(brand_after(high), "") << "0x7B is rejected - it is the byte the old code uniquely ACCEPTED";
}

/* The bytes the broken filter used to be the only ones to pass. A regression to `> 0x7B`
 * would store these and reject everything above, so this is the case that fails loudest
 * if the comparison is ever flipped back.
 */
TEST(BydBrandFilter, BytesAboveTheRangeAreRejectedRatherThanBeingTheOnlyOnesAccepted) {
  CAN_frame f = identification_frame("AAAAAAA");
  for (int i = 1; i <= 7; i++) {
    f.data.u8[i] = 0xC3;  // well above 0x7B: high-bit bytes a real frame can carry
  }
  EXPECT_EQ(brand_after(f), "") << "nothing above 'z' reaches the brand string";
}

/* A rejected byte leaves its slot untouched, and the array starts zeroed - so an invalid
 * byte truncates the name there rather than shifting the rest up. Worth pinning because
 * the fix makes rejection COMMON where the broken filter made it almost universal, so this
 * is newly reachable behaviour rather than a corner.
 */
TEST(BydBrandFilter, AnInvalidByteTruncatesRatherThanShiftingTheRestForward) {
  CAN_frame f = identification_frame("GoodWes");
  f.data.u8[4] = 0x20;  // a space, mid-name: below 'A', so rejected
  EXPECT_EQ(brand_after(f), "Goo") << "the name stops at the hole; later bytes do not move up";
}

// Byte 0 is the request flag, not part of the name: setting bit 0 takes the other branch
// entirely, and the name must not be read from a frame that is asking for identification.
TEST(BydBrandFilter, AFrameRequestingIdentificationDoesNotPopulateTheName) {
  CAN_frame f = identification_frame("GoodWes");
  f.data.u8[0] = 0x01;
  EXPECT_EQ(brand_after(f), "") << "the request branch sends data, it does not read a name";
}

/* THE DESTINATION IS NEVER CLEARED BETWEEN FRAMES, so a rejected byte has to clear its
 * own slot rather than merely be skipped.
 *
 * The helper above memsets the brand before every call, which is right for pinning one
 * frame's behaviour and is exactly why this case needs its own: production has no such
 * reset. Two frames in a row, the second name shorter than the first, and the leftover
 * tail of the first is read as part of the second - "GoodWes" then "SMA" produced
 * "SMAdWes", a brand string that never appeared on any wire.
 *
 * Like the truncation case this is NEWLY reachable: while the broken filter accepted
 * almost nothing the array was almost never written, so there was nothing to leave behind.
 */
TEST(BydBrandFilter, AShorterNameDoesNotInheritTheTailOfTheOneBefore) {
  std::memset(datalayer.system.info.inverter_brand, 0, sizeof(datalayer.system.info.inverter_brand));
  BydCanInverter inverter;

  inverter.map_can_frame_to_variable(identification_frame("GoodWes"));
  ASSERT_EQ(std::string(datalayer.system.info.inverter_brand), "GoodWes") << "the long name first";

  inverter.map_can_frame_to_variable(identification_frame("SMA"));
  EXPECT_EQ(std::string(datalayer.system.info.inverter_brand), "SMA")
      << "the shorter name kept the previous one's tail - the brand reported is a splice of "
         "two frames rather than anything the inverter sent";
}

/* The range's INTERIOR, which nothing else pins.
 *
 * `> 0x40 && < 0x7B` is not "letters": it also admits 0x5B-0x60, the six punctuation bytes
 * between 'Z' and 'a'. That matters because the obvious tidy-up - swapping the range for
 * isalpha() - still accepts 'A' and 'z' and still rejects 0x40 and 0x7B, so it passes the
 * bounds case above while silently changing what the filter does. The bounds test was the
 * sole cover for the whole guard; this shares it.
 */
TEST(BydBrandFilter, TheRangeIsAByteRangeAndNotALetterTest) {
  CAN_frame f = identification_frame("AAAAAAA");
  const uint8_t between_Z_and_a[6] = {0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60};
  for (int i = 0; i < 6; i++) {
    f.data.u8[i + 1] = between_Z_and_a[i];
  }
  EXPECT_EQ(brand_after(f), "[\\]^_`A")
      << "the inherited guard is a byte range, so the six non-letters between 'Z' and 'a' "
         "are accepted; a letters-only test would pass every other case in this file";
}
