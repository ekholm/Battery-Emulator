#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>

#include "utils/source_scan.h"

/* Every CAN interface must drain a BATCH of received frames per call, not one.
 *
 * receive_can() runs once per iteration of the 1 kHz core loop, so a path that
 * takes a single frame per call caps that interface at ~1000 frames/second
 * regardless of the bus. The native path did exactly that and was measured on
 * silicon losing 74.7 % of a 3956 f/s stream on an idle board, while the three
 * add-on paths beside it had always drained a batch.
 *
 * This reads comm_can.cpp as source. Not because it is absent - it is compiled
 * into this binary, with emulated chips under the vendored drivers - but because
 * the property is the SHAPE common to all four paths rather than any one of
 * them: the defect was one interface being unlike its siblings, and a
 * behavioural case only ever drives the interfaces its board arrangement has.
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

}  // namespace

TEST(CanNativeDrainSource, TheNativePathDrainsAWholeBatchPerCall) {
  const std::string body = required_function_body(comm_can_source(), "receive_frame_can_native()");
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("while ("), std::string::npos)
      << "receive_frame_can_native() takes frames with an `if` again, which caps the native "
         "interface at one frame per core-loop iteration - about 1000 f/s, against the ~3956 f/s a "
         "500 kbit bus carries. Measured: 74.7 % of the stream lost on an idle board.";
  EXPECT_EQ(body.find("if (ACAN_ESP32::can.available())"), std::string::npos) << "the single-frame guard is back";
}

TEST(CanNativeDrainSource, TheNativeBoundIsAskedOfTheRingRatherThanCopied) {
  const std::string body = required_function_body(comm_can_source(), "receive_frame_can_native()");
  ASSERT_FALSE(body.empty());

  EXPECT_NE(body.find("driverReceiveBufferSize()"), std::string::npos)
      << "the drain bound is no longer taken from the driver's own ring depth. Any bound BELOW the "
         "ring can leave frames behind on every iteration, because the ring is the most the ISR can "
         "have queued since the last call - a smaller constant is the same defect, milder.";
}

TEST(CanNativeDrainSource, NoInterfaceIsLeftDrainingOneFrameAtATime) {
  const std::string src = comm_can_source();
  for (const char* fn : {"receive_frame_can_native()", "receive_frame_can_addon()", "_receive_frame_canfd("}) {
    const std::string body = required_function_body(src, fn);
    ASSERT_FALSE(body.empty()) << fn;
    EXPECT_NE(body.find("while ("), std::string::npos)
        << fn
        << " drains one frame per call while its siblings drain a batch. That asymmetry, with "
           "nothing in the file explaining it, is what this fix was for.";
  }
}
