#include <gtest/gtest.h>

#include "WString.h"

namespace {

// Arduino's String null-guards on both the const char* constructor and operator+=; the emul did
// not, and std::string throws instead. The codebase meets a null on exactly this path: the Tesla
// battery HTML renders battery_manufactureDate, a plain char* that stays null until a CAN frame
// parses the serial number. That renders empty on hardware, so a host build that throws is the
// harness diverging from the firmware, not a defect the firmware has.
TEST(EmulStringNullGuard, ConstructingFromNullYieldsAnEmptyStringLikeArduino) {
  const char* absent = nullptr;

  String s(absent);

  EXPECT_EQ(s.length(), 0);
  EXPECT_STREQ(s.c_str(), "");
}

TEST(EmulStringNullGuard, AppendingNullLeavesTheStringUnchangedLikeArduino) {
  const char* absent = nullptr;
  String s("kept");

  s += absent;

  EXPECT_STREQ(s.c_str(), "kept");
}

}  // namespace
