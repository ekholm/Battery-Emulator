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
