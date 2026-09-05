#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <string>

#include "../Software/src/communication/can/canfd_init_error.h"
#include "../Software/src/devboard/utils/events.h"

/* EVENT_CANMCP2518FD_INIT_FAILURE exists to name WHICH of ACAN2517FD::begin()'s
 * twenty-one error codes was returned, and it could not: the raise cast a
 * 21-bit one-hot mask to uint8_t, so every code from bit 8 up - thirteen of the
 * twenty-one, kRequestedModeTimeOut among them - reached the event log as data
 * 0, indistinguishable from an event that carried no detail at all.
 *
 * Three halves below.
 *
 * The first exercises canfd_init_error_index(), which is where the conversion
 * now lives precisely so it CAN be exercised: comm_can.cpp is not in this
 * binary and cannot be (test/CMakeLists.txt's curated firmware list), so a
 * conversion written inline at the call site would be untestable by
 * construction.
 *
 * The second pins the PREMISE the conversion rests on by reading ACAN2517FD.h:
 * twenty-one codes, one-hot, bits 0 through 20. If a library bump widens that
 * block past bit 30 the index no longer characterises the mask, and nothing
 * else in this tree would notice.
 *
 * The third reads comm_can.cpp as text, for the same reason ota_confirm_tests
 * reads Software.cpp: the call sites are not in this binary, so a regression
 * that reinstates the cast would compile, ship, and pass every test here.
 */

namespace {

// Comments are stripped before anything is asserted about source text: the
// header this change adds NAMES the uint8_t cast it removes, in prose, and a
// matcher that reads comments would convict on the explanation.
std::string canfd_strip_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src.compare(i, 2, "//") == 0) {
      while (i < src.size() && src[i] != '\n') {
        ++i;
      }
    } else if (src.compare(i, 2, "/*") == 0) {
      const size_t end = src.find("*/", i + 2);
      const size_t stop = end == std::string::npos ? src.size() : end + 2;
      for (; i < stop; ++i) {
        if (src[i] == '\n') {
          out += '\n';
        }
      }
    } else {
      out += src[i++];
    }
  }
  return out;
}

std::string canfd_read_source(const std::string& relative_to_test_dir) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/" + relative_to_test_dir;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return canfd_strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

// The body of a function, by brace depth from its signature line. Matched with
// the opening brace attached, because both functions read here are FORWARD
// DECLARED above their definitions - matching the bare signature lands on the
// declaration and then walks into whatever function follows it.
std::string canfd_function_body(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature + " {");
  EXPECT_NE(at, std::string::npos) << "no definition of `" << signature << "` in the source this test reads";
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = src.find('{', at);
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

constexpr int kLowestCodeBit = 0;
constexpr int kHighestCodeBit = 20;
constexpr int kNumberOfCodes = kHighestCodeBit - kLowestCodeBit + 1;

}  // namespace

// ---------------------------------------------------------------------------
// The conversion
// ---------------------------------------------------------------------------

TEST(CanFdInitErrorIndex, EveryCodeGetsItsOwnNonZeroValue) {
  std::set<int16_t> seen;
  for (int bit = kLowestCodeBit; bit <= kHighestCodeBit; bit++) {
    const int16_t index = canfd_init_error_index(static_cast<uint32_t>(1) << bit);
    EXPECT_NE(index, 0) << "bit " << bit << " reports as 0, which is the value that means `no detail`";
    EXPECT_TRUE(seen.insert(index).second) << "bit " << bit << " collides with another code";
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kNumberOfCodes));
}

TEST(CanFdInitErrorIndex, TheThirteenCodesTheCastDestroyedSurvive) {
  // This is the defect, stated as a test. (uint8_t) keeps bits 0-7, so bits 8
  // and up all came out 0; every one of them must now be distinct and non-zero.
  for (int bit = 8; bit <= kHighestCodeBit; bit++) {
    const uint32_t code = static_cast<uint32_t>(1) << bit;
    EXPECT_EQ(static_cast<uint8_t>(code), 0) << "bit " << bit << " is not one the old cast lost";
    EXPECT_EQ(canfd_init_error_index(code), static_cast<int16_t>(bit + 1));
  }
}

TEST(CanFdInitErrorIndex, TheModeTimeoutIsReportedExactly) {
  // kRequestedModeTimeOut, 1 << 16: the only code begin() can return with the
  // chip's interrupt already attached, and the one that used to read as 0.
  EXPECT_EQ(canfd_init_error_index(static_cast<uint32_t>(1) << 16), 17);
}

TEST(CanFdInitErrorIndex, ZeroStaysZero) {
  // 0 means "begin() succeeded" at the call site and "no detail" in the log.
  // One-basing the index is what keeps those two from colliding with bit 0.
  EXPECT_EQ(canfd_init_error_index(0), 0);
  EXPECT_EQ(canfd_init_error_index(static_cast<uint32_t>(1) << 0), 1);
}

TEST(CanFdInitErrorIndex, AnAccumulatedMaskReportsItsLowestCode) {
  // begin() ORs the settings-validation codes together. The lowest is reported;
  // this test states that choice rather than leaving it to be discovered.
  const uint32_t settings_faults = (1u << 4) | (1u << 5) | (1u << 19);
  EXPECT_EQ(canfd_init_error_index(settings_faults), 5);
  EXPECT_EQ(canfd_init_error_index((1u << 12) | (1u << 16)), 13);
}

TEST(CanFdInitErrorIndex, EveryValueFitsTheEventDataSlot) {
  // set_event() takes int16_t. An index that did not fit would be truncated
  // again, one layer further down, which is the bug this replaces.
  for (int bit = kLowestCodeBit; bit <= kHighestCodeBit; bit++) {
    const int16_t index = canfd_init_error_index(static_cast<uint32_t>(1) << bit);
    EXPECT_GT(index, 0);
    EXPECT_LE(index, kNumberOfCodes);
  }
}

TEST(CanFdInitErrorIndex, TheEventHubCarriesTheIndexIntact) {
  // The half of the pair that IS linked here: events.cpp stores and returns the
  // value unchanged, so what the call site computes is what a reader sees.
  init_events();
  for (int bit = kLowestCodeBit; bit <= kHighestCodeBit; bit++) {
    const int16_t index = canfd_init_error_index(static_cast<uint32_t>(1) << bit);
    set_event(EVENT_CANMCP2518FD_INIT_FAILURE, index);
    EXPECT_EQ(get_event_pointer(EVENT_CANMCP2518FD_INIT_FAILURE)->data, index) << "bit " << bit;
  }
}

// ---------------------------------------------------------------------------
// The premise, read from the library
// ---------------------------------------------------------------------------

TEST(CanFdInitErrorPremise, TheLibraryDeclaresTwentyOneOneHotCodes) {
  const std::string src = canfd_read_source("../Software/src/lib/pierremolinaro-ACAN2517FD/ACAN2517FD.h");
  std::set<int> bits;
  size_t at = 0;
  const std::string marker = "static const uint32_t k";
  while ((at = src.find(marker, at)) != std::string::npos) {
    const size_t shift = src.find("<<", at);
    const size_t line_end = src.find('\n', at);
    ASSERT_NE(shift, std::string::npos);
    ASSERT_LT(shift, line_end) << "a begin() code that is not a one-hot shift";
    bits.insert(std::stoi(src.substr(shift + 2, line_end - shift - 2)));
    at = line_end;
  }
  EXPECT_EQ(bits.size(), static_cast<size_t>(kNumberOfCodes));
  EXPECT_EQ(*bits.begin(), kLowestCodeBit);
  EXPECT_EQ(*bits.rbegin(), kHighestCodeBit);
}

// ---------------------------------------------------------------------------
// The call sites, read from comm_can.cpp
// ---------------------------------------------------------------------------

TEST(CanFdInitErrorPlacement, BothFailurePathsConvertTheCode) {
  const std::string src = canfd_read_source("../Software/src/communication/can/comm_can.cpp");

  const std::string first = canfd_function_body(src, "static bool begin_canfd()");
  EXPECT_NE(first.find("set_event(EVENT_CANMCP2518FD_INIT_FAILURE, canfd_init_error_index(errorCode2517))"),
            std::string::npos)
      << "begin_canfd() does not raise the event through the conversion";

  const std::string second = canfd_function_body(src, "static bool begin_canfd_2()");
  EXPECT_NE(second.find("set_event(EVENT_CANMCP2518FD_INIT_FAILURE, canfd_init_error_index(errorCode2517_2))"),
            std::string::npos)
      << "begin_canfd_2() does not raise the event through the conversion";
}

TEST(CanFdInitErrorPlacement, EachPathNamesItsOwnErrorCode) {
  // begin_canfd_2() is begin_canfd() with a suffix applied throughout, so a
  // missed suffix reads correct and reports the sibling chip's code.
  const std::string src = canfd_read_source("../Software/src/communication/can/comm_can.cpp");

  const std::string first = canfd_function_body(src, "static bool begin_canfd()");
  EXPECT_EQ(first.find("errorCode2517_2"), std::string::npos) << "begin_canfd() reads the second chip's code";

  const std::string second = canfd_function_body(src, "static bool begin_canfd_2()");
  EXPECT_NE(second.find("canfd_init_error_index(errorCode2517_2)"), std::string::npos);
}

TEST(CanFdInitErrorPlacement, NoTruncatingCastReachesTheEvent) {
  // The defect itself, pinned as text. Reinstating (uint8_t) - or any narrowing
  // cast - on this raise compiles cleanly and silently restores the data-0 bug.
  const std::string src = canfd_read_source("../Software/src/communication/can/comm_can.cpp");
  size_t at = 0;
  int raises = 0;
  const std::string marker = "set_event(EVENT_CANMCP2518FD_INIT_FAILURE,";
  while ((at = src.find(marker, at)) != std::string::npos) {
    const size_t end = src.find(';', at);
    const std::string call = src.substr(at, end - at);
    EXPECT_EQ(call.find("(uint8_t)"), std::string::npos) << "a narrowing cast is back on this raise: " << call;
    EXPECT_EQ(call.find("(int8_t)"), std::string::npos) << "a narrowing cast is back on this raise: " << call;
    EXPECT_NE(call.find("canfd_init_error_index("), std::string::npos) << "raise bypasses the conversion: " << call;
    raises++;
    at = end;
  }
  EXPECT_EQ(raises, 2) << "this event is raised somewhere this test does not check";
}

TEST(CanFdInitErrorPlacement, TheSerialLogStillCarriesTheFullMask) {
  // The index is lossy on an accumulated mask by design; that is only
  // acceptable while the untruncated value is still printed beside it.
  const std::string src = canfd_read_source("../Software/src/communication/can/comm_can.cpp");
  for (const std::string& fn : {std::string("static bool begin_canfd()"), std::string("static bool begin_canfd_2()")}) {
    const std::string body = canfd_function_body(src, fn);
    const size_t printed = body.find("logging.println(errorCode2517");
    const size_t raised = body.find("set_event(EVENT_CANMCP2518FD_INIT_FAILURE");
    EXPECT_NE(printed, std::string::npos) << fn << " no longer logs the full error mask";
    EXPECT_LT(printed, raised) << fn << " logs the mask after raising the event";
    EXPECT_NE(body.find(", HEX)"), std::string::npos) << fn << " no longer logs the mask in hex";
  }
}
