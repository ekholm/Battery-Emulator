#include <gtest/gtest.h>

// The emulated CANMessage first: the real ACAN_ESP32_CANMessage.h sits behind
// the same include guard and has the same 16-byte layout, so the vendored
// buffer below compiles against the one definition every other test uses.
#include "src/lib/pierremolinaro-ACAN2517FD/CANMessage.h"

#include <Arduino.h>  // IRAM_ATTR, which the vendored buffer marks its ISR half with

#include "esp_heap_caps.h"

#include "../Software/src/communication/can/can_rx_ring_depth.h"
#include "../Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32_Buffer16.h"

/* The TWAI driver's receive ring, run for real rather than read as text.
 *
 * can_rx_ring_depth_tests.cpp holds the placement to the source. These cases
 * hold the behaviour the placement change brought with it: the storage now
 * comes from heap_caps_malloc instead of new[], so a failed allocation no
 * longer aborts but returns, and every path that used to rely on new[] -
 * a zero-size ring, a re-init, a failure - is its own code now.
 */

namespace {

CANMessage frame(uint32_t id) {
  CANMessage m;
  m.id = id;
  m.len = 1;
  m.data[0] = static_cast<uint8_t>(id);
  return m;
}

class Buffer16 : public ::testing::Test {
 protected:
  void SetUp() override { emul_heap::reset(); }
};

}  // namespace

TEST_F(Buffer16, TheRingIsAskedForInInternalByteAddressableMemory) {
  ACAN_ESP32_Buffer16 ring;
  ASSERT_TRUE(ring.initWithSize(CAN_DRIVER_RX_RING_DEPTH));

  EXPECT_EQ(emul_heap::last_caps, static_cast<uint32_t>(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))
      << "the ring is no longer placed in internal DRAM, which the TWAI interrupt needs in a flash window";
  EXPECT_EQ(emul_heap::last_size, sizeof(CANMessage) * CAN_DRIVER_RX_RING_DEPTH);
}

TEST_F(Buffer16, ADeepRingHoldsItsDepthInOrderAndReportsOverflowPastIt) {
  ACAN_ESP32_Buffer16 ring;
  ASSERT_TRUE(ring.initWithSize(CAN_DRIVER_RX_RING_DEPTH));
  EXPECT_EQ(ring.size(), CAN_DRIVER_RX_RING_DEPTH);

  for (uint32_t i = 0; i < CAN_DRIVER_RX_RING_DEPTH; i++) {
    ASSERT_TRUE(ring.append(frame(i))) << "frame " << i << " did not fit a ring of " << CAN_DRIVER_RX_RING_DEPTH;
  }
  EXPECT_FALSE(ring.didOverflow());
  EXPECT_FALSE(ring.append(frame(999))) << "a full ring accepted one more";
  EXPECT_TRUE(ring.didOverflow());

  CANMessage out;
  for (uint32_t i = 0; i < CAN_DRIVER_RX_RING_DEPTH; i++) {
    ASSERT_TRUE(ring.remove(out));
    EXPECT_EQ(out.id, i) << "frames came out of the ring out of order";
  }
  EXPECT_FALSE(ring.remove(out));
}

// ACAN_ESP32::end() re-initialises both rings to size 0 and treats that as
// success. new CANMessage[0] returned a valid pointer; the allocator returns
// NULL, so zero has to be ok on its own terms.
TEST_F(Buffer16, AZeroSizeRingIsOkAndHoldsNothing) {
  ACAN_ESP32_Buffer16 ring;
  EXPECT_TRUE(ring.initWithSize(0)) << "initWithSize(0) now reports a failure, which end() did not expect";
  EXPECT_EQ(ring.size(), 0u);

  EXPECT_FALSE(ring.append(frame(1)));
  CANMessage out;
  EXPECT_FALSE(ring.remove(out));
}

// The behaviour new[] could not have: a failed allocation returns, begin()
// reports it, and the ring is left EMPTY rather than claiming a size it has no
// storage for - an interrupt appending to that would write through NULL.
TEST_F(Buffer16, AFailedAllocationFailsInitAndLeavesARingThatWritesNothing) {
  ACAN_ESP32_Buffer16 ring;
  emul_heap::fail_next = true;

  EXPECT_FALSE(ring.initWithSize(CAN_DRIVER_RX_RING_DEPTH)) << "a failed allocation was reported as success";
  EXPECT_EQ(ring.size(), 0u) << "the ring claims a size it has no storage for";
  EXPECT_FALSE(ring.append(frame(1))) << "a ring with no storage accepted a frame";
}

TEST_F(Buffer16, ReInitAndFreeGiveTheStorageBack) {
  {
    ACAN_ESP32_Buffer16 ring;
    ASSERT_TRUE(ring.initWithSize(CAN_DRIVER_RX_RING_DEPTH));
    ASSERT_TRUE(ring.initWithSize(32));  // what a driver restart does
    ring.free();
    ASSERT_TRUE(ring.initWithSize(CAN_DRIVER_RX_RING_DEPTH));
  }  // destructor
  EXPECT_EQ(emul_heap::allocations, 3);
  EXPECT_EQ(emul_heap::frees, emul_heap::allocations) << "a ring's storage is not given back";
}
