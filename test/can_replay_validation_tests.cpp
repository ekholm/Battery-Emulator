#include <gtest/gtest.h>

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
