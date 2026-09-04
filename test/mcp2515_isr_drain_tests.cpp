#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../Software/src/lib/mcp2515_lite/mcp2515_rx_ring.h"
#include "../Software/src/lib/mcp2515_lite/mcp2515_tx_status.h"

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

/* The extent of the `{ ... }` block opening at or after `at`, by brace depth.
 *
 * Every earlier attempt to bound a statement to its enclosing block here used
 * the closing brace's INDENTATION - first as part of a literal, then as a
 * `find("\n    }")`. Both measure formatting rather than structure: the first
 * broke when the MCP2515 block was nested one level deeper by a change with
 * nothing to say about the ISR drain, and the second then matched the wrong
 * brace entirely, which is how a guard could be made decorative with this file
 * still green. Depth is the property actually being asserted.
 */
std::pair<size_t, size_t> block_at(const std::string& src, size_t at) {
  const size_t open = src.find('{', at);
  if (open == std::string::npos) {
    return {std::string::npos, std::string::npos};
  }
  int depth = 0;
  for (size_t j = open; j < src.size(); ++j) {
    if (src[j] == '{') {
      ++depth;
    } else if (src[j] == '}' && --depth == 0) {
      return {open, j};
    }
  }
  return {open, std::string::npos};
}

// The integer behind a `#define NAME value`, for headers the host cannot
// include (mcp2515_lite.h pulls in SPI and FreeRTOS).
long defined_value(const std::string& src, const std::string& name) {
  const size_t at = src.find("#define " + name + " ");
  EXPECT_NE(at, std::string::npos) << name << " is gone";
  if (at == std::string::npos) {
    return -1;
  }
  return strtol(src.c_str() + at + name.size() + 9, nullptr, 10);
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

/* The transmit half of READ STATUS.
 *
 * The three TXREQ bits sit at 2, 4 and 6, interleaved with the transmit
 * interrupt flags at 3, 5 and 7 - which answer a different question (a
 * completion nobody has acknowledged) and are what the deleted shadow mask was
 * built from. Reading the wrong bit of a pair one apart is the whole failure
 * mode, so the tests below set exactly one bit at a time.
 */
TEST(Mcp2515TxStatus, AChipWithNothingToSendOffersEveryBuffer) {
  EXPECT_EQ(mcp2515_tx_free_mask(0x00), MCP2515_TX_ALL_FREE);
  // The receive flags share the byte and say nothing about transmit buffers.
  EXPECT_EQ(mcp2515_tx_free_mask(0x03), MCP2515_TX_ALL_FREE);
}

TEST(Mcp2515TxStatus, ABufferWithAFrameStillInItIsNotFree) {
  // TXB0, TXB1, TXB2 pending, one at a time.
  EXPECT_EQ(mcp2515_tx_free_mask(0x04), 0x06);
  EXPECT_EQ(mcp2515_tx_free_mask(0x10), 0x05);
  EXPECT_EQ(mcp2515_tx_free_mask(0x40), 0x03);

  // And all three at once, which is the case the speed change waits on.
  EXPECT_EQ(mcp2515_tx_free_mask(0x54), 0x00);
}

TEST(Mcp2515TxStatus, ATransmitCompletionIsNotTheSameAsABusyBuffer) {
  // TX0IF, TX1IF and TX2IF set with every TXREQ clear: three frames sent and
  // not yet acknowledged. The buffers are free - reading these bits instead
  // would report the opposite of the truth.
  EXPECT_EQ(mcp2515_tx_free_mask(0xA8), MCP2515_TX_ALL_FREE);
}

TEST(Mcp2515IsrDrain, EverythingTheInterruptCallsIsPinnedIntoIram) {
  const std::string src = driver_source();

  // The whole point is that this path runs with the flash cache off. A call out
  // of it into flash is a crash, not a slowdown.
  for (const char* pinned :
       {"bool IRAM_ATTR MCP2515_Lite::drainRx()", "bool IRAM_ATTR MCP2515_Lite::busTryAcquireIsr()",
        "void IRAM_ATTR MCP2515_Lite::busReleaseIsr()", "void IRAM_ATTR MCP2515_Lite::maskIsrPin()",
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
  const std::string drain = body_of(driver_source(), "bool IRAM_ATTR MCP2515_Lite::drainRx()");

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

  const std::string drain = body_of(driver_source(), "bool IRAM_ATTR MCP2515_Lite::drainRx()");
  EXPECT_NE(drain.find("if (!_iram_spi.transfer(cmd_frame, rx_frame, 3)) {"), std::string::npos)
      << "the drain acts on a CANINTF read that may never have happened - a fabricated flag byte makes it read "
         "receive buffers that hold nothing";
  EXPECT_NE(drain.find("if (!_iram_spi.transfer(cmd_frame, rx_frame, MCP2515_RXB_LENGTH + 1)) {"), std::string::npos)
      << "the drain decodes a receive buffer whose transfer did not complete, publishing a frame that was never on "
         "the wire";

  /*(d) added the second half of giving up: saying so. The pin is level
   * triggered, so an abandoned drain that reports success leaves a low pin
   * nobody masks and nobody clears, and the interrupt is re-entered on it
   * forever.
   */
  size_t reported = 0;
  for (size_t at = drain.find("return false;"); at != std::string::npos; at = drain.find("return false;", at + 1)) {
    reported++;
  }
  EXPECT_EQ(reported, 2u) << "the drain has " << reported
                          << " abandoned paths that report failure, and it has two transfers it can abandon - one "
                             "reporting success would leave the level trigger with nothing to clear it";
}

TEST(Mcp2515IsrDrain, TheInterruptSurvivesAFlashWriteThroughTheGlobalFlag) {
  const std::string src = driver_source();
  const std::string config = source("sdkconfig.be_size.defaults");

  // Without ESP_INTR_FLAG_IRAM on the GPIO service the source is masked for
  // the duration of every flash operation, and an IRAM handler behind a masked
  // source never runs - the drain would be pinned code that is switched off
  // exactly when it matters. The build now supplies the flag through
  // CONFIG_ARDUINO_ISR_IRAM=y in the shipping config instead of a bespoke
  // service allocation here: the flag is a property of the interrupt SOURCE,
  // and a service this driver allocated with it would bind every later
  // attachInterrupt() caller to it silently.
  EXPECT_NE(config.find("\nCONFIG_ARDUINO_ISR_IRAM=y"), std::string::npos)
      << "the shipping config does not carry CONFIG_ARDUINO_ISR_IRAM=y - without it Arduino's GPIO service "
         "is flash-resident and the drain is masked for exactly the windows it exists to cover";
  EXPECT_NE(src.find("attachInterruptArg(digitalPinToInterrupt(_int_pin), mcp2515_isr_handler, this, FALLING)"),
            std::string::npos)
      << "the handler does not register through Arduino's dispatcher";
  EXPECT_EQ(src.find("gpio_install_isr_service"), std::string::npos)
      << "the bespoke service allocation is back - it makes ESP_INTR_FLAG_IRAM a property every later "
         "attachInterrupt() caller inherits silently";
  EXPECT_EQ(src.find("gpio_isr_handler_add"), std::string::npos)
      << "the handler bypasses Arduino's dispatcher - with the global flag on, the dispatcher is IRAM and "
         "the bypass buys nothing but a second registration path";
}

TEST(Mcp2515IsrDrain, AFlashResidentHandlerIsRefusedAtBoot) {
  const std::string src = driver_source();

  // The flag makes Arduino's DISPATCHER IRAM-resident, not the callbacks it
  // dispatches: a flash-resident callback on an IRAM service is a cache-off
  // fetch in exactly the window the flag keeps serviced - a crash where the
  // old arrangement merely lost frames. The boot-time check refuses to
  // register such a handler at all; the deep call chain is audited per linked
  // image by mcp2515_isr_iram_audit.py, which a source test cannot see.
  const size_t check = src.find("esp_ptr_in_iram(reinterpret_cast<const void*>(&MCP2515_Lite::mcp2515_isr_handler))");
  ASSERT_NE(check, std::string::npos) << "the boot-time IRAM check on the handler is gone";
  const size_t attach = src.find("attachInterruptArg(digitalPinToInterrupt(_int_pin)");
  ASSERT_NE(attach, std::string::npos);
  EXPECT_LT(check, attach) << "the handler is registered before its residency is checked - a flash-resident "
                              "handler would be live on the IRAM service until the check runs";
}

TEST(Mcp2515IsrDrain, NoProjectLevelInterruptRegistrationGrowsUnaudited) {
  // The boot-time residency check above covers only THIS driver's handler.
  // With CONFIG_ARDUINO_ISR_IRAM=y the dispatcher services every registered
  // GPIO handler through flash windows, so any NEW attachInterrupt() caller
  // with a flash-resident handler is a cache-off crash the moment its pin
  // fires during a write. Today the project registers no GPIO interrupt
  // outside the vendored libs (mcp2515_lite checks itself; ACAN2517FD's
  // registration is gone - comm_can.cpp passes nullptr with INT at 255, and
  // the library skips attachInterrupt entirely). A new caller must join the
  // per-image audit (mcp2515_isr_iram_audit.py) before this census grows.
  const std::string self = __FILE__;
  const std::string root = self.substr(0, self.find_last_of('/')) + "/../Software/src";
  const std::string lib_dir = "/lib/";
  std::vector<std::string> hits;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    const std::string path = entry.path().string();
    if (!entry.is_regular_file() || path.find(lib_dir) != std::string::npos) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".h") {
      continue;
    }
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
      const std::string code = line.substr(0, line.find("//"));
      for (const char* call :
           {"attachInterrupt(", "attachInterruptArg(", "gpio_isr_handler_add(", "gpio_install_isr_service("}) {
        if (code.find(call) != std::string::npos) {
          hits.push_back(path + ": " + line);
        }
      }
    }
  }
  EXPECT_TRUE(hits.empty()) << "a project-level GPIO interrupt registration site appeared - with "
                               "CONFIG_ARDUINO_ISR_IRAM=y its handler runs during flash windows and must be "
                               "IRAM-resident and covered by mcp2515_isr_iram_audit.py:\n" +
                                   [&hits] {
                                     std::string all;
                                     for (const auto& hit : hits) {
                                       all += hit + "\n";
                                     }
                                     return all;
                                   }();
}

TEST(Mcp2515IsrDrain, OscillatorAutodetectionCannotLockTheDrainOut) {
  const std::string src = driver_source();

  // begin() runs twice when MCP2515_FREQ() is 0 - the devkit and the 3LB, i.e.
  // every boot on the boards where the drain is otherwise live. With the flag
  // there is exactly ONE registration path, so the second pass cannot find
  // state the first pass poisoned; this pins that a second path does not grow
  // back.
  const size_t first = src.find("attachInterruptArg(");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(src.find("attachInterruptArg(", first + 1), std::string::npos)
      << "a second registration path is back - autodetection and the real begin() can then diverge again";

  // The drain itself must still stay out of autodetection, which runs the chip
  // in loopback and would otherwise leave its test frame in the ring.
  EXPECT_NE(src.find("if (_isr_interrupt_installed && !skip_task_start) {"), std::string::npos)
      << "the drain goes live during autodetection - its loopback test frame would reach the consumer as traffic";
}

/*(d) criterion (iv): the ring's second producer is gone entirely.
 *
 * It used to be the task, re-reading the pin's level after each wake to collect
 * what the interrupt had deferred. That was the weak half of the design: task
 * context is frozen for the whole flash window, which is exactly when the
 * frames need collecting. The level trigger holds the request in hardware
 * instead, so the interrupt is the only producer and the ring's lock-freedom
 * stops resting on two writers taking turns.
 */
TEST(Mcp2515IsrDrain, TheRingHasExactlyOneProducer) {
  const std::string src = driver_source();
  const std::string task = body_of(src, "void MCP2515_Lite::canTask(void* pvParameters)");

  EXPECT_NE(task.find("if (!self->_isr_drain_enabled) {"), std::string::npos)
      << "the task drains receive buffers even with the interrupt drain live - two producers on one ring head";
  EXPECT_EQ(task.find("drainRx()"), std::string::npos)
      << "the task drains into the ring again - the interrupt is meant to be the only producer";
  EXPECT_EQ(task.find("digitalRead(self->_int_pin)"), std::string::npos)
      << "the task is back to re-reading the pin, which is the recovery that does not work in the window it is for";

  // And the one caller that is left is the interrupt.
  EXPECT_NE(body_of(src, "void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg)").find("drainRx()"),
            std::string::npos)
      << "the interrupt no longer drains, so nothing does";
}

/*(d) criterion (i): only receive flags may reach the pin.
 *
 * The level trigger below is only sound while every source of /INT clears
 * itself. READ RX BUFFER clears RXnIF on the chip select's rising edge, so a
 * receive flag is gone before the interrupt returns; TXnIF needs a register
 * write that only the task makes, and the task is frozen for the whole of a
 * flash write. One transmit completion would hold the pin low for the window.
 */
TEST(Mcp2515IsrDrain, OnlyReceiveFlagsCanPullTheInterruptPinLow) {
  const std::string begin = body_of(driver_source(), "bool MCP2515_Lite::begin(const MCP2515_Lite_Speed& speed");

  const size_t enable = begin.find("modifyRegister(REG_CANINTE,");
  ASSERT_NE(enable, std::string::npos) << "nothing configures CANINTE";
  const std::string line = begin.substr(enable, begin.find('\n', enable) - enable);

  // The mask has to name TX0IE - a bit modify only writes the bits it masks, so
  // leaving it out would leave a previously enabled TX0IE enabled - and the
  // data must not.
  EXPECT_NE(line.find("CANINTE_TX0IE"), std::string::npos)
      << "TX0IE is not in the write mask, so nothing clears it: " << line;
  const size_t comma = line.find(',', line.find(',') + 1);
  ASSERT_NE(comma, std::string::npos) << line;
  const std::string data = line.substr(comma);
  EXPECT_EQ(data.find("CANINTE_TX0IE"), std::string::npos)
      << "the transmit interrupt is enabled, and TXnIF does not clear itself - the level trigger would never "
         "release: "
      << line;
  EXPECT_NE(data.find("CANINTE_RX0IE"), std::string::npos) << line;
  EXPECT_NE(data.find("CANINTE_RX1IE"), std::string::npos) << line;
}

/*(d) criterion (ii): the pin is level triggered where the interrupt
 * drains, and only there.
 *
 * An edge is a one-shot. A frame arriving while the interrupt is deferring to
 * the task pulls /INT low once, and nothing brings that fall back - so inside a
 * flash window, where the task cannot act, the frames waited for the 1000 ms
 * backstop. A level is not consumed by being read.
 *
 * The converse matters just as much: where the TASK drains, a level nobody
 * clears is re-entered until the task gets to run, which is a storm rather than
 * a wake. So the switch belongs where the drain is proven live, not at install
 * time - autodetection installs the interrupt too, and binding the
 * register-level SPI can still fail.
 */
TEST(Mcp2515IsrDrain, ThePinIsLevelTriggeredOnlyOnceThereIsADrainBehindIt) {
  const std::string src = driver_source();

  const size_t enabled = src.find("_isr_drain_enabled = true;");
  ASSERT_NE(enabled, std::string::npos);
  const size_t level = src.find("GPIO_INTR_LOW_LEVEL", enabled);
  ASSERT_NE(level, std::string::npos) << "the pin is never switched to level triggering, so a deferred frame still "
                                         "waits for a fall that has already happened";
  EXPECT_EQ(src.rfind("GPIO_INTR_LOW_LEVEL", enabled), std::string::npos)
      << "the pin is level triggered before the drain is known to be live - nothing would clear that level";

  // The install-time type stays an edge: attachInterruptArg registers FALLING,
  // and only the point where the drain is known live switches to a level. A
  // level nobody drains is a level nobody clears.
  EXPECT_NE(src.find("attachInterruptArg(digitalPinToInterrupt(_int_pin), mcp2515_isr_handler, this, FALLING)"),
            std::string::npos)
      << "the interrupt is installed level triggered, before there is anything behind it to clear the level";
}

/*(d): an interrupt that could not drain must mask its own pin.
 *
 * This is the correction the row's premise needed. Level triggering is free
 * while the drain runs - READ RX BUFFER releases the pin before the interrupt
 * returns - but the interrupt has two paths that return with the pin still low:
 * the task held the SPI bus, or a transfer never completed. Either one re-enters
 * immediately and forever, starving the very task that has to release the bus.
 * Masking makes the level a retry: busReleaseTask() re-arms, the still-low pin
 * re-enters at once, and the frames leave the chip.
 */
TEST(Mcp2515IsrDrain, AnInterruptThatCouldNotDrainMasksItsOwnPin) {
  const std::string src = driver_source();
  const std::string handler = body_of(src, "void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg)");

  const size_t deferral = handler.find("_isr_bus_deferrals = instance->_isr_bus_deferrals + 1");
  ASSERT_NE(deferral, std::string::npos) << "the interrupt no longer counts deferring to the task";
  EXPECT_NE(handler.find("maskIsrPin()", deferral), std::string::npos)
      << "the interrupt defers to the task without masking the level it did not clear - it is re-entered on that "
         "level until the task it is waiting for gets to run";

  const size_t incomplete = handler.find("if (!drained)");
  ASSERT_NE(incomplete, std::string::npos)
      << "the interrupt ignores whether the drain finished, so a transfer that never completed leaves the pin low "
         "with nothing to clear it";
  EXPECT_NE(handler.find("maskIsrPin()", incomplete), std::string::npos);

  // The mask itself may only be register writes: it runs with the cache off.
  const std::string mask = body_of(src, "void IRAM_ATTR MCP2515_Lite::maskIsrPin()");
  EXPECT_NE(mask.find("gpio_ll_intr_disable("), std::string::npos)
      << "the mask goes through something other than the always_inline register write - gpio_intr_disable() is "
         "flash-resident and would fault in the window this path exists for";
  for (const char* forbidden : {"gpio_intr_disable(", "DEBUG_PRINTF", "portENTER_CRITICAL"}) {
    EXPECT_EQ(mask.find(forbidden), std::string::npos) << "maskIsrPin() calls " << forbidden;
  }
}

/* A clean drain is the whole event, so it wakes nobody.
 *
 * Once CANINTE is receive-only the interrupt fires for exactly one reason, and
 * the drain finishes that reason before returning - the consumer takes frames
 * from the ring, not from the task. Waking the task anyway costs two SPI
 * transactions per received frame, and while it holds the bus for those the
 * NEXT interrupt has to defer: an RX interrupt scheduling the task's TX work is
 * what manufactures the contention the handover exists to survive, and it is
 * also what leaves a flash window likelier to start with the task holding the
 * bus - the one case the drain cannot help with.
 *
 * The two exceptions are the two that leave work only the task can do: the
 * drain is off, so the frames are still in the chip; or the pin was masked, and
 * only a task transaction re-arms it.
 */
TEST(Mcp2515IsrDrain, ADrainThatFinishedItsWorkWakesNobody) {
  const std::string handler = body_of(driver_source(), "void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg)");

  const size_t notify = handler.find("vTaskNotifyGiveFromISR(");
  ASSERT_NE(notify, std::string::npos) << "the interrupt can no longer wake the task at all";
  EXPECT_NE(handler.find("wake_task &&"), std::string::npos)
      << "the task is woken on every interrupt - with a receive-only CANINTE that is once per frame, for work it "
         "does not have, and the bus hold it costs is what makes the next interrupt defer";

  // The default is to wake: only the drain being live may turn it off.
  EXPECT_NE(handler.find("bool wake_task = true;"), std::string::npos)
      << "the fallback path no longer wakes the task, and there the frames are still in the chip with only the task "
         "able to read them out";
  const size_t off = handler.find("wake_task = false;");
  ASSERT_NE(off, std::string::npos) << "nothing suppresses the wake, so the drain still pays for a task it does not "
                                       "need";
  EXPECT_LT(handler.find("_isr_drain_enabled"), off) << "the wake is suppressed without checking that the drain is "
                                                        "live";

  // And both paths that mask must turn it back on, or the pin stays off until
  // the poll timeout.
  size_t restored = 0;
  for (size_t at = handler.find("wake_task = true;", off); at != std::string::npos;
       at = handler.find("wake_task = true;", at + 1)) {
    restored++;
  }
  EXPECT_EQ(restored, 2u) << "the interrupt has two paths that mask the pin and " << restored
                          << " that ask for the task that re-arms it - a mask nobody re-arms waits for the poll "
                             "timeout, which is the backstop this item exists to stop relying on";
}

/* The poll that is now the only backstop has to be worth waking for.
 *
 * ADrainThatFinishedItsWorkWakesNobody took receive out of the task's reasons
 * to run, which leaves this poll as the sole recovery for everything else: an
 * ERRIF the chip cannot raise on a receive-only pin, a transmit buffer that
 * freed with nothing sending to notice it, and the remote case of a pin the
 * interrupt masked with no task transaction following to re-arm it. At 1000 ms
 * each of those is a second of blindness in a driver whose point is not losing
 * a millisecond, so the drain shortens it - and only the drain does, because
 * without it the task is woken per frame anyway and the extra polls would be
 * pure bus traffic.
 */
TEST(Mcp2515IsrDrain, TheDrainShortensThePollThatIsNowItsOnlyBackstop) {
  const std::string header = source("Software/src/lib/mcp2515_lite/mcp2515_lite.h");
  const long plain = defined_value(header, "MCP2515_LITE_POLL_TIMEOUT_MS");
  const long drained = defined_value(header, "MCP2515_LITE_ISR_DRAIN_POLL_TIMEOUT_MS");

  ASSERT_GT(plain, 0) << "the poll timeout is not a positive number of milliseconds";
  ASSERT_GT(drained, 0) << "a zero or negative drain poll timeout is a busy loop holding the SPI bus, which is the "
                           "one thing the interrupt cannot survive";
  EXPECT_LT(drained, plain) << "the drain's poll is " << drained << " ms against the plain " << plain
                            << " ms - it is meant to be SHORTER, because with receive answered by the interrupt "
                               "this poll is the only thing left that finds an error or frees a transmit buffer";

  const std::string task = body_of(driver_source(), "void MCP2515_Lite::canTask(void* pvParameters)");
  EXPECT_NE(
      task.find("self->_isr_drain_enabled ? MCP2515_LITE_ISR_DRAIN_POLL_TIMEOUT_MS : MCP2515_LITE_POLL_TIMEOUT_MS"),
      std::string::npos)
      << "the task does not pick its wait on whether the drain is live, or picks it the wrong way round - the short "
         "poll belongs to the drain, and only to it";
  EXPECT_NE(task.find("ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(poll_timeout_ms))"), std::string::npos)
      << "the chosen timeout is not what the task actually waits on";
}

TEST(Mcp2515IsrDrain, TheTaskRearmsThePinWhenItReleasesTheBus) {
  const std::string release = body_of(driver_source(), "void MCP2515_Lite::busReleaseTask()");

  const size_t clear = release.find("_isr_pin_masked = false;");
  const size_t rearm = release.find("gpio_ll_intr_enable_on_core(");
  ASSERT_NE(clear, std::string::npos) << "nothing re-arms a masked pin, so the drain stops at the first deferral";
  ASSERT_NE(rearm, std::string::npos);
  // Ordering, not presence: an interrupt that fires the instant the pin comes
  // back may have to mask again, and clearing the flag after that would throw
  // its mask away and leave the pin off with nothing to turn it on.
  EXPECT_LT(clear, rearm) << "the mask flag is cleared after the pin is re-armed - an interrupt in between masks "
                             "again and has its flag wiped, so the pin stays off";

  // The release of the bus has to come first as well, or the re-armed interrupt
  // sees the task still holding it and defers straight back.
  const size_t released = release.find("_task_wants_bus = false;");
  ASSERT_NE(released, std::string::npos);
  EXPECT_LT(released, rearm) << "the pin is re-armed while the task still claims the bus - the interrupt would defer "
                                "again and mask again";
}

/*(d) criterion (iii): which transmit buffers are free is the chip's
 * answer, not a flag history.
 *
 * The driver kept a `tx_free_mask` it maintained from the TXnIF flags. Criterion
 * (i) stops those flags reaching the pin at all, so the shadow would have been a
 * copy that nothing refreshes - and it was always only as good as the flags it
 * happened to see. READ STATUS carries all three TXREQ bits in one 2-byte
 * transaction, which is the question the mask was approximating.
 */
TEST(Mcp2515IsrDrain, TheDriverAsksTheChipWhichTransmitBuffersAreFree) {
  const std::string task = body_of(driver_source(), "void MCP2515_Lite::canTask(void* pvParameters)");

  EXPECT_NE(task.find("cmd_frame[0] = CMD_READ_STATUS;"), std::string::npos)
      << "nothing reads the chip's transmit status, so the free-buffer answer is a shadow again";
  EXPECT_NE(task.find("mcp2515_tx_free_mask(rx_frame[1])"), std::string::npos)
      << "the status byte is read but not decoded into free buffers";

  // The shadow's two writers are what must be gone: a mask that starts at "all
  // free" and one that is OR-ed back together from the interrupt flags.
  EXPECT_EQ(task.find("tx_free_mask = 0x07"), std::string::npos)
      << "the free-buffer mask is initialised from an assumption again rather than read from the chip";
  EXPECT_EQ(task.find("tx_free_mask |="), std::string::npos)
      << "the free-buffer mask is maintained from the transmit interrupt flags again - flags criterion (i) stops the "
         "chip raising";
}

/* A masked pin must come with a wake, or the re-arm waits for the
 * backstop.
 *
 * Only busReleaseTask() re-arms a masked pin, so the mask's real ceiling is
 * "when does the task next finish a transaction". The handler waking the task
 * after any mask is what makes that one scheduling latency rather than
 * MCP2515_LITE_POLL_TIMEOUT_MS: the notify wakes the task, the task's next
 * pass takes and releases the bus, and the release re-arms. ca1073e5 made the
 * wake conditional - a clean drain wakes nobody - and
 * ADrainThatFinishedItsWorkWakesNobody owns that logic; what this test pins is
 * the geometry it rests on: both masks happen before the notify point, and the
 * notify sits after the whole drain branch, where every path that set
 * wake_task can still reach it.
 */
TEST(Mcp2515IsrDrain, EveryMaskComesWithAWake) {
  const std::string handler = body_of(driver_source(), "void IRAM_ATTR MCP2515_Lite::mcp2515_isr_handler(void* arg)");

  const size_t notify = handler.find("vTaskNotifyGiveFromISR(");
  ASSERT_NE(notify, std::string::npos) << "the interrupt no longer wakes the task at all";

  size_t masks = 0;
  for (size_t at = handler.find("maskIsrPin()"); at != std::string::npos; at = handler.find("maskIsrPin()", at + 1)) {
    EXPECT_LT(at, notify) << "the pin is masked after the task was notified - nothing wakes the task for THIS mask, "
                             "so the re-arm waits for the 1000 ms backstop instead of a scheduling latency";
    masks++;
  }
  EXPECT_EQ(masks, 2u) << "the handler has " << masks
                       << " mask calls where its two could-not-drain paths need one each";

  // The notify must not sit inside the drain-enabled branch, where a fallback
  // build - or a masking path that runs before the branch ends - could never
  // reach it.
  const size_t drain_branch = handler.find("if (instance->_isr_drain_enabled)");
  ASSERT_NE(drain_branch, std::string::npos);
  const size_t branch_end = handler.find("\n    }", drain_branch);
  ASSERT_NE(branch_end, std::string::npos);
  EXPECT_GT(notify, branch_end) << "the task is only notified when the drain is enabled";
}

/* A torn-down handler must not leave a mask behind.
 *
 * detachIsrPin() removes the handler, which disables the pin on its own. A
 * stale _isr_pin_masked would make the next busReleaseTask() re-arm a pin
 * nobody handles - or, on a re-begin(), hand the fresh install a mask it never
 * took. Autodetection detaches and re-begins on every autodetect boot, so this
 * is a boot path, not a corner.
 */
TEST(Mcp2515IsrDrain, DetachingTheHandlerForgetsAPendingMask) {
  const std::string detach = body_of(driver_source(), "void MCP2515_Lite::detachIsrPin()");

  EXPECT_NE(detach.find("_isr_pin_masked = false;"), std::string::npos)
      << "detachIsrPin() keeps a pending mask - the next busReleaseTask() re-arms a pin whose handler is gone";
}

/* Autodetection times the RECEIVE interrupt now, so it must receive.
 *
 * Criterion (i) took TX0IE out of CANINTE. Before it, autodetection's timed
 * edge came from whichever raised first, and a transmit completion alone was
 * enough; after it, the only thing that can pull the pin low is a received
 * frame, and the only receiver of the test frame is the chip itself - in
 * loopback mode. Flip that flag to false and nothing fails loudly: every
 * autodetect times out at 100 ms and answers 8 MHz, on every 16 MHz board.
 */
TEST(Mcp2515IsrDrain, AutodetectionReceivesItsOwnTestFrame) {
  const std::string autodetect = body_of(driver_source(), "uint32_t MCP2515_Lite::autodetectOscillatorFrequency()");

  EXPECT_NE(autodetect.find("begin({7813, 8000000}, true, true)"), std::string::npos)
      << "autodetection does not run the chip in loopback - with CANINTE receive-only, no interrupt ever fires and "
         "the timeout answers 8 MHz regardless of the crystal";
}

/* The READ STATUS decode against the datasheet's bit numbers, all 256
 * bytes.
 *
 * The named tests above pick the bits one at a time; this pins the whole
 * layout to independent constants (TXREQ0 = 0x04, TXREQ1 = 0x10, TXREQ2 =
 * 0x40, straight from the datasheet's READ STATUS figure) so a wrong base or a
 * wrong stride cannot agree with them on any input.
 */
TEST(Mcp2515TxStatus, TheDecodeMatchesTheDatasheetOnEveryStatusByte) {
  for (int status = 0; status < 256; status++) {
    const uint8_t mask = mcp2515_tx_free_mask((uint8_t)status);
    EXPECT_EQ((mask & 0x01) != 0, (status & 0x04) == 0) << "status " << status;
    EXPECT_EQ((mask & 0x02) != 0, (status & 0x10) == 0) << "status " << status;
    EXPECT_EQ((mask & 0x04) != 0, (status & 0x40) == 0) << "status " << status;
    EXPECT_EQ(mask & ~MCP2515_TX_ALL_FREE, 0) << "status " << status << " frees a buffer the chip does not have";
  }
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
  /* Bounded to the guard's OWN block, by brace depth.
   *
   * Two earlier forms of this assertion were anchored on indentation instead.
   * The original literal carried the guard's leading spaces and broke when the
   * MCP2515 block was nested one level deeper - a change with nothing to say
   * about the ISR drain. Its replacement asked whether the offer came before
   * the next `\n    }`, and that brace is the ENCLOSING block's, not the
   * guard's: moving the offer out of the guard and leaving `if
   * (mcp2515_bus_is_exclusive()) {}` behind kept this case green. The mutation
   * that showed it is V01 in scripts/r513.mut; reading the assertion did not.
   */
  const size_t guard = src.find("if (mcp2515_bus_is_exclusive()) {");
  ASSERT_NE(guard, std::string::npos) << "the exclusivity guard is gone";
  const auto guarded = block_at(src, guard);
  ASSERT_NE(guarded.second, std::string::npos) << "the guard's block never closes";
  const size_t offer = src.find("can2515->useIsrDrain(", guard);
  ASSERT_NE(offer, std::string::npos) << "the drain is never offered";
  EXPECT_LT(offer, guarded.second) << "the drain is enabled outside the guard that asks whether the bus is "
                                      "exclusive - the guard itself may still be there, and empty";
  EXPECT_EQ(src.find("can2515->useIsrDrain(", offer + 1), std::string::npos)
      << "the drain is offered a second time, so one of the two is not answering to the guard";
  EXPECT_EQ(src.rfind("can2515->useIsrDrain(", guard), std::string::npos)
      << "the drain is also enabled before the guard, which makes the guard decorative";
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
