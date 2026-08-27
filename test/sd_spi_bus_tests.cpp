#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <string>

#include "../Software/src/devboard/utils/events.h"

/* On the T-CAN485 the SD card and the CAN add-ons must not share an SPI
 * controller.
 *
 * Two SPIClass::begin() calls on one ESP32 controller cannot coexist - the
 * controller's MISO input has exactly one source GPIO, and the second begin()
 * silently re-points it. With the SD card and the MCP2515 both on VSPI (the
 * shipped configuration this fixes), the SD mounted and then went deaf the
 * moment init_CAN() ran, with every later log write failing eventless.
 * alloc_pins() cannot catch it: the pin sets are disjoint, the collision is
 * over the controller. Measured on a T-CAN485, 15/15 boots.
 *
 * These read the source because the SDCARD-gated half of hw_lilygo.h has no
 * host build (the same reason the defect sat unseen), and because compiling
 * that header a second time with SDCARD defined only here would give the class
 * two definitions across the binary.
 *
 * The bus assignments they pin, as a truth table - each configured combination
 * lands on its own controller:
 *
 *   SD card        HSPI   (moved here by this change)
 *   MCP2515 add-on VSPI   (classic-ESP32 default, unchanged)
 *   MCP2517 add-on VSPI   (moved off HSPI by this change; shares the header PINS
 *                          with the 2515, so both being configured at once is
 *                          already refused by alloc_pins() on the identical
 *                          pin numbers - each is alone on VSPI when present)
 */

namespace {

/* Collapse every run of whitespace to one space.
 *
 * Without this the assertions below pin the LAYOUT as well as the code, and
 * they fail on a change that alters neither: splitting
 *
 *   uint8_t SD_SPI_BUS() override { return HSPI; }
 *
 * across three lines - exactly what clang-format does once that line grows past
 * the 120-column limit - turns the SD test red while reporting "the T-CAN485 SD
 * card has left HSPI", which is not true and sends the reader after a defect
 * that is not there (R255, demonstrated). At 48 columns today it has room, but
 * one rename is all it takes.
 *
 * What this does NOT buy: the search is still textual, so it would also match
 * the code written inside a comment. The real guard against that is the
 * per-board check, which reads the values rather than the file. */
std::string squeeze_whitespace(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  bool in_space = false;
  for (const char c : in) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      in_space = true;
      continue;
    }
    if (in_space && !out.empty()) {
      out.push_back(' ');
    }
    in_space = false;
    out.push_back(c);
  }
  return out;
}

std::string hw_lilygo_source() {
  // Located relative to this file rather than through a CMake define, so the
  // test needs no build-system plumbing to run.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/devboard/hal/hw_lilygo.h";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "hw_lilygo.h is where this test looks: " << path;
  return squeeze_whitespace(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

std::string sdcard_source() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/devboard/sdcard/sdcard.cpp";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "sdcard.cpp is where this test looks: " << path;
  return squeeze_whitespace(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

}  // namespace

TEST(SdSpiBus, TheSdCardIsOnHspi) {
  const std::string src = hw_lilygo_source();
  EXPECT_NE(src.find("uint8_t SD_SPI_BUS() override { return HSPI; }"), std::string::npos)
      << "the T-CAN485 SD card has left HSPI - if it is back on VSPI it shares a controller with the "
         "MCP2515 add-on, and the second begin() steals the controller's MISO: the SD mounts, goes deaf "
         "when init_CAN() runs, and every log write fails with no event";
  EXPECT_EQ(src.find("SD_SPI_BUS() override { return VSPI; }"), std::string::npos)
      << "the pre-wq255 assignment is back";
}

TEST(SdSpiBus, TheFdAddonVacatedHspi) {
  const std::string src = hw_lilygo_source();
  EXPECT_NE(src.find("uint8_t MCP2517_BUS() override { return VSPI; }"), std::string::npos)
      << "the MCP2517 add-on no longer overrides its bus to VSPI - on a classic ESP32 its default is "
         "HSPI, which now belongs to the SD card, so an FD add-on plus SD logging would re-create the "
         "exact collision wq255 fixed, one bus over";
}

/* The three tests above search normalised text, and that normalisation is the
 * only thing standing between them and a false alarm on a reformat. Pin it
 * here: the same declaration wrapped the way clang-format wraps a long line
 * must still be found, while a real change of the returned bus must not be. */
TEST(SdSpiBus, ReformattingTheDeclarationIsNotADefect) {
  const std::string wrapped =
      "  uint8_t SD_SPI_BUS() override {\n"
      "    return HSPI;\n"
      "  }\n";
  EXPECT_NE(squeeze_whitespace(wrapped).find("uint8_t SD_SPI_BUS() override { return HSPI; }"), std::string::npos)
      << "a behaviour-preserving reformat now reads as the SD card leaving HSPI";

  const std::string moved_back =
      "  uint8_t SD_SPI_BUS() override {\n"
      "    return VSPI;\n"
      "  }\n";
  EXPECT_EQ(squeeze_whitespace(moved_back).find("uint8_t SD_SPI_BUS() override { return HSPI; }"), std::string::npos)
      << "normalising whitespace must not blind the search to the bus actually changing";
}

TEST(SdSpiBus, TheMcp2515StaysOffTheSdController) {
  const std::string src = hw_lilygo_source();
  size_t at = src.find("MCP2515_BUS");
  if (at == std::string::npos) {
    // No override: the classic-ESP32 default is VSPI, which is what the truth
    // table above needs. Nothing more to hold.
    return;
  }
  EXPECT_NE(src.find("MCP2515_BUS() override { return VSPI; }"), std::string::npos)
      << "hw_lilygo.h now overrides MCP2515_BUS to something other than VSPI. If that is HSPI, the "
         "2515 shares a controller with the SD card and the wq255 defect is back. If it is a genuinely "
         "new bus assignment, re-derive the truth table at the top of this file before loosening this.";
}

/* The other half of the T-CAN485 SD story.
 *
 * The bus change above removes the collision that TRIGGERS the silence. These pin the
 * silence itself being gone, because a reader of either half alone gets the
 * wrong story: with only the bus change the card stops going deaf on this board, and
 * with only this a deaf card finally says so.
 *
 * Source-reading for the same reason the bus tests are: sdcard.cpp is behind
 * SDCARD and has no host build, so there is nothing to link against. Every
 * assertion is an ASSERT/EXPECT on a literal that exists, so a restructure
 * fails loudly rather than silently matching nothing.
 */
TEST(SdWriteFailure, TheOpenResultIsNoLongerDiscarded) {
  const std::string src = sdcard_source();

  // The single checked helper, and nothing opening a log file around it.
  EXPECT_NE(src.find("static bool open_sd_log_file(File& file, const char* path, bool& is_open)"), std::string::npos);
  EXPECT_NE(src.find("is_open = (bool)file;"), std::string::npos);

  // The shape that caused the defect: an SD.open() whose result is assumed good
  // by setting the flag true next to it. It must not come back at any site.
  EXPECT_EQ(src.find("can_log_file = SD.open(CAN_LOG_FILE, FILE_APPEND); can_file_open = true;"), std::string::npos);
  EXPECT_EQ(src.find("log_file = SD.open(LOG_FILE, FILE_APPEND); log_file_open = true;"), std::string::npos);
}

TEST(SdWriteFailure, TheWriteReturnIsCheckedOnBothPaths) {
  const std::string src = sdcard_source();

  // write() reports what it actually wrote; a short write is a failed write.
  EXPECT_NE(src.find("const size_t can_written = can_log_file.write(buffer, receivedMessageSize);"), std::string::npos);
  EXPECT_NE(src.find("if (can_written != receivedMessageSize)"), std::string::npos);
  EXPECT_NE(src.find("const size_t log_written = log_file.write(buffer, receivedMessageSize);"), std::string::npos);
  EXPECT_NE(src.find("if (log_written != receivedMessageSize)"), std::string::npos);
}

/* LATCHED - for the record of the failure, not for spam control.
 *
 * An earlier version argued latching is what stops a dead card from feeding the
 * failing log ring one line per failed write. That mechanism is wrong:
 * set_event_internal() logs only on the inactive->active EDGE, an event stays
 * active however it was set, and nothing ever clears this one - so a plain
 * set_event() would also log exactly once. The behavioural test below pins the
 * real no-spam property. What latching actually buys is clear_event()
 * immunity: the record of a failure whose evidence is MISSING data cannot be
 * cleared by code, only by the user resetting the events page - the same shape
 * as EVENT_SD_INIT_FAILED. This test pins the choice staying latched. */
TEST(SdWriteFailure, TheEventIsLatchedAndRaisedFromOneHelper) {
  const std::string src = sdcard_source();

  EXPECT_NE(src.find("static void report_sd_write_failure() { set_event_latched(EVENT_SD_WRITE_FAILED, 0); }"),
            std::string::npos);
  EXPECT_EQ(src.find("set_event(EVENT_SD_WRITE_FAILED"), std::string::npos) << "must be latched, not plain";
}

/* WARNING, not ERROR: update_bms_status() turns any active error into
 * system_status = FAULT, and an optional log dying is not a reason to fault a
 * running emulator. Its sibling EVENT_SD_INIT_FAILED is WARNING for the same
 * reason, so this also pins the two staying consistent. */
TEST(SdWriteFailure, TheEventIsAWarningLikeItsMountSibling) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  std::ifstream ev(dir + "/../Software/src/devboard/utils/events.cpp");
  ASSERT_TRUE(ev.is_open());
  const std::string src =
      squeeze_whitespace(std::string((std::istreambuf_iterator<char>(ev)), std::istreambuf_iterator<char>()));

  EXPECT_NE(src.find("events.entries[EVENT_SD_WRITE_FAILED].level = EVENT_LEVEL_WARNING;"), std::string::npos);
  EXPECT_NE(src.find("events.entries[EVENT_SD_INIT_FAILED].level = EVENT_LEVEL_WARNING;"), std::string::npos);
  EXPECT_EQ(src.find("events.entries[EVENT_SD_WRITE_FAILED].level = EVENT_LEVEL_ERROR;"), std::string::npos);
}

/* A failed open or a short write stops through the pause machinery that already
 * exists, so the writer does not retry every pass against a card that is not
 * coming back, and the web UI's resume is what re-opens it. */
TEST(SdWriteFailure, FailureStopsThroughTheExistingPauseMachinery) {
  const std::string src = sdcard_source();

  EXPECT_NE(src.find("can_logging_paused = true; vRingbufferReturnItem(can_bufferHandle, (void*)buffer); return;"),
            std::string::npos);
  EXPECT_NE(src.find("logging_paused = true; vRingbufferReturnItem(log_bufferHandle, (void*)buffer); return;"),
            std::string::npos);
}

/* The two properties the latched argument was really about, pinned against the
 * event machinery itself rather than against a comment - events.cpp IS in the
 * host build, so these are behavioural.
 *
 * First: a repeated failure while the event is active takes the already-active
 * path - counted, but not re-announced. This is what actually keeps a dead
 * card from feeding the failing log ring, and it holds for latched and plain
 * events alike: the guard is the inactive->active edge, not the latch. The
 * MQTTpublished flag is reset only on that edge, so it observes the edge. */
TEST(SdWriteFailure, ARepeatedFailureIsCountedButNotReAnnounced) {
  reset_all_events();
  set_event_latched(EVENT_SD_WRITE_FAILED, 0);
  const EVENTS_STRUCT_TYPE* e = get_event_pointer(EVENT_SD_WRITE_FAILED);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->occurences, 1);
  EXPECT_FALSE(e->MQTTpublished) << "the first failure is a fresh announcement";

  set_event_MQTTpublished(EVENT_SD_WRITE_FAILED);
  set_event_latched(EVENT_SD_WRITE_FAILED, 0);
  EXPECT_EQ(e->occurences, 2) << "the second failure must still be counted";
  EXPECT_TRUE(e->MQTTpublished) << "an active event re-set must take the already-active path - "
                                   "re-announcing here is one log line into the failing ring per failed write";
  reset_all_events();
}

/* Second: what latched actually buys. clear_event() clears a plain active
 * event and must NOT clear this one - the failure's evidence is data that is
 * MISSING from the log, so the record has to outlive any code path that
 * tidies events; only the user's events-page reset may clear it. */
TEST(SdWriteFailure, TheFailureRecordSurvivesClearEvent) {
  reset_all_events();
  set_event_latched(EVENT_SD_WRITE_FAILED, 0);
  const EVENTS_STRUCT_TYPE* e = get_event_pointer(EVENT_SD_WRITE_FAILED);
  ASSERT_NE(e, nullptr);
  clear_event(EVENT_SD_WRITE_FAILED);
  EXPECT_EQ(e->state, EVENT_STATE_ACTIVE_LATCHED)
      << "clear_event() cleared a latched event - the failure record no longer outlives code-side tidying";
  reset_all_events();
}
