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
