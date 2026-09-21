#include <gtest/gtest.h>

#include <string>

#include "source_scan.h"

/* A failed CAN-FD init must tear the driver down BEFORE it clears the pointer
 * the driver's interrupt reads.
 *
 * `ACAN2517FD::begin()` installs its nINT handler and starts its polling task
 * inside the block that can still go on to report `kRequestedModeTimeOut`, so
 * a non-zero return does NOT mean the driver is inert. The handler upstream
 * installs is the captureless lambda `[] { canfd->isr(); }` - captureless
 * because begin() takes a plain `void(*)()` - so it reads the GLOBAL. Clearing
 * that global while the handler is attached means the next falling edge on
 * nINT evaluates `canfd->isr()`, which on ESP32 is one member access
 * (`mISRSemaphore`) off a null pointer, inside an ISR.
 *
 * None of this is reachable from the host binary: comm_can.cpp is outside the
 * curated source list in test/CMakeLists.txt, and bringing it in means
 * bringing in ACAN2517FD, mcp2515_lite, the SD card and the streaming
 * webserver. So these tests read the two files as text. That is a weaker
 * instrument than linking them, and it is the strongest one available - and
 * the placement it guards is exactly the kind that reads fine and is wrong.
 *
 * Two halves. The first pins the fix in comm_can.cpp. The second pins the
 * PREMISE in the vendored library, because the fix is only necessary while
 * begin() can return non-zero with the handler live, and the vendored tree
 * gets bumped by people who have never read comm_can.cpp.
 */

namespace {

using namespace source_scan;

constexpr const char* kCommCan = "../Software/src/communication/can/comm_can.cpp";
constexpr const char* kAcan2517 = "../Software/src/lib/pierremolinaro-ACAN2517FD/ACAN2517FD.cpp";

// The failure block of one begin_canfd*(), i.e. the `{ ... }` that follows the
// `if (errorCode... != 0)` inside it. Both statements under test have to live
// in the SAME block: a teardown that sits outside the failure branch runs on
// the success path too, and one that sits in a different branch never runs.
std::string failure_block(const std::string& function) {
  const size_t guard = function.find("!= 0)");
  EXPECT_NE(guard, std::string::npos) << "no `!= 0)` failure guard in this function";
  if (guard == std::string::npos) {
    return "";
  }
  return brace_block_at(function, guard);
}

struct DriverCase {
  const char* signature;  // the begin_canfd*() under test
  const char* driver;     // the global it clears
  const char* sibling;    // the OTHER global, which it must NOT tear down
};

const DriverCase kDrivers[] = {
    {"static bool begin_canfd() {", "canfd", "canfd_2"},
    {"static bool begin_canfd_2() {", "canfd_2", "canfd"},
};

class CanFdInitTeardown : public ::testing::TestWithParam<DriverCase> {};

// The whole defect in one assertion: the pointer is cleared, and something
// tore the driver down first.
TEST_P(CanFdInitTeardown, TheFailurePathTearsTheDriverDownBeforeClearingIt) {
  const DriverCase& c = GetParam();
  const std::string block = failure_block(function_body(read_source(kCommCan), c.signature));

  const std::string clear = std::string(c.driver) + " = nullptr;";
  const std::string teardown = std::string(c.driver) + "->end();";

  const size_t at_clear = block.find(clear);
  const size_t at_teardown = block.find(teardown);

  ASSERT_NE(at_clear, std::string::npos) << "no `" << clear << "` on the failure path";
  ASSERT_NE(at_teardown, std::string::npos)
      << "`" << clear << "` with no `" << teardown << "` before it: begin() can return non-zero "
      << "with the nINT handler still attached, and the handler reads this global";
  EXPECT_LT(at_teardown, at_clear)
      << "`" << teardown << "` must come BEFORE `" << clear << "` - after it, the teardown is "
      << "itself a null dereference";
}

// begin_canfd_2() is a copy of begin_canfd(). The copy that tears down its
// sibling would leave its own chip's handler attached AND kill a working one,
// and reads almost identically.
TEST_P(CanFdInitTeardown, TheTeardownNamesTheDriverThePathIsAbandoning) {
  const DriverCase& c = GetParam();
  const std::string block = failure_block(function_body(read_source(kCommCan), c.signature));

  EXPECT_EQ(block.find(std::string(c.sibling) + "->end();"), std::string::npos)
      << "this failure path tears down " << c.sibling << ", which is not the driver it clears";
}

INSTANTIATE_TEST_SUITE_P(BothChips, CanFdInitTeardown, ::testing::ValuesIn(kDrivers));

/* The premise, read from the vendored library.
 *
 * If a library bump ever moves the requested-mode wait above the install
 * block, `begin()` returning non-zero WOULD mean the driver is inert and the
 * teardown above becomes dead code. That is a good outcome and a silent one:
 * nothing in comm_can.cpp would notice. This test is the notification.
 */
class Acan2517FdInstallBlock : public ::testing::Test {
 protected:
  // The `if (errorCode == 0) { ... }` block that performs the install, found
  // by the attach it contains rather than by counting guards.
  //
  // begin() is OVERLOADED and the two-argument forwarder is defined FIRST, so
  // the signature has to name the filters parameter: anchored on the shared
  // prefix, this reads the six-line forwarder and every assertion below comes
  // back "not found" for the wrong reason.
  std::string install_block() {
    const std::string src = read_source(kAcan2517);
    const std::string kSignature = "const ACAN2517FDFilters & inFilters) {";
    EXPECT_EQ(src.find(kSignature), src.rfind(kSignature))
        << "`" << kSignature << "` is no longer unique - this test would read an overload";

    const std::string begin = function_body(src, kSignature);
    const size_t attach = begin.find("attachInterrupt (itPin");
    EXPECT_NE(attach, std::string::npos) << "ACAN2517FD::begin() no longer attaches by that name";
    if (attach == std::string::npos) {
      return "";
    }
    // The last install guard at or above the attach is the block that owns it.
    const size_t guard = begin.rfind("if (errorCode == 0) {", attach);
    EXPECT_NE(guard, std::string::npos) << "the attach is no longer under an `errorCode == 0` guard";
    if (guard == std::string::npos) {
      return "";
    }
    return brace_block_at(begin, guard);
  }
};

TEST_F(Acan2517FdInstallBlock, ANonZeroReturnCanStillLeaveTheHandlerAttached) {
  const std::string block = install_block();
  const size_t err = block.find("errorCode |=");
  const size_t attach = block.find("attachInterrupt (itPin");

  ASSERT_NE(attach, std::string::npos);
  ASSERT_NE(err, std::string::npos)
      << "no error code is set inside the block that attaches the handler any more. If the "
      << "vendored ACAN2517FD was bumped, a non-zero begin() may now mean the driver IS inert - "
      << "re-derive the failure path in comm_can.cpp before trusting its teardown or dropping it";
  EXPECT_LT(err, attach) << "the error code is set after the attach, not before it";
}

// The second reason the failure path calls end() rather than a bare
// detachInterrupt: the driver's polling task is started unconditionally, above
// the attach, so it outlives a failure even on a board declaring no interrupt.
TEST_F(Acan2517FdInstallBlock, ThePollingTaskIsStartedWithoutRegardToTheErrorCode) {
  const std::string block = install_block();
  const size_t task = block.find("xTaskCreate (myESP32Task");
  const size_t attach = block.find("attachInterrupt (itPin");

  ASSERT_NE(task, std::string::npos) << "the ACAN2517Handler task is no longer started here";
  ASSERT_NE(attach, std::string::npos);
  EXPECT_LT(task, attach) << "the task start moved below the attach";

  // Nothing between the timeout and the task start consults the code, so the
  // task runs whatever begin() is about to return. The timeout assignment
  // being the LAST mention of errorCode in the block is how that is checked:
  // any later mention is a candidate guard, and a guarded task start would
  // make a bare detachInterrupt sufficient and end() more than is needed.
  const size_t err = block.find("errorCode |=");
  ASSERT_NE(err, std::string::npos);
  EXPECT_EQ(block.rfind("errorCode"), err)
      << "errorCode is mentioned again after the requested-mode timeout - re-derive whether the "
      << "task start is now guarded before trusting this reason for calling end()";
}

}  // namespace
