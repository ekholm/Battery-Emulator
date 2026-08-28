#include <gtest/gtest.h>

#include "../Software/src/devboard/webserver/debug_log_status.h"

// The debug-log readouts (/log and /export_log's RAM path) explain an empty
// buffer through this one helper. The contract under test: "web logging is
// off", "the CAN logger owns the shared buffer", and "logging is on but
// nothing was printed" are DIFFERENT claims - conflating them is the same
// defect the CAN-log readout had, arriving on the debug path. The two loggers
// share one RAM buffer: the debug writers run only while web_logging_active
// && !can_logging_active.

namespace {

TEST(DebugLogStatusTest, WebLoggingOffSaysOffAndNamesTheSettingAndTheReboot) {
  std::string msg = debug_log_empty_explanation(false, false);
  EXPECT_NE(msg.find("Web logging is off"), std::string::npos);
  // WEBENABLED is read once at boot (comm_nvm.cpp) - without the reboot the
  // reader flips the checkbox and wonders why nothing changes.
  EXPECT_NE(msg.find("General logging via Webserver"), std::string::npos);
  EXPECT_NE(msg.find("reboot"), std::string::npos);
  // The off-state must not read like a statement about firmware activity.
  EXPECT_EQ(msg.find("nothing has been logged"), std::string::npos);
}

TEST(DebugLogStatusTest, CanLoggerOwnershipIsNamedWhileItHoldsTheBuffer) {
  std::string msg = debug_log_empty_explanation(true, true);
  EXPECT_NE(msg.find("CAN logger"), std::string::npos);
  EXPECT_NE(msg.find("not recorded"), std::string::npos);
  EXPECT_EQ(msg.find("Web logging is off"), std::string::npos);
}

TEST(DebugLogStatusTest, ActiveButEmptySaysNothingLoggedAndDoesNotSayOff) {
  std::string msg = debug_log_empty_explanation(true, false);
  EXPECT_NE(msg.find("nothing has been logged"), std::string::npos);
  EXPECT_EQ(msg.find("Web logging is off"), std::string::npos);
  EXPECT_EQ(msg.find("CAN logger"), std::string::npos);
}

TEST(DebugLogStatusTest, OffWinsOverCanOwnership) {
  // With web logging off the buffer would not fill either way; naming the CAN
  // logger would misdirect the reader toward a knob that is not the reason.
  std::string msg = debug_log_empty_explanation(false, true);
  EXPECT_NE(msg.find("Web logging is off"), std::string::npos);
  EXPECT_EQ(msg.find("CAN logger"), std::string::npos);
}

}  // namespace
