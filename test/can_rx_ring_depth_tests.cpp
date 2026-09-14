#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "emul/can_drivers.h"
#include "utils/source_scan.h"

#include "../Software/src/communication/can/CanReceiver.h"
#include "../Software/src/communication/can/can_rx_ring_depth.h"
#include "../Software/src/communication/can/comm_can.h"
#include "../Software/src/lib/mcp2515_lite/mcp2515_rx_ring.h"

/* The receive rings are deep enough to carry a bus through a flash write.
 *
 * Every driver shipped a 32-frame ring (the MCP2515 drain 64), which covers
 * 16 ms at 2,000 frames/s, and the bench measured OTA stalls of 85-94 ms on
 * the S3 boards. Three things have to hold for the deeper rings to help, and
 * each is pinned here:
 *
 *   - the depth covers the stall it was sized for, at the rate it was sized for;
 *   - comm_can.cpp actually hands that depth to every driver it begins,
 *     including after a speed change builds a new settings object;
 *   - the two rings an IRAM interrupt writes are placed in internal DRAM. A
 *     deeper ring is a bigger block, and on an ESP32-S3 with PSRAM a big enough
 *     plain allocation goes to PSRAM, which the interrupt cannot reach during the
 *     very flash window the ring exists for.
 *
 * The first two are run against the emulated drivers. The placement lives in
 * ESP-IDF heap calls that the host build does not have, so it is read from the
 * source, anchored on the arguments that decide it.
 */

using emul_can::Chip;

namespace {

class RecordingReceiver : public CanReceiver {
 public:
  void receive_can_frame(CAN_frame* rx_frame) override { received.push_back(*rx_frame); }
  std::vector<CAN_frame> received;
};

std::string source(const char* relative) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../" + relative;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

// Frames that land in `stall_ms` at `rate_fps`, rounded up.
uint32_t frames_in(uint32_t stall_ms, uint32_t rate_fps) {
  return (stall_ms * rate_fps + 999) / 1000;
}

}  // namespace

TEST(CanRxRingDepth, TheDriverRingsHoldEveryFrameOfTheStallTheyWereSizedFor) {
  const uint32_t need = frames_in(CAN_RX_RING_SIZED_STALL_MS, CAN_RX_RING_SIZED_RATE_FPS);
  EXPECT_EQ(need, 188u) << "94 ms at 2,000 frames/s";

  EXPECT_GE(CAN_DRIVER_RX_RING_DEPTH, need)
      << "the TWAI and CAN-FD rings no longer outlast an OTA stall at the rate they were sized for";
  EXPECT_GE(MCP2515_LITE_ISR_RING_DEPTH, need)
      << "the MCP2515 interrupt ring no longer outlasts an OTA stall at the rate the other rings are sized for";
}

TEST(CanRxRingDepth, TheNativeControllerIsBegunWithTheDepth) {
  ASSERT_TRUE(emul_can::is_running(Chip::Native));
  EXPECT_EQ(emul_can::rx_ring_depth(Chip::Native), CAN_DRIVER_RX_RING_DEPTH)
      << "init_native_can() begins the TWAI driver with its library default ring";
}

TEST(CanRxRingDepth, BothFdChipsAreBegunWithTheDepth) {
  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2518fd));
  EXPECT_EQ(emul_can::rx_ring_depth(Chip::Mcp2518fd), CAN_DRIVER_RX_RING_DEPTH)
      << "the first MCP2518FD is begun with its library default ring";

  ASSERT_TRUE(emul_can::is_running(Chip::Mcp2518fd2));
  EXPECT_EQ(emul_can::rx_ring_depth(Chip::Mcp2518fd2), CAN_DRIVER_RX_RING_DEPTH)
      << "the second MCP2518FD is begun with its library default ring";
}

// init_native_can() deletes the settings object and builds a new one on every
// speed change, so a depth set once at boot would be lost at the first battery
// that asks for another bitrate.
TEST(CanRxRingDepth, ANativeSpeedChangeKeepsTheDepth) {
  ASSERT_TRUE(change_can_speed(CAN_NATIVE, CAN_Speed::CAN_SPEED_250KBPS));

  EXPECT_EQ(emul_can::rx_ring_depth(Chip::Native), CAN_DRIVER_RX_RING_DEPTH)
      << "the speed change re-began the TWAI driver with its library default ring";
}

// The native drain takes at most one ring's worth per call. A full ring after
// a stall must come out in one pass, or the backlog carries into the next.
TEST(CanRxRingDepth, OneNativeDrainPassEmptiesAFullRing) {
  emul_can_tear_down_all_interfaces();
  emul_can::reset();
  RecordingReceiver receiver;
  register_can_receiver(&receiver, CAN_NATIVE);
  emul_can_init_on_full_board();
  ASSERT_TRUE(emul_can::is_running(Chip::Native));

  for (uint32_t i = 0; i < CAN_DRIVER_RX_RING_DEPTH; i++) {
    CAN_frame frame = {};
    frame.ID = 0x100 + i;
    frame.DLC = 1;
    emul_can::queue_received(Chip::Native, frame);
  }

  receive_can();

  EXPECT_EQ(receiver.received.size(), CAN_DRIVER_RX_RING_DEPTH)
      << "a ring filled during a stall took more than one drain pass to empty";
}

TEST(CanRxRingDepth, TheTwaiRingIsAllocatedInInternalDram) {
  const std::string buffer = source("Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32_Buffer16.h");

  const std::string allocate = required_function_body(buffer, "static CANMessage * allocate (const uint16_t inSize)");
  EXPECT_NE(allocate.find("heap_caps_malloc (sizeof (CANMessage) * inSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)"),
            std::string::npos)
      << "the TWAI ring is no longer asked for in internal DRAM - on an S3 with PSRAM the heap may place it where "
         "the interrupt faults in a flash window";

  const std::string init = required_function_body(buffer, "bool initWithSize (const uint16_t inSize)");
  EXPECT_NE(init.find("mBuffer = allocate (inSize)"), std::string::npos)
      << "initWithSize() no longer takes its storage from the internal allocator";
  EXPECT_EQ(buffer.find("new CANMessage ["), std::string::npos)
      << "a plain new[] is back, which leaves the ring's placement to its size";
}

TEST(CanRxRingDepth, TheMcp2515DriverObjectIsAllocatedInInternalDram) {
  const std::string driver = source("Software/src/lib/mcp2515_lite/mcp2515_lite.cpp");

  const std::string op_new = required_function_body(driver, "void* MCP2515_Lite::operator new(size_t size)");
  EXPECT_NE(op_new.find("heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)"), std::string::npos)
      << "MCP2515_Lite is no longer allocated in internal DRAM - its interrupt ring is a member, and a 4 KB object "
         "goes to PSRAM on an S3 that has it";

  const std::string header = source("Software/src/lib/mcp2515_lite/mcp2515_lite.h");
  EXPECT_NE(header.find("static void* operator new(size_t size);"), std::string::npos)
      << "the class no longer declares its own operator new, so `new MCP2515_Lite` uses the global one";
}

TEST(CanRxRingDepth, TheMcp2515InterruptIsNotAttachedToARingOutsideInternalDram) {
  const std::string driver = source("Software/src/lib/mcp2515_lite/mcp2515_lite.cpp");

  const size_t check = driver.find("esp_ptr_internal(&_isr_ring)");
  ASSERT_NE(check, std::string::npos) << "the boot check on where the interrupt ring lives is gone";
  const size_t attach = driver.find("attachInterruptArg(digitalPinToInterrupt(_int_pin)");
  ASSERT_NE(attach, std::string::npos);
  EXPECT_LT(check, attach) << "the interrupt is attached before the ring's placement is checked";

  // Present and in front is not enough: `||` would still read both calls and
  // attach whenever the handler alone is resident. Both must hold.
  EXPECT_TRUE(std::regex_search(
      driver, std::regex(R"(esp_ptr_in_iram\([^;{]*\)\s*&&\s*esp_ptr_internal\(&_isr_ring\)\)\s*\{)")))
      << "the interrupt is no longer attached only when the handler AND its ring are IRAM-safe";
}
