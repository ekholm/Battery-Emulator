#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "../Software/src/devboard/webserver/can_replay_validation.h"

// A CAN replay that cannot reach any wire must be refused up front - stored
// unvalidated, it is indistinguishable on every page from one that is
// transmitting. The decision logic is a pure function of the requested
// number and a readiness predicate, so the whole matrix runs on the host.

namespace {

bool all_ready(int) {
  return true;
}
bool none_ready(int) {
  return false;
}
bool only_native_ready(int interface) {
  return interface == 0;
}

}  // namespace

TEST(CanReplayValidationTest, InRangeReadyInterfaceIsAccepted) {
  EXPECT_EQ(can_replay_interface_rejection(0, all_ready), "");
  EXPECT_EQ(can_replay_interface_rejection(4, all_ready), "");
}

// The cross-dialect trap by its literal value: 5 is the MCP2515 add-on in the
// settings/BATTCOMM dialect but NO_CAN_INTERFACE in the replay's enum. The
// rejection must both refuse it and NAME the trap, or the person who typed it
// retypes it.
TEST(CanReplayValidationTest, TheDialectTrapValueFiveIsRefusedAndExplained) {
  std::string msg = can_replay_interface_rejection(5, all_ready);
  ASSERT_NE(msg, "");
  EXPECT_NE(msg.find("Invalid CAN interface 5"), std::string::npos);
  EXPECT_NE(msg.find("MCP2515"), std::string::npos) << "the settings-dialect meaning of 5 must be named";
  EXPECT_NE(msg.find("no interface at all"), std::string::npos);
}

TEST(CanReplayValidationTest, OutOfRangeValuesAreRefusedWithTheValidNumbering) {
  for (int bad : {-1, 6, 42, 179}) {
    std::string msg = can_replay_interface_rejection(bad, all_ready);
    ASSERT_NE(msg, "") << "value " << bad << " accepted";
    EXPECT_NE(msg.find("0=CAN Native"), std::string::npos) << "the valid numbering must be listed";
  }
}

// In range but the driver never came up on this board: frames would land in a
// null driver, and the only symptom without this refusal is a buffer-full
// count while the canlog shows a healthy replay.
TEST(CanReplayValidationTest, UnreadyInterfaceIsRefusedWithTheReason) {
  std::string msg = can_replay_interface_rejection(2, only_native_ready);
  ASSERT_NE(msg, "");
  EXPECT_NE(msg.find("not initialized on this board"), std::string::npos);
  EXPECT_NE(msg.find("no wire"), std::string::npos);
  // And the same number on a board where it IS up sails through.
  EXPECT_EQ(can_replay_interface_rejection(2, all_ready), "");
}

TEST(CanReplayValidationTest, ReadinessIsConsultedOnlyForInRangeValues) {
  // Out-of-range must be refused as INVALID even if the predicate would lie -
  // range first, readiness second, so the dialect explanation is never
  // shadowed by the wiring one.
  std::string msg = can_replay_interface_rejection(5, none_ready);
  EXPECT_NE(msg.find("Invalid CAN interface"), std::string::npos);
  EXPECT_EQ(msg.find("not initialized"), std::string::npos);
}

/* The DLC bound.
 *
 * The parser copied `[n]` tokens into CAN_frame's 64-byte array with n read
 * straight from the uploaded line into a uint8_t - so 0..255 against 64, and a
 * line declaring 200 bytes with 200 tokens wrote 136 past the end of a
 * file-scope frame. Every case below is a line an upload can contain.
 */
namespace {
constexpr size_t kFrameCapacity = 64;  // sizeof(CAN_frame::data.u8)
}

TEST(CanReplayDlcBound, EveryLengthAFrameCanActuallyCarryIsAccepted) {
  for (long n = 0; n <= (long)kFrameCapacity; n++) {
    EXPECT_EQ(can_replay_dlc_rejection(n, kFrameCapacity), "") << "length " << n << " must be accepted";
  }
}

TEST(CanReplayDlcBound, TheFirstLengthPastTheEndIsRefused) {
  EXPECT_EQ(can_replay_dlc_rejection(64, kFrameCapacity), "") << "the last byte that fits";
  EXPECT_NE(can_replay_dlc_rejection(65, kFrameCapacity), "") << "one past the end must not be copied";
}

// The reported overrun: 200 tokens into 64 bytes is 136 past the end of a
// file-scope frame, so it corrupts .bss rather than smashing a return address.
TEST(CanReplayDlcBound, TheReportedOverrunIsRefusedAndSaysWhy) {
  const std::string rejection = can_replay_dlc_rejection(200, kFrameCapacity);
  ASSERT_NE(rejection, "");
  EXPECT_NE(rejection.find("200"), std::string::npos) << "name the length the line declared: " << rejection;
  EXPECT_NE(rejection.find("64"), std::string::npos) << "and the length that would fit: " << rejection;
}

/* The check has to happen on the value as PARSED. `[300]` narrows to 44 in a
 * uint8_t, so a bound applied after the assignment sees a value that fits and
 * replays a 44-byte frame the file never described.
 */
TEST(CanReplayDlcBound, ALengthThatWouldNarrowIntoRangeIsStillRefused) {
  EXPECT_NE(can_replay_dlc_rejection(300, kFrameCapacity), "") << "300 narrows to 44 in a uint8_t";
  EXPECT_NE(can_replay_dlc_rejection(256, kFrameCapacity), "") << "256 narrows to 0";
  EXPECT_NE(can_replay_dlc_rejection(320, kFrameCapacity), "") << "320 narrows to 64, exactly the capacity";
}

// toInt() yields 0 for text it cannot parse, which is a legal empty frame - but
// an explicitly negative length is a malformed line, not an empty one.
TEST(CanReplayDlcBound, ANegativeLengthIsRefusedRatherThanWrappingHuge) {
  const std::string rejection = can_replay_dlc_rejection(-1, kFrameCapacity);
  ASSERT_NE(rejection, "");
  /* Pin the MESSAGE, not just the refusal. Without its own check a negative
   * length is still refused - it becomes enormous when compared as unsigned -
   * but the line it prints quotes that enormous number, which sends whoever
   * reads the log looking for a frame length nothing in the file declared. */
  EXPECT_NE(rejection.find("negative"), std::string::npos) << "say it was negative: " << rejection;
  EXPECT_EQ(rejection.find("18446744073709551615"), std::string::npos)
      << "the wrapped value must not reach the log: " << rejection;

  EXPECT_EQ(can_replay_dlc_rejection(0, kFrameCapacity), "") << "an empty frame is a frame";
}

/* The bound is only worth anything if the CALL SITE hands it the value as parsed.
 *
 * Narrowing before the check is the second half of the defect this fix exists to
 * close: `[300]` becomes 44 in a uint8_t, the bound then sees a length that fits,
 * and the replay sends a 44-byte frame the file never described. The helper above
 * refuses 300 correctly - but nothing above pins that the caller passes 300 rather
 * than 44, and `webserver.cpp` is not part of this binary, so a cast added at the
 * call site restores the bug with the whole suite still green. Verified: it does.
 *
 * So this reads the call site, the way the other tests over that file do. It is a
 * weaker instrument than running the code and it is here because the code cannot be
 * run; the shape that would retire it is a helper that parses the token itself, so
 * no caller ever holds a wide value to narrow.
 */
namespace {

std::string webserver_source() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  std::ifstream src(dir + "/../Software/src/devboard/webserver/webserver.cpp");
  EXPECT_TRUE(src.is_open()) << "webserver.cpp is not where this test looks";
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(CanReplayDlcBound, TheCallSiteChecksTheParsedValueNotTheNarrowedOne) {
  const std::string src = webserver_source();

  const size_t call = src.find("can_replay_dlc_rejection(");
  ASSERT_NE(call, std::string::npos) << "the replay parser no longer calls the bound - re-check this test";
  const std::string args = src.substr(call, src.find(')', call) - call);
  EXPECT_EQ(args.find("uint8_t"), std::string::npos)
      << "the declared length is narrowed before it is checked, which is the exact bug the check "
         "exists to close: a line declaring 300 becomes 44 and passes. Pass the parsed value.\n  "
      << args;

  const size_t assign = src.find("currentFrame.DLC =");
  ASSERT_NE(assign, std::string::npos) << "the DLC assignment moved - re-check this test";
  EXPECT_LT(call, assign) << "the bound must run BEFORE the value reaches the uint8_t field";
}

/* The data-byte parser. The strtok scan moved out of webserver.cpp
 * (which no host binary compiles) into can_replay_parse_data, so the bug it
 * retires can finally be pinned on the host: a NULL or empty data field must
 * parse zero bytes WITHOUT calling strtok on NULL - which would continue the
 * previous line's freed scan and put freed heap on the wire. The caller skips
 * a line whose supplied count is short of its declared DLC, which is why a
 * valid zero-length frame ([0], emitted by the log writer as "...] ") still
 * transmits while a [8]-with-no-data line does not.
 */
namespace {
// strtok mutates its input, so every case parses from its own writable buffer.
size_t parse(const char* line, size_t declared, unsigned char* out) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s", line);
  return can_replay_parse_data(buf, declared, out);
}
}  // namespace

TEST(CanReplayParseData, ANullDataFieldParsesZeroAndNeverTouchesStrtok) {
  // The defect: a line ending at the ']' gave a default-constructed String,
  // c_str()/begin() == NULL, and strtok(NULL,...) continued the previous scan.
  unsigned char out[64] = {0xAB};
  EXPECT_EQ(can_replay_parse_data(nullptr, 8, out), 0u);
  EXPECT_EQ(out[0], 0xAB);  // untouched - nothing was parsed
}

TEST(CanReplayParseData, AnEmptyDataFieldParsesZero) {
  unsigned char out[64] = {0xAB};
  EXPECT_EQ(parse("", 8, out), 0u);
  EXPECT_EQ(out[0], 0xAB);
}

TEST(CanReplayParseData, TheBytesActuallySuppliedAreParsedInOrder) {
  unsigned char out[64] = {0};
  EXPECT_EQ(parse("11 22 33", 3, out), 3u);
  EXPECT_EQ(out[0], 0x11);
  EXPECT_EQ(out[1], 0x22);
  EXPECT_EQ(out[2], 0x33);
}

TEST(CanReplayParseData, ParsingStopsAtTheDeclaredLengthEvenIfMoreAreSupplied) {
  // declared is the already-bounded DLC; the parser must not write past it.
  unsigned char out[64] = {0};
  EXPECT_EQ(parse("11 22 33 44 55", 2, out), 2u);
  EXPECT_EQ(out[2], 0);  // the third token never reached the array
}

TEST(CanReplayParseData, AShortLineReportsFewerThanDeclaredSoTheCallerCanSkip) {
  // [8] with three tokens: the caller sees 3 < 8 and skips - it would otherwise
  // transmit stale data for the missing five bytes.
  unsigned char out[64] = {0};
  EXPECT_EQ(parse("11 22 33", 8, out), 3u);
}

TEST(CanReplayParseData, AZeroLengthFrameParsesZeroAndIsNotShort) {
  // The log writer emits "...[0] " for a valid zero-length CAN frame. supplied
  // (0) is not < declared (0), so the caller transmits it rather than dropping
  // it - the behaviour a literal "skip when the field is empty" guard would
  // have lost.
  unsigned char out[64] = {0};
  EXPECT_EQ(parse("", 0, out), 0u);
}

TEST(CanReplayParseData, TheCallSiteUsesTheHelperAndSkipsAShortLine) {
  // webserver.cpp is not in any host binary, so pin the call site by reading it:
  // the raw strtok scan and its const-cast must be gone, and the line must be
  // skipped when the helper reports fewer bytes than the DLC declared.
  const std::string src = webserver_source();

  EXPECT_NE(src.find("can_replay_parse_data("), std::string::npos)
      << "the replay parser no longer routes data bytes through the host-tested helper";
  EXPECT_EQ(src.find("strtok((char*)"), std::string::npos)
      << "the const-cast strtok scan is back in the webserver, where the NULL data field UAF lived "
         "and no host test can see it";
  EXPECT_EQ(src.find("strtok(NULL"), std::string::npos)
      << "a bare strtok(NULL, ...) is back at the call site - it continues the previous line's freed scan";

  const size_t call = src.find("can_replay_parse_data(");
  const std::string after = src.substr(call, 400);
  // The exact guard, so a `false &&` or other weakening that keeps the operands
  // around does not pass on the substring alone.
  EXPECT_NE(after.find("if (suppliedBytes < currentFrame.DLC)"), std::string::npos)
      << "the call site no longer skips a line that supplied fewer bytes than it declared - it would "
         "transmit a frame the file did not fully describe.\n  "
      << after;
  const size_t guard = after.find("if (suppliedBytes < currentFrame.DLC)");
  if (guard != std::string::npos) {
    EXPECT_NE(after.find("continue;", guard), std::string::npos) << "the short-line guard no longer skips the line";
  }
}
