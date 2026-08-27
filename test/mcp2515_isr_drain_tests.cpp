#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <string>

#include "../Software/src/lib/mcp2515_lite/mcp2515_rx_ring.h"

/* The MCP2515's received frames leave the chip inside the interrupt.
 *
 * The reason is one measured fact about this hardware: during a flash write the
 * scheduler is suspended on both cores, but an interrupt allocated with
 * ESP_INTR_FLAG_IRAM keeps running (esp_intr_noniram_disable() masks only the
 * handlers that lack the flag). A drain that runs in a task therefore recovers
 * only what the chip's two receive buffers held - two frames, ~0.45 ms at
 * 500 kbit - while a drain that runs in the interrupt keeps moving frames out
 * for the whole window.
 *
 * Two of the pieces that buys have real logic and are tested here as behaviour:
 * the ring the drain publishes into, and the decode of a raw RX buffer. The
 * rest of the contract is about where code lives, which only the source can
 * answer, so the remaining tests read it.
 */
namespace {

// The decode is templated on the frame type precisely so that this header stays
// free of mcp2515_lite.h (SPI, FreeRTOS). This stand-in carries the members the
// decode writes; TheDriverFrameStillHasTheMembersTheDecodeWrites below is what
// keeps it honest against a rename in the driver.
struct TestFrame {
  uint8_t flags;
  bool ext;
  uint8_t dlc;
  uint32_t id;
  uint8_t data[8];
};

using TestRing = Mcp2515RxRing<TestFrame, 4>;

TestFrame frame_with_id(uint32_t id) {
  TestFrame frame{};
  frame.id = id;
  return frame;
}

std::string source(const char* relative) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../" + relative;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

std::string driver_source() {
  return source("Software/src/lib/mcp2515_lite/mcp2515_lite.cpp");
}

// The body of one function, from its signature to the next line that starts in
// column zero with a closing brace.
std::string body_of(const std::string& src, const std::string& signature) {
  const size_t start = src.find(signature);
  EXPECT_NE(start, std::string::npos) << signature << " is gone";
  if (start == std::string::npos) {
    return "";
  }
  const size_t end = src.find("\n}", start);
  EXPECT_NE(end, std::string::npos);
  return src.substr(start, end - start);
}

}  // namespace

TEST(Mcp2515RxRing, FramesComeBackInTheOrderTheyWentIn) {
  TestRing ring;
  EXPECT_EQ(ring.size(), 0u);

  for (uint32_t id = 1; id <= 3; id++) {
    EXPECT_TRUE(ring.push(frame_with_id(id)));
  }
  EXPECT_EQ(ring.size(), 3u);

  for (uint32_t id = 1; id <= 3; id++) {
    TestFrame out{};
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.id, id);
  }

  TestFrame drained{};
  EXPECT_FALSE(ring.pop(drained));
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.dropped(), 0u);
}

TEST(Mcp2515RxRing, AFullRingDropsTheNewFrameAndKeepsTheOldOnes) {
  TestRing ring;
  for (uint32_t id = 1; id <= ring.capacity(); id++) {
    ASSERT_TRUE(ring.push(frame_with_id(id)));
  }

  // The consumer is a task, and during the window this ring exists for there is
  // no task running - so overflow is the expected failure, and what matters is
  // which frames it costs. Dropping the newest keeps the ring a record of the
  // oldest unread traffic rather than a shuffled one.
  EXPECT_FALSE(ring.push(frame_with_id(99)));
  EXPECT_FALSE(ring.push(frame_with_id(100)));
  EXPECT_EQ(ring.dropped(), 2u);
  EXPECT_EQ(ring.size(), ring.capacity());

  for (uint32_t id = 1; id <= ring.capacity(); id++) {
    TestFrame out{};
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.id, id);
  }
}

TEST(Mcp2515RxRing, TheRingKeepsItsOrderAcrossManyTimesItsOwnCapacity) {
  TestRing ring;
  // Streaming far past the capacity is what catches a slot index that aliases
  // or an index pair that only agrees on the first lap. It does NOT reach the
  // 32-bit wrap of the indices themselves - that is 4 billion frames away, and
  // is argued from the difference arithmetic rather than tested here.
  for (uint32_t id = 1; id <= 1000; id++) {
    ASSERT_TRUE(ring.push(frame_with_id(id))) << "at frame " << id;
    TestFrame out{};
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.id, id);
  }
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.dropped(), 0u);
}

TEST(Mcp2515DecodeRxBuffer, AStandardFrameKeepsItsElevenBitIdentifier) {
  // 0x123 in SIDH/SIDL, IDE clear, DLC 3.
  const uint8_t rxb[MCP2515_RXB_LENGTH] = {0x24, 0x60, 0x00, 0x00, 0x03, 0xDE, 0xAD, 0xBE, 0, 0, 0, 0, 0};
  TestFrame frame{};
  mcp2515_decode_rx_buffer(rxb, frame);

  EXPECT_FALSE(frame.ext);
  EXPECT_EQ(frame.id, 0x123u);
  EXPECT_EQ(frame.dlc, 3);
  EXPECT_EQ(frame.data[0], 0xDE);
  EXPECT_EQ(frame.data[1], 0xAD);
  EXPECT_EQ(frame.data[2], 0xBE);
}

TEST(Mcp2515DecodeRxBuffer, AnExtendedFrameKeepsItsTwentyNineBitIdentifier) {
  const uint32_t id = 0x12345678;
  uint8_t rxb[MCP2515_RXB_LENGTH] = {0};
  rxb[0] = (uint8_t)(id >> 21);
  rxb[1] = (uint8_t)(((id >> 13) & 0xE0) | MCP2515_RXB_SIDL_IDE | ((id >> 16) & 0x03));
  rxb[2] = (uint8_t)(id >> 8);
  rxb[3] = (uint8_t)id;
  rxb[MCP2515_RXB_DLC] = 8;
  for (uint8_t i = 0; i < 8; i++) {
    rxb[MCP2515_RXB_DATA + i] = i + 1;
  }

  TestFrame frame{};
  mcp2515_decode_rx_buffer(rxb, frame);

  EXPECT_TRUE(frame.ext);
  EXPECT_EQ(frame.id, id);
  EXPECT_EQ(frame.dlc, 8);
  for (uint8_t i = 0; i < 8; i++) {
    EXPECT_EQ(frame.data[i], i + 1);
  }
}

TEST(Mcp2515DecodeRxBuffer, OnlyTheLengthNibbleOfTheDlcByteIsALength) {
  // RXBnDLC carries RTR in bit 6 and two reserved bits above the length. A
  // remote-transmission-request frame therefore arrives with 0x43 for "length
  // 3", and reading the whole byte as a length reads 0x43 = 67 - which the old
  // code clamped to 8, silently turning an empty frame into eight bytes of
  // whatever the buffer held.
  uint8_t rxb[MCP2515_RXB_LENGTH] = {0x24, 0x60, 0x00, 0x00, 0x43, 0x11, 0x22, 0x33, 0, 0, 0, 0, 0};
  TestFrame frame{};
  mcp2515_decode_rx_buffer(rxb, frame);
  EXPECT_EQ(frame.dlc, 3);

  // And a genuinely out-of-range length still cannot walk past the eight data
  // bytes the chip holds.
  rxb[MCP2515_RXB_DLC] = 0x0F;
  mcp2515_decode_rx_buffer(rxb, frame);
  EXPECT_EQ(frame.dlc, 8);
}

// Runs of whitespace to a single space, so an assertion about the ORDER of
// statements does not also assert their indentation.
std::string collapse_whitespace(const std::string& in) {
  std::string out;
  bool in_space = false;
  for (char c : in) {
    if (isspace(static_cast<unsigned char>(c))) {
      in_space = true;
      continue;
    }
    if (in_space && !out.empty()) {
      out += ' ';
    }
    in_space = false;
    out += c;
  }
  return out;
}

TEST(Mcp2515IsrDrain, EverythingTheInterruptCallsIsPinnedIntoIram) {
  const std::string src = driver_source();

  // The whole point is that this path runs with the flash cache off. A call out
  // of it into flash is a crash, not a slowdown.
  for (const char* pinned :
       {"void IRAM_ATTR MCP2515_Lite::drainRx()", "bool IRAM_ATTR MCP2515_Lite::busTryAcquireIsr()",
        "void IRAM_ATTR MCP2515_Lite::busReleaseIsr()",
        "void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg)"}) {
    EXPECT_NE(src.find(pinned), std::string::npos)
        << pinned << " is not pinned into IRAM - with the cache off it would fault instead of running";
  }

  const std::string spi = source("Software/src/lib/mcp2515_lite/mcp2515_iram_spi.cpp");
  // Repinned to the return type it now has; what the assertion is for
  // is the IRAM_ATTR on the definition, not the void.
  EXPECT_NE(spi.find("IRAM_ATTR Mcp2515IramSpi::transfer("), std::string::npos)
      << "the register-level transfer is the drain's innermost call and is not in IRAM";
}

TEST(Mcp2515IsrDrain, TheDrainCallsNothingThatLivesInFlash) {
  const std::string drain = body_of(driver_source(), "void IRAM_ATTR MCP2515_Lite::drainRx()");

  // Each of these is flash-resident here: the Arduino SPI class, the logging
  // macro and the queue API. The ring, the decode and the register-level
  // transfer are the substitutes, and they are a header, a header and IRAM.
  for (const char* forbidden : {"_spi.beginTransaction", "_spi.transferBytes", "spiTransactionBlocking", "DEBUG_PRINTF",
                                "xQueueSend", "digitalWrite", "digitalRead", "logging"}) {
    EXPECT_EQ(drain.find(forbidden), std::string::npos)
        << "drainRx() calls " << forbidden << ", which is not resident when the flash cache is off";
  }
  EXPECT_NE(drain.find("_iram_spi.transfer("), std::string::npos) << "the drain no longer uses the IRAM SPI service";
  EXPECT_NE(drain.find("_isr_ring.push("), std::string::npos) << "the drain no longer publishes into the ring";
}

/* `inline` is permission, not instruction - and the permission was not taken.
 *
 * The drain's whole legality rests on every function it reaches being resident
 * with the flash cache off. TheDrainCallsNothingThatLivesInFlash above checks
 * that by NAME, which is all a source scan can do; it cannot see where the
 * compiler actually put a header function. At -Os it put one out-of-line copy of
 * mcp2515_decode_rx_buffer into .flash.text and had the IRAM drain reach it with
 * an l32r/callx8 - measured in the built ELF on esp32devkit_330 and lilygo_330,
 * while stark_330 inlined it, from identical source. So the property was holding
 * by luck of inlining, per env, and the first frame to arrive during a flash
 * write on a devkit would have faulted.
 *
 * always_inline is what makes it a build error instead of a coin toss, so what
 * is pinned here is the attribute. Confirming the OUTCOME needs a linked image,
 * which no host test has: ekholm-notes scripts/mcp2515_isr_iram_audit.py walks
 * the interrupt's call graph in a built .elf and is the instrument for that.
 */
TEST(Mcp2515IsrDrain, TheHeaderCodeTheDrainReachesCannotBeLeftOutOfLine) {
  const std::string ring = source("Software/src/lib/mcp2515_lite/mcp2515_rx_ring.h");

  EXPECT_NE(ring.find("#define MCP2515_ISR_INLINE inline __attribute__((always_inline))"), std::string::npos)
      << "the ISR-inlining attribute is gone - the drain's calls into this header are back to whatever the compiler "
         "felt like, and out-of-line means .flash.text means a fault with the cache off";

  // Everything drainRx() reaches in this header, not just the decode: the two
  // unpackers it calls and the ring push it publishes through.
  for (const char* fn : {"static MCP2515_ISR_INLINE uint32_t mcp2515_unpack_extended_id(",
                         "static MCP2515_ISR_INLINE uint32_t mcp2515_unpack_standard_id(",
                         "static MCP2515_ISR_INLINE void mcp2515_decode_rx_buffer(",
                         "MCP2515_ISR_INLINE bool push(const Frame& frame) {"}) {
    EXPECT_NE(ring.find(fn), std::string::npos)
        << fn << " is not forced inline, so the compiler may emit it into flash and the interrupt would call it there";
  }
}

/* An SPI transfer that never completed must not become a CAN frame.
 *
 * The register-level service busy-waits on USR and gives up after a bounded
 * spin. It used to give up by BREAKING out of the wait and then copying the
 * receive registers out anyway - which hold whatever was there, not what the
 * chip sent. drainRx() had no way to know, so it decoded those bytes and pushed
 * them into the ring: not a lost frame but an invented one, with an arbitrary
 * identifier and payload, handed to a battery or inverter driver as traffic.
 * The header even described the outcome as the frames being "lost".
 */
TEST(Mcp2515IsrDrain, AnIncompleteTransferIsDroppedRatherThanDecoded) {
  const std::string spi = source("Software/src/lib/mcp2515_lite/mcp2515_iram_spi.cpp");
  const std::string transfer = body_of(spi, "IRAM_ATTR Mcp2515IramSpi::transfer(");

  const size_t timeout = transfer.find("_timeouts = _timeouts + 1");
  ASSERT_NE(timeout, std::string::npos) << "the transfer no longer counts giving up on the peripheral";
  EXPECT_NE(transfer.find("completed = false", timeout), std::string::npos)
      << "giving up on the peripheral is counted but not reported, so the caller cannot tell a completed transfer "
         "from one whose receive registers were never written";
  EXPECT_NE(transfer.find("return completed;"), std::string::npos)
      << "an incomplete transfer still copies the receive registers out - those bytes are not what the chip sent";

  const std::string drain = body_of(driver_source(), "void IRAM_ATTR MCP2515_Lite::drainRx()");
  EXPECT_NE(drain.find("if (!_iram_spi.transfer(cmd_frame, rx_frame, 3)) {"), std::string::npos)
      << "the drain acts on a CANINTF read that may never have happened - a fabricated flag byte makes it read "
         "receive buffers that hold nothing";
  EXPECT_NE(drain.find("if (!_iram_spi.transfer(cmd_frame, rx_frame, MCP2515_RXB_LENGTH + 1)) {"), std::string::npos)
      << "the drain decodes a receive buffer whose transfer did not complete, publishing a frame that was never on "
         "the wire";
}

TEST(Mcp2515IsrDrain, TheInterruptIsInstalledWithTheFlagThatSurvivesAFlashWrite) {
  const std::string src = driver_source();

  // Without ESP_INTR_FLAG_IRAM the source is masked for the duration of every
  // flash operation, and an IRAM handler behind a masked source never runs -
  // the drain would be pinned code that is switched off exactly when it matters.
  EXPECT_NE(src.find("gpio_install_isr_service(ESP_INTR_FLAG_IRAM)"), std::string::npos)
      << "the GPIO interrupt service is installed without ESP_INTR_FLAG_IRAM";
  EXPECT_NE(src.find("gpio_isr_handler_add((gpio_num_t)_int_pin, mcp2515_isr_handler, this)"), std::string::npos)
      << "the handler goes back through Arduino's dispatcher, which is not IRAM in this build";

  // And when someone else installed the service first, its flags are unknown,
  // so the drain must decline rather than assume.
  const std::string install = body_of(src, "bool MCP2515_Lite::installIsrDrainInterrupt()");
  EXPECT_NE(install.find("ESP_ERR_INVALID_STATE"), std::string::npos)
      << "an already-installed interrupt service is treated as if it were ours";
  EXPECT_NE(install.find("return false"), std::string::npos);
}

TEST(Mcp2515IsrDrain, OscillatorAutodetectionCannotLockTheDrainOut) {
  const std::string src = driver_source();

  // begin() runs twice when MCP2515_FREQ() is 0 - the devkit and the 3LB, i.e.
  // every boot on the boards where the drain is otherwise live. The GPIO
  // interrupt service is installed once for the whole system, so if the first
  // pass reaches for attachInterrupt() the service is installed WITHOUT
  // ESP_INTR_FLAG_IRAM and the second pass can only decline. Both passes have
  // to take the same path.
  EXPECT_NE(src.find("_isr_interrupt_installed = _isr_drain_requested && installIsrDrainInterrupt();"),
            std::string::npos)
      << "the interrupt path is chosen on something other than whether the drain was requested - if "
         "autodetection is excluded, it installs Arduino's service first and the drain never runs";
  EXPECT_EQ(src.find("detachInterrupt(digitalPinToInterrupt(_int_pin));\n    reset();"), std::string::npos)
      << "autodetection tears the pin down with detachInterrupt() directly, which does not remove an "
         "IDF-registered handler";

  // The drain itself must still stay out of autodetection, which runs the chip
  // in loopback and would otherwise leave its test frame in the ring.
  EXPECT_NE(src.find("if (_isr_interrupt_installed && !skip_task_start) {"), std::string::npos)
      << "the drain goes live during autodetection - its loopback test frame would reach the consumer as traffic";
}

TEST(Mcp2515IsrDrain, TheRingHasExactlyOneProducer) {
  const std::string task = body_of(driver_source(), "void MCP2515_Lite::canTask(void* pvParameters)");

  // The ring is lock-free because its producers never run at once. The task may
  // only read frames when the interrupt is not doing it, and only while holding
  // the bus, which is what locks the interrupt out.
  EXPECT_NE(task.find("if (!self->_isr_drain_enabled) {"), std::string::npos)
      << "the task drains receive buffers even with the interrupt drain live - two producers on one ring head";
  // Whitespace-collapsed: the property is that the three calls are adjacent and in
  // that order, not how deeply the block happens to be indented. Matching the
  // literal indentation made this fail on a re-indent that changed nothing.
  EXPECT_NE(collapse_whitespace(task).find("self->busAcquireTask(); self->drainRx(); self->busReleaseTask();"),
            std::string::npos)
      << "the task's own drain call no longer holds the bus across it";
  EXPECT_NE(task.find("digitalRead(self->_int_pin) == LOW"), std::string::npos)
      << "nothing re-checks the level after a deferral - a frame that arrived then waits for the 1000 ms backstop";
}

TEST(Mcp2515IsrDrain, TheInterruptNeverWaitsForTheTask) {
  const std::string acquire = body_of(driver_source(), "bool IRAM_ATTR MCP2515_Lite::busTryAcquireIsr()");

  // A lock in the interrupt is the one shape that cannot work: the task holding
  // it is frozen for the whole flash window, so the interrupt would spin
  // through exactly the window it was written to work through.
  EXPECT_EQ(acquire.find("while ("), std::string::npos) << "the interrupt waits for the bus instead of giving up";
  EXPECT_EQ(acquire.find("portENTER_CRITICAL"), std::string::npos) << "the interrupt takes a lock";
  EXPECT_NE(acquire.find("return false"), std::string::npos) << "the interrupt has no way to decline the bus";
}

TEST(Mcp2515IsrDrain, TheDrainIsOnlyOfferedOnABusThisChipHasToItself) {
  const std::string src = source("Software/src/communication/can/comm_can.cpp");
  const std::string exclusive = body_of(src, "static bool mcp2515_bus_is_exclusive()");

  EXPECT_NE(exclusive.find("#ifdef SDCARD"), std::string::npos)
      << "the SD card no longer disqualifies the bus - on the T-CAN485 that is the pair measured going deaf when the "
         "bus is shared";
  EXPECT_NE(exclusive.find("esp32hal->SD_SPI_BUS() == bus"), std::string::npos);
  EXPECT_NE(exclusive.find("esp32hal->MCP2517_BUS() == bus"), std::string::npos)
      << "an FD chip on the same controller no longer disqualifies the bus";
  EXPECT_NE(exclusive.find("esp32hal->MCP2517_BUS2() == bus"), std::string::npos)
      << "the second FD chip defaults to DEFAULT_MCP2515_BUS on the T-2CAN, so it has to be checked too";
  EXPECT_NE(src.find("if (mcp2515_bus_is_exclusive()) {\n      can2515->useIsrDrain("), std::string::npos)
      << "the drain is enabled without asking whether the bus is exclusive";
}

TEST(Mcp2515IsrDrain, TheDriverFrameStillHasTheMembersTheDecodeWrites) {
  // TestFrame above stands in for MCP2515_Lite_Frame, which cannot be included
  // on the host. If the driver's frame is renamed or reshaped, the stand-in
  // silently stops representing it - so pin the members the decode assigns.
  const std::string header = source("Software/src/lib/mcp2515_lite/mcp2515_lite.h");
  const size_t start = header.find("} MCP2515_Lite_Frame;");
  ASSERT_NE(start, std::string::npos) << "MCP2515_Lite_Frame is gone or renamed";
  const std::string frame = header.substr(header.rfind("typedef struct {", start), start);
  for (const char* member : {"uint8_t flags;", "bool ext;", "uint8_t dlc;", "uint32_t id;", "uint8_t data[8];"}) {
    EXPECT_NE(frame.find(member), std::string::npos)
        << member << " is gone from MCP2515_Lite_Frame - the decode writes it and the host stand-in claims it exists";
  }
}
