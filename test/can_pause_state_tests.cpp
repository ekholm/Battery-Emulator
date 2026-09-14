#include <gtest/gtest.h>

#include <fstream>
#include <string>

/* native_can_initialized has to keep meaning "the TWAI peripheral is up" across a pause.
 * It did not: stop_can() disabled the module and left the flag set, and restart_can()
 * discarded begin()'s error code, so the flag was wrong in one direction after every pause and
 * in the other direction on any board whose boot-time init had failed.
 *
 * Why the stale-TRUE half is a crash and not a tidiness problem: the native transmit path
 * refuses on this flag, and allowed_to_send_CAN does not cover the gap. CAN replay
 * transmits from its own FreeRTOS task ("CAN_Replay", webserver.cpp) while core_loop's 1 s
 * sub-task runs the pause edge, and update_pause_state() necessarily sets allowed_to_send_CAN
 * true BEFORE restart_can() brings the peripheral back. A frame replayed in that window used to
 * reach tryToSend() on a clock-gated module, which faults with
 * interrupts off: a double exception the watchdog reboots straight back into.
 *
 * comm_can.cpp is not part of this binary - it reaches for the ESP32 CAN drivers - so these
 * read the source, the same way the other tests over this file do. That is also why these defects
 * lived there unnoticed: the file has no host build to fail.
 */
namespace {

std::string source_of(const std::string& relative_path) {
  // Located relative to this file rather than through a CMake define, so the test needs no
  // build-system plumbing to run.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../" + relative_path;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << relative_path << " is where this test looks: " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

std::string comm_can_source() {
  return source_of("Software/src/communication/can/comm_can.cpp");
}

// The brace-balanced block that opens at the first '{' at or after 'from'.
std::string brace_block(const std::string& src, size_t from) {
  const size_t open = src.find('{', from);
  if (open == std::string::npos) {
    return "";
  }
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') {
      ++depth;
    } else if (src[i] == '}') {
      if (--depth == 0) {
        return src.substr(open, i - open + 1);
      }
    }
  }
  return "";
}

std::string function_body(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature);
  EXPECT_NE(at, std::string::npos) << signature << " is not where this test looks";
  if (at == std::string::npos) {
    return "";
  }
  return brace_block(src, at);
}

// Comments carry the words these tests match on, so a rationale block would satisfy an
// assertion no code satisfies. Strip them first.
std::string without_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src.compare(i, 2, "/*") == 0) {
      const size_t end = src.find("*/", i + 2);
      i = (end == std::string::npos) ? src.size() : end + 2;
    } else if (src.compare(i, 2, "//") == 0) {
      const size_t end = src.find('\n', i);
      i = (end == std::string::npos) ? src.size() : end;
    } else {
      out += src[i++];
    }
  }
  return out;
}

}  // namespace

TEST(CanPauseStateSource, StoppingCanTakesTheFlagDownWithThePeripheral) {
  const std::string body = without_comments(function_body(comm_can_source(), "void stop_can()"));
  const size_t end_call = body.find("ACAN_ESP32::can.end");
  ASSERT_NE(end_call, std::string::npos) << "stop_can() no longer calls end() - re-check this test";
  EXPECT_NE(body.find("native_can_initialized = false", end_call), std::string::npos)
      << "stop_can() must clear native_can_initialized after end(): end() calls "
         "periph_module_disable(PERIPH_TWAI_MODULE), and the native transmit path decides "
         "whether to touch TWAI registers by reading that flag";
}

TEST(CanPauseStateSource, RestartingCanSetsTheFlagFromTheResultRatherThanDiscardingIt) {
  const std::string body = without_comments(function_body(comm_can_source(), "void restart_can()"));
  EXPECT_NE(body.find("ACAN_ESP32::can.begin"), std::string::npos)
      << "restart_can() no longer calls begin() - re-check this test";
  EXPECT_NE(body.find("= ACAN_ESP32::can.begin("), std::string::npos)
      << "restart_can() must bind begin()'s error code rather than discarding it - a failed "
         "resume that says nothing is how a working interface stays refused in both directions";
  EXPECT_NE(body.find("native_can_initialized ="), std::string::npos)
      << "restart_can() must write the flag it is responsible for restoring";
}

TEST(CanPauseStateSource, AFailedRestartIsReportedTheWayEveryOtherFailedStartIs) {
  const std::string body = without_comments(function_body(comm_can_source(), "void restart_can()"));
  EXPECT_NE(body.find("EVENT_CAN_NATIVE_INIT_FAILURE"), std::string::npos)
      << "restart_can() must raise the same event init_CAN() and change_can_speed() raise: "
         "these boards log nothing unless USBENABLED is set, so an event is the only channel "
         "that tells a user why the interface is dead";
}

/* The three writers of this flag have to agree, or the reader cannot mean anything. init_CAN()
 * and change_can_speed() already did; this pins that stop_can()/restart_can() joined them
 * rather than the pair being fixed in isolation.
 */
TEST(CanPauseStateSource, EveryNativeStartOrStopMaintainsTheFlag) {
  const std::string src = comm_can_source();
  for (const std::string& fn : {std::string("void init_CAN()"), std::string("void stop_can()"),
                                std::string("void restart_can()"), std::string("bool change_can_speed(")}) {
    const std::string body = without_comments(function_body(src, fn));
    EXPECT_NE(body.find("native_can_initialized ="), std::string::npos)
        << fn
        << " starts or stops the native interface without saying so in the flag that "
           "decides whether the rest of the firmware uses it";
  }
}

/* The pause ordering the two halves rest on. stop_can() may only run once sending is already
 * refused; if the assignment moved below the edge check, pausing would open the same window
 * resuming has, and nothing else would notice.
 */
TEST(CanPauseStateSource, SendingIsForbiddenBeforeThePeripheralIsStopped) {
  const std::string body = without_comments(
      function_body(source_of("Software/src/devboard/safety/safety.cpp"), "void update_pause_state()"));
  const size_t assignment = body.find("allowed_to_send_CAN = (");
  const size_t stop = body.find("stop_can()");
  ASSERT_NE(assignment, std::string::npos) << "update_pause_state() no longer computes allowed_to_send_CAN here";
  ASSERT_NE(stop, std::string::npos) << "update_pause_state() no longer stops CAN on the pause edge";
  EXPECT_LT(assignment, stop) << "allowed_to_send_CAN must be false BEFORE stop_can() disables the peripheral - "
                                 "transmit_can_frame_to_interface() returns on that flag first, and CAN replay "
                                 "transmits from its own task";
}
