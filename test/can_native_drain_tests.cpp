#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>

/* Every CAN interface must drain a BATCH of received frames per call, not one.
 *
 * receive_can() runs once per iteration of the 1 kHz core loop, so a path that
 * takes a single frame per call caps that interface at ~1000 frames/second
 * regardless of the bus. The native path did exactly that and was measured on
 * silicon losing 74.7 % of a 3956 f/s stream on an idle board, while the three
 * add-on paths beside it had always drained a batch.
 *
 * comm_can.cpp is not part of this binary - it reaches for the ESP32 CAN drivers
 * - so this reads the source, which is why it is deliberately about the SHAPE
 * common to all four paths rather than about any one of them: the defect was one
 * interface being unlike its siblings, and that is a property a reader can see
 * and a text scan can check.
 */
namespace {

std::string comm_can_source() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/communication/can/comm_can.cpp";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "comm_can.cpp is where this test looks: " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

// The definition's body by brace depth. Skips a forward declaration of the same
// signature: comm_can.cpp declares its statics at the top, and a plain find()
// would land on the declaration and read whatever function follows it.
std::string function_body(const std::string& src, const std::string& signature) {
  for (size_t at = src.find(signature); at != std::string::npos; at = src.find(signature, at + 1)) {
    const size_t brace = src.find('{', at);
    const size_t semicolon = src.find(';', at);
    if (brace == std::string::npos || semicolon < brace) {
      continue;
    }
    int depth = 0;
    for (size_t i = brace; i < src.size(); ++i) {
      if (src[i] == '{') {
        ++depth;
      } else if (src[i] == '}' && --depth == 0) {
        return src.substr(brace, i - brace + 1);
      }
    }
  }
  ADD_FAILURE() << signature << " has no definition where this test looks";
  return "";
}

}  // namespace

TEST(CanNativeDrainSource, TheNativePathDrainsAWholeBatchPerCall) {
  const std::string body = function_body(comm_can_source(), "receive_frame_can_native()");
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("while ("), std::string::npos)
      << "receive_frame_can_native() takes frames with an `if` again, which caps the native "
         "interface at one frame per core-loop iteration - about 1000 f/s, against the ~3956 f/s a "
         "500 kbit bus carries. Measured: 74.7 % of the stream lost on an idle board.";
  EXPECT_EQ(body.find("if (ACAN_ESP32::can.available())"), std::string::npos) << "the single-frame guard is back";
}

TEST(CanNativeDrainSource, TheNativeBoundIsAskedOfTheRingRatherThanCopied) {
  const std::string body = function_body(comm_can_source(), "receive_frame_can_native()");
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("driverReceiveBufferSize()"), std::string::npos)
      << "the drain bound is no longer taken from the driver's own ring depth. Any bound BELOW the "
         "ring can leave frames behind on every iteration, because the ring is the most the ISR can "
         "have queued since the last call - a smaller constant is the same defect, milder.";
}

TEST(CanNativeDrainSource, NoInterfaceIsLeftDrainingOneFrameAtATime) {
  const std::string src = comm_can_source();
  for (const char* fn : {"receive_frame_can_native()", "receive_frame_can_addon()", "_receive_frame_canfd("}) {
    const std::string body = function_body(src, fn);
    ASSERT_FALSE(body.empty()) << fn;
    EXPECT_NE(body.find("while ("), std::string::npos)
        << fn
        << " drains one frame per call while its siblings drain a batch. That asymmetry, with "
           "nothing in the file explaining it, is what this fix was for.";
  }
}
