#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32_RxSlot.h"

/* Reading one slot of the TWAI receive FIFO.
 *
 * The defect these tests pin: on silicon that reports Miss Status (SR.8) an
 * RX data overrun does not corrupt the FIFO. The controller marks the slot of
 * every frame it could not store and leaves the rest of the queue intact - so
 * a reader that decodes such a slot anyway delivers whatever the register
 * window still holds, which is the PREVIOUS frame. That produces k stale
 * copies of one frame followed by a gap of exactly k - the receive signature
 * measured on the bench at two different ring depths, and the reason this
 * check is not cosmetic.
 *
 * The window here is a plain array indexed the way the driver indexes the
 * peripheral, so the read path runs on the host. The stale-copy tests all
 * leave the previous frame's bytes in the window on purpose: that is what the
 * hardware does, and a test that zeroed it would pass with the Miss Status
 * check removed.
 */

namespace {

using ACAN_ESP32_RxSlot::Outcome;

constexpr bool kMissStatusSilicon = true;  // S2 / S3 / C3 / C6 / H2
constexpr bool kClassicSilicon = false;    // the original ESP32

class FakeTwaiWindow {
 public:
  volatile uint32_t* base() { return mRegs; }

  uint32_t read(uint32_t byte_offset) const { return mRegs[byte_offset >> 2]; }
  void write(uint32_t byte_offset, uint32_t value) { mRegs[byte_offset >> 2] = value; }

  /* Presents a standard-format data frame in the slot at the head of the
   * FIFO, encoded as the controller does: the identifier left-aligned across
   * two registers, the data bytes one per register.
   */
  void present_standard_frame(uint32_t id, const std::vector<uint8_t>& data) {
    clear_miss_status();
    write(ACAN_ESP32_RxSlot::kFrameInfoOffset, static_cast<uint32_t>(data.size()) & 0xF);
    write(ACAN_ESP32_RxSlot::kIdOffset, (id >> 3) & 0xFF);
    write(ACAN_ESP32_RxSlot::kIdOffset + 4, (id << 5) & 0xE0);
    for (size_t i = 0; i < data.size(); ++i) {
      write(ACAN_ESP32_RxSlot::kDataSFFOffset + 4 * static_cast<uint32_t>(i), data[i]);
    }
  }

  void present_extended_frame(uint32_t id, const std::vector<uint8_t>& data) {
    clear_miss_status();
    write(ACAN_ESP32_RxSlot::kFrameInfoOffset,
          ACAN_ESP32_RxSlot::kFrameFormatEFF | (static_cast<uint32_t>(data.size()) & 0xF));
    write(ACAN_ESP32_RxSlot::kIdOffset, (id >> 21) & 0xFF);
    write(ACAN_ESP32_RxSlot::kIdOffset + 4, (id >> 13) & 0xFF);
    write(ACAN_ESP32_RxSlot::kIdOffset + 8, (id >> 5) & 0xFF);
    write(ACAN_ESP32_RxSlot::kIdOffset + 12, (id << 3) & 0xF8);
    for (size_t i = 0; i < data.size(); ++i) {
      write(ACAN_ESP32_RxSlot::kDataEFFOffset + 4 * static_cast<uint32_t>(i), data[i]);
    }
  }

  /* Marks the head slot as the placeholder the controller leaves behind for a
   * frame it dropped. The frame registers are NOT touched: after an overrun
   * they still hold the last frame that was read, which is the stale copy a
   * reader that ignores SR.8 delivers.
   */
  void mark_head_slot_missed() {
    write(ACAN_ESP32_RxSlot::kStatusOffset, read(ACAN_ESP32_RxSlot::kStatusOffset) | ACAN_ESP32_RxSlot::kStatusMiss);
  }

  void clear_miss_status() {
    write(ACAN_ESP32_RxSlot::kStatusOffset, read(ACAN_ESP32_RxSlot::kStatusOffset) & ~ACAN_ESP32_RxSlot::kStatusMiss);
  }

  void forget_commands() { write(ACAN_ESP32_RxSlot::kCommandOffset, 0); }

  bool slot_was_released() const {
    return (read(ACAN_ESP32_RxSlot::kCommandOffset) & ACAN_ESP32_RxSlot::kReleaseBuffer) != 0;
  }

 private:
  volatile uint32_t mRegs[32] = {};
};

/* -------- the decode, unchanged by the extraction -------- */

TEST(AcanRxSlot, AStandardFrameIsDecodedAndTheSlotReleased) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x123, {0xDE, 0xAD, 0xBE, 0xEF});
  window.forget_commands();

  CANMessage frame;
  ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));

  EXPECT_EQ(0x123u, frame.id);
  EXPECT_FALSE(frame.ext);
  EXPECT_FALSE(frame.rtr);
  ASSERT_EQ(4, frame.len);
  EXPECT_EQ(0xDE, frame.data[0]);
  EXPECT_EQ(0xAD, frame.data[1]);
  EXPECT_EQ(0xBE, frame.data[2]);
  EXPECT_EQ(0xEF, frame.data[3]);
  EXPECT_TRUE(window.slot_was_released());
}

TEST(AcanRxSlot, AnExtendedFrameIsDecodedAndTheSlotReleased) {
  FakeTwaiWindow window;
  window.present_extended_frame(0x1ABCDEF0 & 0x1FFFFFFF, {0x01, 0x02});
  window.forget_commands();

  CANMessage frame;
  ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));

  EXPECT_EQ(0x1ABCDEF0u & 0x1FFFFFFFu, frame.id);
  EXPECT_TRUE(frame.ext);
  ASSERT_EQ(2, frame.len);
  EXPECT_EQ(0x01, frame.data[0]);
  EXPECT_EQ(0x02, frame.data[1]);
  EXPECT_TRUE(window.slot_was_released());
}

TEST(AcanRxSlot, ADlcAboveEightIsClampedToEight) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x100, {});
  window.write(ACAN_ESP32_RxSlot::kFrameInfoOffset, 0xF);
  window.forget_commands();

  CANMessage frame;
  ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));

  EXPECT_EQ(8, frame.len);
}

/* -------- Miss Status: the slot is skipped, not delivered -------- */

TEST(AcanRxSlot, AMissedSlotIsReportedAsAPlaceholderAndReleased) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x321, {0x11, 0x22});
  window.mark_head_slot_missed();
  window.forget_commands();

  CANMessage frame;
  EXPECT_EQ(Outcome::overrunPlaceholder, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));

  /* Released anyway: the slot has to be given back or the FIFO stops moving. */
  EXPECT_TRUE(window.slot_was_released());
}

TEST(AcanRxSlot, AMissedSlotDoesNotDeliverTheStaleWindowContents) {
  FakeTwaiWindow window;
  /* The window still holds the frame that was read before the overrun. */
  window.present_standard_frame(0x321, {0x11, 0x22, 0x33, 0x44});
  window.mark_head_slot_missed();

  CANMessage frame;
  frame.id = 0xFFFFFFFF;
  frame.len = 0xFF;

  ASSERT_EQ(Outcome::overrunPlaceholder, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));

  /* outFrame is untouched - not the stale 0x321 the window still describes.
   * This is the assertion that fails as a delivered stale copy if the SR.8
   * test goes away.
   */
  EXPECT_EQ(0xFFFFFFFFu, frame.id);
  EXPECT_EQ(0xFF, frame.len);
}

TEST(AcanRxSlot, TheFrameQueuedBehindAMissedSlotIsStillDelivered) {
  FakeTwaiWindow window;

  /* The overrun's placeholder, with the previous frame still in the window. */
  window.present_standard_frame(0x321, {0x11, 0x22});
  window.mark_head_slot_missed();
  window.forget_commands();

  CANMessage dropped;
  ASSERT_EQ(Outcome::overrunPlaceholder, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, dropped));
  ASSERT_TRUE(window.slot_was_released());

  /* Releasing it advances the FIFO to the good frame behind it. */
  window.present_standard_frame(0x456, {0x77, 0x88, 0x99});
  window.forget_commands();

  CANMessage frame;
  ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));

  EXPECT_EQ(0x456u, frame.id);
  ASSERT_EQ(3, frame.len);
  EXPECT_EQ(0x77, frame.data[0]);
  EXPECT_EQ(0x88, frame.data[1]);
  EXPECT_EQ(0x99, frame.data[2]);
  EXPECT_TRUE(window.slot_was_released());
}

TEST(AcanRxSlot, ManyMissedSlotsInARowAreAllSkipped) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x200, {0xA0});
  window.mark_head_slot_missed();

  /* k dropped frames leave k placeholders. None may be delivered - k stale
   * copies is precisely the signature this fixes.
   */
  for (int i = 0; i < 5; ++i) {
    CANMessage frame;
    window.forget_commands();
    EXPECT_EQ(Outcome::overrunPlaceholder, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame))
        << "placeholder " << i;
    EXPECT_TRUE(window.slot_was_released()) << "placeholder " << i;
  }
}

/* Only SR.8 marks a missed slot. Every other status bit belongs to something
 * else - SR.7 is Bus Status, SR.1 is Data Overrun Status - and a read that
 * consulted one of those would drop good frames whenever the controller was
 * merely bus-off or had an overrun pending. Without this the bit position is
 * unpinned: a mutation moving the mask to SR.7 passed the whole suite.
 */
TEST(AcanRxSlot, EveryStatusBitBelowSR8LeavesTheSlotDeliverable) {
  for (unsigned bit = 0; bit <= 7; ++bit) {
    FakeTwaiWindow window;
    window.present_standard_frame(0x0AA, {0x5A});
    window.write(ACAN_ESP32_RxSlot::kStatusOffset, 1U << bit);
    window.forget_commands();

    CANMessage frame;
    ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame))
        << "status bit " << bit;
    EXPECT_EQ(0x0AAu, frame.id) << "status bit " << bit;
  }
}

/* And SR.8 marks it whatever else is set alongside. */
TEST(AcanRxSlot, SR8MarksTheSlotEvenWithTheOtherStatusBitsSet) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x0AA, {0x5A});
  window.write(ACAN_ESP32_RxSlot::kStatusOffset, 0xFF | ACAN_ESP32_RxSlot::kStatusMiss);
  window.forget_commands();

  CANMessage frame;
  EXPECT_EQ(Outcome::overrunPlaceholder, ACAN_ESP32_RxSlot::read(window.base(), kMissStatusSilicon, frame));
  EXPECT_TRUE(window.slot_was_released());
}

/* -------- the classic ESP32 path is unchanged -------- */

TEST(AcanRxSlot, OnClassicSiliconSR8IsNotConsultedAndTheSlotIsRead) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x321, {0x11, 0x22});
  /* SR.8 is reserved on the classic ESP32; whatever it reads as must not
   * change the receive path there. Its overrun recovery is the whole-FIFO
   * drain in handleOverrunInterrupt(), which this read is not part of.
   */
  window.mark_head_slot_missed();
  window.forget_commands();

  CANMessage frame;
  ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kClassicSilicon, frame));

  EXPECT_EQ(0x321u, frame.id);
  ASSERT_EQ(2, frame.len);
  EXPECT_EQ(0x11, frame.data[0]);
  EXPECT_EQ(0x22, frame.data[1]);
  EXPECT_TRUE(window.slot_was_released());
}

TEST(AcanRxSlot, OnClassicSiliconAnOrdinaryFrameIsUnaffected) {
  FakeTwaiWindow window;
  window.present_standard_frame(0x0AA, {0x5A});
  window.forget_commands();

  CANMessage frame;
  ASSERT_EQ(Outcome::frame, ACAN_ESP32_RxSlot::read(window.base(), kClassicSilicon, frame));

  EXPECT_EQ(0x0AAu, frame.id);
  ASSERT_EQ(1, frame.len);
  EXPECT_EQ(0x5A, frame.data[0]);
  EXPECT_TRUE(window.slot_was_released());
}

/* -------- interrupt dispatch -------- */

/* The pairing that matters: one interrupt reporting BOTH an overrun and a
 * waiting frame. The read used to be the overrun's `else`, so it did not run.
 * That is right on the classic ESP32, whose drain has just emptied the FIFO.
 * On Miss-Status silicon nothing is drained, and the frame is still queued -
 * it is not lost either way, because the Receive Interrupt is level-triggered
 * on a non-empty FIFO and survives the read of the interrupt register, so the
 * handler is re-entered. Reading in the first pass is what removes that
 * redundant second entry.
 */
TEST(AcanRxSlotDispatch, AnOverrunAndAFrameTogetherStillReadOnMissStatusSilicon) {
  const auto dispatch = ACAN_ESP32_RxSlot::plan(ACAN_ESP32_RxSlot::kInterruptOverrun | ACAN_ESP32_RxSlot::kInterruptRx,
                                                kMissStatusSilicon);

  EXPECT_TRUE(dispatch.handleOverrun);
  EXPECT_TRUE(dispatch.readSlot);
}

TEST(AcanRxSlotDispatch, AnOverrunAndAFrameTogetherDoNotReadOnClassicSilicon) {
  const auto dispatch =
      ACAN_ESP32_RxSlot::plan(ACAN_ESP32_RxSlot::kInterruptOverrun | ACAN_ESP32_RxSlot::kInterruptRx, kClassicSilicon);

  EXPECT_TRUE(dispatch.handleOverrun);
  EXPECT_FALSE(dispatch.readSlot);
}

TEST(AcanRxSlotDispatch, APlainReceiveInterruptReadsOnEitherSilicon) {
  for (const bool has_rx_status : {kMissStatusSilicon, kClassicSilicon}) {
    const auto dispatch = ACAN_ESP32_RxSlot::plan(ACAN_ESP32_RxSlot::kInterruptRx, has_rx_status);

    EXPECT_FALSE(dispatch.handleOverrun) << "has_rx_status=" << has_rx_status;
    EXPECT_TRUE(dispatch.readSlot) << "has_rx_status=" << has_rx_status;
  }
}

TEST(AcanRxSlotDispatch, AnOverrunAloneReadsNothingOnEitherSilicon) {
  for (const bool has_rx_status : {kMissStatusSilicon, kClassicSilicon}) {
    const auto dispatch = ACAN_ESP32_RxSlot::plan(ACAN_ESP32_RxSlot::kInterruptOverrun, has_rx_status);

    EXPECT_TRUE(dispatch.handleOverrun) << "has_rx_status=" << has_rx_status;
    EXPECT_FALSE(dispatch.readSlot) << "has_rx_status=" << has_rx_status;
  }
}

/* An interrupt that reports neither must not touch the FIFO. The transmit
 * interrupt shares the word and is handled outside plan().
 */
TEST(AcanRxSlotDispatch, AnUnrelatedInterruptDoesNothingToTheReceivePath) {
  const auto dispatch = ACAN_ESP32_RxSlot::plan(0x02 /* transmit */, kMissStatusSilicon);

  EXPECT_FALSE(dispatch.handleOverrun);
  EXPECT_FALSE(dispatch.readSlot);
}

}  // namespace

/* -------- the wiring, which nothing else in this binary can reach --------
 *
 * `read()` and `plan()` are testable because they were split out; what CALLS
 * them is not. ACAN_ESP32.cpp is an ESP translation unit - it is in no host
 * target and no test compiles it - so every assertion above holds with the
 * driver wired to none of it. Measured, not feared: replacing
 * `if (dispatch.readSlot)` in the ISR with `if (false)`, so the interrupt never
 * reads a slot and the driver receives nothing at all, leaves this whole suite
 * green.
 *
 * So the four wiring facts are read out of the source, which is the same thing
 * the OTA confirmation path does for `onOTAStart()` and for the same reason.
 * A source-reading test is the weakest kind and it is still the only one that
 * can fail here.
 */

namespace {

std::string driver_source() {
  const std::string self = __FILE__;
  const std::string path =
      self.substr(0, self.find_last_of('/')) + "/../Software/src/lib/pierremolinaro-acan-esp32/ACAN_ESP32.cpp";
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << "this test reads " << path;
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The body of a function, by brace depth from its signature.
std::string body_of(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  EXPECT_NE(at, std::string::npos) << "no `" << signature << "` in the driver this test reads";
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = source.find('{', at);
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') {
      ++depth;
    } else if (source[i] == '}' && --depth == 0) {
      return source.substr(open, i - open + 1);
    }
  }
  return "";
}

// The braced block introduced by `opener`, brace-matched from its `{`.
std::string brace_block_after(const std::string& source, const std::string& opener) {
  const size_t at = source.find(opener);
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = source.find('{', at);
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') {
      ++depth;
    } else if (source[i] == '}' && --depth == 0) {
      return source.substr(open, i - open + 1);
    }
  }
  return "";
}

}  // namespace

TEST(AcanRxSlotWiring, TheInterruptTakesBothItsCallsFromPlan) {
  const std::string isr = body_of(driver_source(), "void IRAM_ATTR ACAN_ESP32::isr (void * inUserArgument)");
  ASSERT_FALSE(isr.empty());

  EXPECT_NE(isr.find("ACAN_ESP32_RxSlot::plan"), std::string::npos)
      << "the ISR no longer asks plan() what to do, so the read/drain choice is back inline where no "
         "test can reach it";
  EXPECT_NE(isr.find("dispatch.handleOverrun"), std::string::npos)
      << "the overrun handler is no longer gated on plan()";
  EXPECT_NE(isr.find("dispatch.readSlot"), std::string::npos)
      << "the slot read is no longer gated on plan(): with this gone the interrupt reads nothing and every "
         "other test in this file still passes";
  /* The old shape is the defect: an `else if` makes the read the overrun's
     alternative, which is what skipped it on Miss-Status silicon. */
  EXPECT_EQ(isr.find("}else if ((interrupt & TWAI_RX_INT_ST)"), std::string::npos)
      << "the read is the overrun's `else` again";
}

TEST(AcanRxSlotWiring, ThePlaceholderIsNotAppendedToTheReceiveQueue) {
  const std::string handler = body_of(driver_source(), "void IRAM_ATTR ACAN_ESP32::handleRXInterrupt (void)");
  ASSERT_FALSE(handler.empty());

  EXPECT_NE(handler.find("if (!getReceivedMessage (frame)) {"), std::string::npos)
      << "handleRXInterrupt() no longer tests what getReceivedMessage() returned, so a slot the read "
         "reported as a placeholder is delivered as a frame - the stale copy this branch exists to stop";
  EXPECT_NE(handler.find("return"), std::string::npos);
}

TEST(AcanRxSlotWiring, TheWholeFifoDrainStaysOnClassicSiliconOnly) {
  const std::string handler = body_of(driver_source(), "void IRAM_ATTR ACAN_ESP32::handleOverrunInterrupt (void)");
  ASSERT_FALSE(handler.empty());

  EXPECT_NE(handler.find("if (!twaiHasRxStatus) {"), std::string::npos)
      << "the drain is no longer gated on the target: on Miss-Status silicon it throws away the real "
         "frames queued behind the lost ones, which is half the defect";
  /* The clear-data-overrun command is OUTSIDE that gate on purpose - it runs on
     both targets, so the status bit cannot latch. Comparing POSITIONS is not
     enough and a mutation proved it: the command moved inside the gate still
     sits after the `if`, so an ordering test passes. What has to be asked is
     whether it is inside the gate's BRACES. */
  ASSERT_NE(handler.find("TWAI_CLR_OVERRUN"), std::string::npos) << "the clear-data-overrun command is gone";
  const std::string gated = brace_block_after(handler, "if (!twaiHasRxStatus) {");
  ASSERT_FALSE(gated.empty());
  EXPECT_EQ(gated.find("TWAI_CLR_OVERRUN"), std::string::npos)
      << "the clear-data-overrun command has moved inside the classic-only gate, so on Miss-Status "
         "silicon nothing clears the overrun status and it latches: "
      << gated;
}
