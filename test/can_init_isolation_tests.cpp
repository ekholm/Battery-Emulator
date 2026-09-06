#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>

/* One CAN chip's init failure must stop at that chip.
 *
 * `init_CAN()` used to `return false` on any failure, which read as failing
 * loudly but could not: the return is discarded at its only call site, so the
 * abort silently skipped every interface declared after the one that failed.
 * Which interfaces survived was decided by initialisation order, and order is
 * not a safety property.
 *
 * comm_can.cpp is not part of this binary - it reaches for the ESP32 CAN
 * drivers - so these read the source. That is the same reason the defect lived
 * there unnoticed: the file has no host build to fail.
 */
namespace {

std::string comm_can_source() {
  // Located relative to this file rather than through a CMake define, so the
  // test needs no build-system plumbing to run.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/communication/can/comm_can.cpp";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "comm_can.cpp is where this test looks: " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

// The body of init_CAN(), by brace depth from its opening line.
std::string init_can_body(const std::string& src) {
  const size_t at = src.find("void init_CAN() {");
  EXPECT_NE(at, std::string::npos) << "init_CAN() is not declared void - the discarded bool is back";
  if (at == std::string::npos) {
    return "";
  }
  size_t i = src.find('{', at);
  int depth = 0;
  for (size_t j = i; j < src.size(); ++j) {
    if (src[j] == '{') {
      ++depth;
    } else if (src[j] == '}') {
      if (--depth == 0) {
        return src.substr(i, j - i + 1);
      }
    }
  }
  return "";
}

}  // namespace

TEST(CanInitIsolation, InitCanReturnsNothingBecauseNobodyReadIt) {
  const std::string src = comm_can_source();
  EXPECT_NE(src.find("void init_CAN() {"), std::string::npos)
      << "init_CAN() must not return a status again unless something examines it - the discarded bool "
         "is what made 'a failed chip stops the boot' look true";
  EXPECT_EQ(init_can_body(src).find("return false"), std::string::npos)
      << "init_CAN() still has a `return false` - every failure path is either an event or a "
         "documented whole-board abort";
}

/* The four chip-init failures. Each must leave its interface inert - null
 * pointer, or the native flag false - because null is what receive_can() and
 * both transmit paths already read as "not there".
 */
TEST(CanInitIsolation, AFailedChipIsLeftInertRatherThanAbortingTheRest) {
  const std::string body = init_can_body(comm_can_source());
  ASSERT_FALSE(body.empty());

  for (const char* marker : {"can2515 = nullptr;", "canfd = nullptr;", "canfd_2 = nullptr;",
                             "native_can_initialized = false;"}) {
    EXPECT_NE(body.find(marker), std::string::npos)
        << marker << " is gone - a chip that failed to start would be left looking usable";
  }
}

/* The native path was the worst of the abort sites: it raised no event at all,
 * only a log, and these boards log nothing unless USBENABLED is set - so the
 * one failure that took out every other interface was also the only silent one.
 */
TEST(CanInitIsolation, TheNativeFailureRaisesAnEventLikeEveryOtherChip) {
  const std::string body = init_can_body(comm_can_source());
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("EVENT_CAN_NATIVE_INIT_FAILURE"), std::string::npos)
      << "the native CAN init failure raises no event - it is invisible on a board without USBENABLED";
  for (const char* ev : {"EVENT_CANMCP2515_INIT_FAILURE", "EVENT_CAN_NATIVE_INIT_FAILURE"}) {
    EXPECT_NE(body.find(ev), std::string::npos) << ev << " is missing from init_CAN()";
  }
}

/* Pin allocation is the deliberate exception and must stay one: an incoherent
 * pin map is a fact about the whole board, not a fault in one chip, and
 * carrying on would hand the same pad to whichever interface asks next.
 */
TEST(CanInitIsolation, AnIncoherentPinMapStillStopsEverything) {
  const std::string body = init_can_body(comm_can_source());
  ASSERT_FALSE(body.empty());

  int alloc_sites = 0;
  int guarded_returns = 0;
  for (size_t at = body.find("alloc_pins("); at != std::string::npos; at = body.find("alloc_pins(", at + 1)) {
    ++alloc_sites;
    const size_t stop = body.find("return;", at);
    const size_t next_alloc = body.find("alloc_pins(", at + 1);
    if (stop != std::string::npos && (next_alloc == std::string::npos || stop < next_alloc)) {
      ++guarded_returns;
    }
  }
  EXPECT_GT(alloc_sites, 0) << "no alloc_pins() calls found in init_CAN() - the scan has drifted";
  // Every site, not a threshold: one dropped return is exactly the regression
  // this guards, and a count that allows slack would not see it.
  EXPECT_EQ(guarded_returns, alloc_sites)
      << guarded_returns << " of " << alloc_sites << " pin-allocation failures still stop initialisation; "
      << "if continuing past an incoherent pin map is now wanted, it is a decision to take deliberately, "
      << "not by deleting a return";
}

/* A FAILED runtime speed change must take the native interface out of
 * service, not just report itself.
 *
 * `change_can_speed()` re-runs init_native_can(). On failure it logged and
 * returned false while leaving `native_can_initialized` true - and that flag is
 * what receive_can() gates the native receive path on, so the firmware kept
 * polling an interface whose begin() had just failed. The boot path already
 * cleared the flag and raised the event; only the runtime path did neither.
 *
 * Source-read for the same reason as the cases above: comm_can.cpp has no host
 * build, which is why both halves of this class of defect lived there unseen.
 */
namespace {

// The body of change_can_speed(), by brace depth from its opening line.
/* The same brace-matched extraction, for receive_can(). Needed
 * because everything else in this file pins the WRITERS of
 * native_can_initialized and nothing pins its reader. */
std::string receive_can_body(const std::string& src) {
  const size_t at = src.find("void receive_can(");
  EXPECT_NE(at, std::string::npos) << "receive_can() is not where this test looks";
  if (at == std::string::npos) {
    return "";
  }
  size_t i = src.find('{', at);
  int depth = 0;
  const size_t start = i;
  for (; i < src.size(); ++i) {
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

std::string change_can_speed_body(const std::string& src) {
  const size_t at = src.find("bool change_can_speed(");
  EXPECT_NE(at, std::string::npos) << "change_can_speed() is not where this test looks";
  if (at == std::string::npos) {
    return "";
  }
  size_t i = src.find('{', at);
  int depth = 0;
  const size_t start = i;
  for (; i < src.size(); ++i) {
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

}  // namespace

/* Every other case here pins a WRITER of native_can_initialized -
 * that init_CAN() clears it on a failed boot, that change_can_speed() clears it
 * on a failed re-init. None of them pins the READER, and the flag is worth
 * exactly what its reader does with it: delete the guard in receive_can() and
 * the flag becomes decorative, a dead interface gets polled again, and every
 * assertion in this file still passes. That is the one blind spot the
 * source-reading form has here, so close it in the same form.
 */
TEST(CanInitIsolation, ReceiveCanStillGatesTheNativePathOnTheFlag) {
  const std::string body = receive_can_body(comm_can_source());
  ASSERT_FALSE(body.empty());

  const size_t gate = body.find("if (native_can_initialized)");
  EXPECT_NE(gate, std::string::npos)
      << "receive_can() no longer gates the native receive path on native_can_initialized, so clearing that flag "
         "no longer takes the interface out of service - which is the whole point of both fixes";

  const size_t call = body.find("receive_frame_can_native()");
  ASSERT_NE(call, std::string::npos) << "receive_can() does not call receive_frame_can_native() - the scan has drifted";
  EXPECT_LT(gate, call) << "the native receive happens before the flag is tested";
}

TEST(CanInitIsolation, AFailedRuntimeSpeedChangeTakesTheNativeInterfaceOutOfService) {
  const std::string body = change_can_speed_body(comm_can_source());
  ASSERT_FALSE(body.empty());

  // The failure branch must clear the flag receive_can() gates on. Asserting the
  // assignment exists is weaker than running it, but comm_can.cpp cannot be run
  // here at all - and the defect was precisely that this line was absent.
  EXPECT_NE(body.find("native_can_initialized = false"), std::string::npos)
      << "change_can_speed() can fail init_native_can() without taking the interface out of service - "
         "receive_can() will keep polling a dead interface";

  // And it must say so through the same event the boot path raises, or the
  // runtime failure stays as silent as the boot one used to be.
  EXPECT_NE(body.find("EVENT_CAN_NATIVE_INIT_FAILURE"), std::string::npos)
      << "a failed runtime speed change is reported only by a return value, and nothing acts on it";

  /* The clear must be INSIDE the failure branch, and the success path must set
   * the flag true. Checking only "the clear comes after errorCode != 0" is not
   * enough - the success path is also after it, so a version that cleared the
   * flag on SUCCESS would pass, and that is worse than the original defect: it
   * would take the interface out of service after a speed change that WORKED.
   * Anchoring on the failure branch's own `return false` separates the two.
   */
  const size_t fail_at = body.find("errorCode != 0");
  ASSERT_NE(fail_at, std::string::npos);
  const size_t fail_return = body.find("return false", fail_at);
  ASSERT_NE(fail_return, std::string::npos);

  const size_t clear_at = body.find("native_can_initialized = false");
  ASSERT_NE(clear_at, std::string::npos);
  EXPECT_GT(clear_at, fail_at) << "the flag is cleared before the failure is known";
  EXPECT_LT(clear_at, fail_return) << "the flag is cleared outside the failure branch - on the SUCCESS path it would "
                                      "disable an interface whose speed change succeeded";

  const size_t set_true_at = body.find("native_can_initialized = true");
  ASSERT_NE(set_true_at, std::string::npos) << "the success path does not mark the interface in service";
  EXPECT_GT(set_true_at, fail_return) << "the success path's flag set is inside the failure branch";
}
