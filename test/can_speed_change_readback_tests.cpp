#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>

/* A failed MCP2515 speed change used to be UNREPORTABLE.
 *
 * `change_can_speed()` returns true for the 2515 unconditionally, and that is
 * not a discarded status - there was no status to discard. `changeSpeed()`
 * returns void and is asynchronous: it records the request and notifies the
 * driver task, which enacted it with three register writes and never read the
 * chip back. An interface left at the old bitrate, or stuck in CONFIG mode and
 * therefore off the bus entirely, looked exactly like a speed change that
 * worked - at every layer, all the way up to the web UI.
 *
 * Neither file here is in this binary: mcp2515_lite.cpp reaches for Arduino and
 * FreeRTOS, comm_can.cpp for the ESP32 CAN drivers. So these read the source,
 * for the same reason can_init_isolation_tests.cpp does - and it is the same
 * reason the defect survived: neither file has a host build to fail. What is
 * pinned is therefore the SHAPE of the chain, anchored on the tokens that carry
 * the meaning rather than on where they sit in the file.
 */
namespace {

std::string read_source(const std::string& relative_to_test_dir) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/" + relative_to_test_dir;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

// The brace-matched body that follows `signature`. Generic, unlike the three
// single-purpose copies in can_init_isolation_tests.cpp - if a fifth extractor
// is ever wanted, hoist this one into a shared header rather than copying it.
std::string body_after(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature);
  EXPECT_NE(at, std::string::npos) << "not found, the scan has drifted: " << signature;
  if (at == std::string::npos) {
    return "";
  }
  const size_t start = src.find('{', at);
  int depth = 0;
  for (size_t i = start; i < src.size(); ++i) {
    if (src[i] == '{') {
      ++depth;
    } else if (src[i] == '}') {
      if (--depth == 0) {
        return src.substr(start, i - start + 1);
      }
    }
  }
  return "";
}

std::string driver_source() {
  return read_source("../Software/src/lib/mcp2515_lite/mcp2515_lite.cpp");
}

std::string comm_can_source() {
  return read_source("../Software/src/communication/can/comm_can.cpp");
}

// The `if (self->_speed_change_pending) { ... }` block inside the driver task.
std::string speed_change_block(const std::string& src) {
  return body_after(src, "if (self->_speed_change_pending)");
}

}  // namespace

/* The defect itself: the task requested the mode and moved on. Whatever shape
 * the verification takes, what must not come back is a bare mode request with
 * nothing asked afterwards.
 */
TEST(Mcp2515SpeedChange, TheTaskVerifiesTheModeInsteadOfAssumingIt) {
  const std::string block = speed_change_block(driver_source());
  ASSERT_FALSE(block.empty());

  EXPECT_NE(block.find("enterMode(CANCTRL_REQOP_CONFIG)"), std::string::npos)
      << "the speed change enters CONFIG mode without confirming the chip got there - the timing registers are then "
         "written to a chip that is still running at the old speed";
  EXPECT_NE(block.find("enterMode(CANCTRL_REQOP_NORMAL)"), std::string::npos)
      << "the speed change returns to NORMAL without confirming it - an interface stuck in CONFIG is off the bus "
         "entirely and nothing would say so";
  EXPECT_EQ(block.find("modifyRegister(REG_CANCTRL"), std::string::npos)
      << "a mode change in the speed-change path goes straight at the register again, which is the fire-and-forget "
         "shape this item removed";
}

/* A verdict that is computed and dropped is the same defect one layer in, so
 * pin that every step feeds it and that failure is what sets it.
 */
TEST(Mcp2515SpeedChange, EveryStepOfTheChangeFeedsOneVerdict) {
  const std::string block = speed_change_block(driver_source());
  ASSERT_FALSE(block.empty());

  EXPECT_NE(block.find("applySpeedConfig(self->_next_speed) &&"), std::string::npos)
      << "applySpeedConfig()'s answer is not folded into the verdict - a bitrate this oscillator cannot produce "
         "writes nothing and would still report success";

  const size_t set_failed = block.find("_speed_change_failed = true");
  ASSERT_NE(set_failed, std::string::npos) << "nothing in the speed-change block records a failure";

  const size_t guard = block.find("if (!changed)");
  ASSERT_NE(guard, std::string::npos) << "the failure flag is not set behind a test of the verdict";
  EXPECT_LT(guard, set_failed) << "the flag is set before the verdict is known - it would fire on every change";
}

/* enterMode() has to READ the chip. Requesting a mode and returning true is a
 * rename of the defect, not a fix.
 */
TEST(Mcp2515SpeedChange, EnterModeReadsTheStatusRegisterBack) {
  const std::string body = body_after(driver_source(), "bool MCP2515_Lite::enterMode(");
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("readRegister(REG_CANSTAT)"), std::string::npos)
      << "enterMode() never reads CANSTAT, so it cannot know whether the mode was adopted";
  EXPECT_NE(body.find("== reqop"), std::string::npos)
      << "enterMode() reads the status but does not compare it against the mode it asked for";
  EXPECT_NE(body.find("return false"), std::string::npos) << "enterMode() has no failure return";

  /* The poll must be bounded AND must be a poll: the chip finishes the frame in
   * progress before changing mode, so a single immediate read would report a
   * working interface as broken - a worse defect than the silence, and exactly
   * the trap the native path's own mutation testing found.
   */
  EXPECT_NE(body.find("MCP2515_LITE_MODE_CHANGE_ATTEMPTS"), std::string::npos)
      << "the readback is not a bounded retry - one immediate read will false-alarm on a busy chip";
  EXPECT_NE(body.find("vTaskDelay"), std::string::npos) << "the retry loop does not wait between attempts";
}

/* An unreachable bitrate writes no timing registers at all and left the chip at
 * its old speed, silently. applySpeedConfig() must say so rather than return
 * void.
 */
TEST(Mcp2515SpeedChange, AnUnreachableBitrateIsAnAnswerNotSilence) {
  const std::string src = driver_source();

  EXPECT_NE(src.find("bool MCP2515_Lite::applySpeedConfig("), std::string::npos)
      << "applySpeedConfig() is void again - a bitrate the chip cannot produce is indistinguishable from one it "
         "adopted";

  const std::string body = body_after(src, "bool MCP2515_Lite::applySpeedConfig(");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("return false"), std::string::npos) << "applySpeedConfig() cannot report the calculation failing";
  EXPECT_NE(body.find("return true"), std::string::npos) << "applySpeedConfig() never reports success";
}

/* The verdict has to reach a human. The driver cannot raise the event itself
 * without dragging the event system into a vendored driver, so the receive path
 * consumes it - the same place, and the same shape, as the hasErrors() poll
 * that was already there.
 */
TEST(Mcp2515SpeedChange, TheVerdictIsCollectedAndRaisedAsAnEvent) {
  const std::string body = body_after(comm_can_source(), "receive_frame_can_addon() {");
  ASSERT_FALSE(body.empty());

  const size_t poll = body.find("can2515->speedChangeFailed()");
  ASSERT_NE(poll, std::string::npos) << "nothing asks the driver whether the speed change worked, so the verdict is "
                                        "computed and thrown away - the failure is reportable again but unreported";

  const size_t event = body.find("EVENT_CANMCP2515_INIT_FAILURE", poll);
  EXPECT_NE(event, std::string::npos)
      << "the failed speed change raises no event - on a board without USBENABLED nothing is logged either, which "
         "is how this class of defect stays invisible";
}
