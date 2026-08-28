#include <gtest/gtest.h>

#include <cstring>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/webserver/debug_logging_html.h"

// Pins the /log page's empty-state condition (offset == 0 && buf[0] == 0)
// at the rendering level - the derivation that a CLEARED buffer (the CAN pages
// reset offset and buf[0] but leave stale tail bytes) shows the explanation
// ALONE, and a live buffer never shows it, was source-reasoned in the review;
// this makes it a regression test.
// The page wraps itself in the shared chrome; the test needs symbols, not markup.
extern const char index_html_header[] = "";
extern const char index_html_footer[] = "";

namespace {

void reset_log_buffer() {
  memset(datalayer.system.info.logged_can_messages, 0, sizeof(datalayer.system.info.logged_can_messages));
  datalayer.system.info.logged_can_messages_offset = 0;
}

}  // namespace

TEST(DebugLogPageTest, ClearedBufferWithStaleTailShowsOnlyTheExplanation) {
  reset_log_buffer();
  datalayer.system.info.web_logging_active = false;
  datalayer.system.info.can_logging_active = false;
  // The CAN logging/replay pages clear by resetting offset and buf[0] ONLY -
  // stale text beyond the head survives. The page must not render it.
  // Layout matters: the tail-renderer skips to the first newline after the
  // head and renders from THERE, so the stale text must sit after one - the
  // shape a genuinely wrapped log leaves behind.
  const char* stale = "tail\nSTALE LINE THAT MUST NOT RENDER\n";
  memcpy(datalayer.system.info.logged_can_messages + 100, stale, strlen(stale));
  datalayer.system.info.logged_can_messages_offset = 0;
  datalayer.system.info.logged_can_messages[0] = '\0';

  String page = debug_logger_processor();

  EXPECT_NE(page.indexOf("Web logging is off"), -1) << "the empty state must be explained";
  EXPECT_EQ(page.indexOf("STALE LINE"), -1) << "stale tail bytes rendered next to the explanation";
}

TEST(DebugLogPageTest, LiveBufferShowsLogsAndNoExplanation) {
  reset_log_buffer();
  datalayer.system.info.web_logging_active = true;
  datalayer.system.info.can_logging_active = false;
  const char* line = "  123.456 a real debug line\n";
  memcpy(datalayer.system.info.logged_can_messages, line, strlen(line));
  datalayer.system.info.logged_can_messages_offset = strlen(line);

  String page = debug_logger_processor();

  EXPECT_NE(page.indexOf("a real debug line"), -1);
  EXPECT_EQ(page.indexOf("Web logging is off"), -1);
  EXPECT_EQ(page.indexOf("nothing has been logged"), -1);
}
