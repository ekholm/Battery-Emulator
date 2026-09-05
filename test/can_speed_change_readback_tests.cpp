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
  /* The return to the running mode must be verified too. The readback change
   * pinned the literal `enterMode(CANCTRL_REQOP_NORMAL)`; the gate made that
   * mode conditional
   * on how begin() was opened, so what is pinned now is the SHAPE - two verified
   * mode changes, config and restore - which is what the assertion always meant.
   * Which mode it restores is Mcp2515ModeGate.ASpeedChangeReturnsToTheModeBeginStartedIn.
   */
  size_t verified_mode_changes = 0;
  for (size_t at = block.find("enterMode("); at != std::string::npos; at = block.find("enterMode(", at + 1)) {
    ++verified_mode_changes;
  }
  EXPECT_GE(verified_mode_changes, 2u)
      << "the speed change does not verify BOTH mode changes - an interface left stuck in CONFIG is off the bus "
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
 *
 * What "unreachable" MEANS is no longer scanned for: the arithmetic moved
 * into mcp2515_timing.cpp and mcp2515_timing_tests.cpp calls it. This test is
 * now only about the chain - that the verdict has a return value to travel on.
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

/* The timing arithmetic must stay in the TU the tests can reach.
 *
 * A private re-implementation inside the driver would compile, link and pass
 * every other test in this file while putting the tolerance check back out of
 * reach - which is precisely the state that let the defect live for years. So
 * pin the delegation, not just the behaviour.
 */
TEST(Mcp2515TimingSeam, TheDriverDelegatesTheTimingArithmetic) {
  const std::string src = driver_source();

  const std::string body = body_after(src, "bool MCP2515_Lite::applySpeedConfig(");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("mcp2515_calculate_timing("), std::string::npos)
      << "applySpeedConfig() no longer calls the extracted, host-tested function";

  EXPECT_EQ(src.find("static bool calculateMCP2515Config"), std::string::npos)
      << "the file-static copy is back - whatever it computes is untestable again";
  EXPECT_NE(src.find("#include \"mcp2515_timing.h\""), std::string::npos)
      << "the driver does not include the timing header";
}

/* The verdict has to reach a human. The driver cannot raise the event itself
 * without dragging the event system into a vendored driver, so the receive path
 * consumes it - the same place, and the same shape, as the hasErrors() poll
 * that was already there.
 */
TEST(Mcp2515SpeedChange, TheVerdictIsCollectedAndRaisedAsAnEvent) {
  // The poll moved out of receive_frame_can_addon() into its own function,
  // because it must run even when the interface is gated OFF. The scan follows it.
  const std::string body = body_after(comm_can_source(), "static void poll_can_addon_speed_change() {");
  ASSERT_FALSE(body.empty());

  const size_t poll = body.find("can2515->speedChangeFailed()");
  ASSERT_NE(poll, std::string::npos) << "nothing asks the driver whether the speed change worked, so the verdict is "
                                        "computed and thrown away - the failure is reportable again but unreported";

  const size_t event = body.find("EVENT_CANMCP2515_INIT_FAILURE", poll);
  EXPECT_NE(event, std::string::npos)
      << "the failed speed change raises no event - on a board without USBENABLED nothing is logged either, which "
         "is how this class of defect stays invisible";
}

/* ------------------------------------------------------------------------- *
 * The three gaps the readback change left.
 * ------------------------------------------------------------------------- */

/* (a) Reporting a failure while leaving the interface in service is the defect
 * the init and speed-change fixes made on the native path: the failure travels as a return value
 * while the state that decides whether the interface is USED still says it is
 * fine. The 2515 now has that state.
 */
TEST(Mcp2515ModeGate, AFailedSpeedChangeTakesTheInterfaceOutOfService) {
  const std::string body = body_after(comm_can_source(), "static void poll_can_addon_speed_change() {");
  ASSERT_FALSE(body.empty());

  const size_t failed = body.find("speedChangeFailed()");
  ASSERT_NE(failed, std::string::npos) << "the verdict is not polled at all";
  const size_t cleared = body.find("can2515_initialized = false", failed);
  EXPECT_NE(cleared, std::string::npos)
      << "a failed speed change reports an event but leaves the interface in service - a chip at an unknown bitrate "
         "keeps being polled and transmitted to, which the readback change said it was not closing";
}

/* ...and a gate you can only shut is a worse bug than no gate. The interface
 * has to be able to come back, which is what speedChangeSucceeded() is for.
 */
TEST(Mcp2515ModeGate, AndTheInterfaceCanComeBack) {
  const std::string body = body_after(comm_can_source(), "static void poll_can_addon_speed_change() {");
  ASSERT_FALSE(body.empty());

  const size_t ok = body.find("speedChangeSucceeded()");
  ASSERT_NE(ok, std::string::npos)
      << "nothing observes a speed change SUCCEEDING, so once the interface is gated off it can never be restored - "
         "the gate is a one-way door";
  EXPECT_NE(body.find("can2515_initialized = true", ok), std::string::npos)
      << "a successful speed change does not put the interface back in service";
}

/* The recovery poll must run even while the interface is gated OFF. If it sat
 * behind the gate, the flag that stops the polling would also stop the only
 * path the restoring verdict arrives on.
 */
TEST(Mcp2515ModeGate, TheVerdictIsPolledOutsideTheUsabilityGate) {
  const std::string body = body_after(comm_can_source(), "void receive_can() {");
  ASSERT_FALSE(body.empty());

  const size_t poll = body.find("poll_can_addon_speed_change()");
  ASSERT_NE(poll, std::string::npos) << "the speed-change verdict is never polled from receive_can()";
  const size_t gate = body.find("if (can2515_initialized)", poll);
  ASSERT_NE(gate, std::string::npos) << "the 2515 receive path is not gated on the interface being usable";
  EXPECT_LT(poll, gate) << "the verdict is polled INSIDE the usability gate, so a failed change stops the polling that "
                           "would ever undo it - the interface could never recover";
}

/* Transmitting onto a bus at an unknown bitrate is worse than staying quiet:
 * it is what an interface at the wrong speed does to everyone else on the wire.
 */
TEST(Mcp2515ModeGate, TransmitIsGatedOnUsabilityNotJustPresence) {
  const std::string src = comm_can_source();
  const size_t send = src.find("can2515->sendFrame(mcp2515_frame)");
  ASSERT_NE(send, std::string::npos) << "the 2515 transmit site has moved, the scan has drifted";

  const size_t line_start = src.rfind("if (", send);
  ASSERT_NE(line_start, std::string::npos);
  const std::string guard = src.substr(line_start, send - line_start);
  EXPECT_NE(guard.find("can2515_initialized"), std::string::npos)
      << "transmit checks only that the chip OBJECT exists, not that the interface is usable - after a failed speed "
         "change the firmware keeps transmitting at a bitrate nobody knows";
}

/* (b) The same fire-and-forget shape removed from the speed-change path
 * still sat in begin(). It matters more here: CNF1..3 are writable only in
 * CONFIG, so a chip that never got there takes none of the timing.
 */
TEST(Mcp2515ModeGate, BeginVerifiesBothModeChanges) {
  const std::string body = body_after(driver_source(), "bool MCP2515_Lite::begin(");
  ASSERT_FALSE(body.empty());

  EXPECT_EQ(body.find("modifyRegister(REG_CANCTRL"), std::string::npos)
      << "begin() still drives CANCTRL directly, which is the unverified mode request this item removed";
  EXPECT_NE(body.find("enterMode(CANCTRL_REQOP_CONFIG)"), std::string::npos)
      << "begin() enters CONFIG without confirming it - the timing registers are then written to a chip that is not "
         "in configuration mode, where they are not writable at all";
  EXPECT_NE(body.find("enterMode("), std::string::npos) << "begin() never confirms the running mode either";
}

/* A begin() that verifies and then returns true anyway has only moved the
 * silence. Every check has to be able to fail the init.
 */
TEST(Mcp2515ModeGate, BeginFailsInitWhenAStepDoesNotTake) {
  const std::string body = body_after(driver_source(), "bool MCP2515_Lite::begin(");
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("if (!enterMode(CANCTRL_REQOP_CONFIG)) {"), std::string::npos)
      << "a chip that will not enter CONFIG still reports a successful init";
  EXPECT_NE(body.find("if (!applySpeedConfig(speed)) {"), std::string::npos)
      << "an unreachable bitrate at BOOT writes no timing registers and still reports a successful init - the same "
         "hole closed for a runtime change";

  // The failure returns must come before the task is started: an init that
  // failed should not leave a driver task polling the chip.
  const size_t task = body.find("xTaskCreate");
  ASSERT_NE(task, std::string::npos) << "begin() no longer starts the task, the scan has drifted";
  EXPECT_LT(body.find("if (!enterMode("), task) << "the mode checks run after the task is started";
}

/* (c) begin()'s loopback argument was never stored, so the speed-change path
 * had nothing to return to but an assumed NORMAL - and a loopback session
 * became live on the bus at the first speed change, silently.
 */
TEST(Mcp2515ModeGate, ASpeedChangeReturnsToTheModeBeginStartedIn) {
  const std::string src = driver_source();
  const std::string begin_body = body_after(src, "bool MCP2515_Lite::begin(");
  ASSERT_FALSE(begin_body.empty());
  EXPECT_NE(begin_body.find("_loopback = loopback"), std::string::npos)
      << "begin() still drops its loopback argument, so nothing downstream can know which mode to restore";

  const std::string block = speed_change_block(src);
  ASSERT_FALSE(block.empty());
  EXPECT_NE(block.find("_loopback"), std::string::npos)
      << "the speed change does not consult the mode the driver was started in";
  EXPECT_EQ(block.find("enterMode(CANCTRL_REQOP_NORMAL)"), std::string::npos)
      << "the speed change hard-codes a return to NORMAL - a driver opened in LOOPBACK silently becomes live on the "
         "bus, transmitting onto a real wire from what the caller believes is a self-contained test";
}

/* The gate must not be reopened by a verdict that has been superseded.
 *
 * The outcome of a change is reported in two independent read-and-clear latches
 * and the poll consumes them in priority order - failure first. That is only
 * safe while at most one of them can be set. If a change that SUCCEEDED and a
 * later one that FAILED both land between two polls, the poll shuts the gate on
 * the failure and clears ONLY that latch; the next poll finds the success still
 * standing and puts the interface back in service at a bitrate nobody knows.
 * The window is one core-loop iteration, which is 1 ms nominal and much longer
 * whenever the loop is stalled - and the pair is a live pattern, not a
 * hypothetical: BMW-PHEV-BATTERY.cpp's bus wake drops to 100 kbit/s and
 * restores 500 kbit/s.
 *
 * So the exclusivity is pinned where it is created: each verdict retires the
 * other, which is the guarantee a single tri-state field would carry in its
 * type. The block comment above this code in the driver explains the same
 * property at length; until now nothing held it.
 */
TEST(Mcp2515ModeGate, AVerdictRetiresTheOneItSupersedes) {
  const std::string block = speed_change_block(driver_source());
  ASSERT_FALSE(block.empty());

  const size_t set_failed = block.find("_speed_change_failed = true");
  const size_t set_ok = block.find("_speed_change_succeeded = true");
  ASSERT_NE(set_failed, std::string::npos) << "nothing records a failure";
  ASSERT_NE(set_ok, std::string::npos) << "nothing records a success";

  EXPECT_NE(block.find("_speed_change_succeeded = false"), std::string::npos)
      << "recording a failure leaves an earlier SUCCESS latched - the caller shuts the gate on the failure and the "
         "next poll reopens it on the stale success, putting a chip at an unknown bitrate back on the bus";
  EXPECT_NE(block.find("_speed_change_failed = false"), std::string::npos)
      << "recording a success leaves an earlier FAILURE latched - the caller reads failure-first and keeps the "
         "interface out of service after the change that fixed it";
}
