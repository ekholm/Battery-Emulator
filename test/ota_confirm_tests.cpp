#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Arduino.h>

#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"
#include "../Software/src/devboard/utils/ota_confirm_gate.h"

/* A fresh image is confirmed by a RUNNING system, and every steady state
 * this firmware can be in has to reach that confirmation.
 *
 * Once verifyRollbackLater() defers the mark to us, not confirming is
 * not "nothing happens" - the bootloader undoes the update on the next reset.
 * So the dangerous shape is a normal operating state that never gets to the
 * mark, and one already exists: on the wizard branches setup() returns early,
 * above the line that starts the core tick. A board in wizard mode is a fresh
 * or factory-reset board, which is exactly the one being rebooted.
 *
 * Two halves below. The first exercises the gate. The second reads
 * Software.cpp, because Software.cpp is not in this binary - which is also why
 * a mark placed there could sit wrong without any test noticing.
 */

namespace {

/* Strip comments out, before anything is asserted about the code.
 *
 * Every test below decides whether the firmware does something by looking for
 * the text of a call, and this file is surrounded by comments that NAME those
 * calls - the placement comment in core_loop says `ota_confirm_check`, the one
 * in setup() explains why the mark is gone. Commenting a call out then leaves
 * its name in place and the assertion passes on dead code, which is exactly
 * how an earlier mutation harness certified a mutation it never ran. Newlines
 * are kept so brace depth and ordering still mean what they meant.
 */
std::string strip_comments(const std::string& src) {
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

std::string read_source(const std::string& relative_to_test_dir) {
  // Located relative to this file rather than through a CMake define, so the
  // test needs no build-system plumbing to run.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/" + relative_to_test_dir;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

// The body of a function, by brace depth from its signature line.
std::string function_body(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature);
  EXPECT_NE(at, std::string::npos) << "no `" << signature << "` in the source this test reads";
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

// The `{ ... }` block that follows `at`, by brace depth. Used to ask whether a
// call sits INSIDE a particular guard rather than merely somewhere near it.
std::string brace_block_at(const std::string& src, size_t at) {
  const size_t open = src.find('{', at);
  if (open == std::string::npos) {
    return "";
  }
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

// The text between the parentheses of the first `call` in `src`, trimmed.
std::string call_argument(const std::string& src, const std::string& call) {
  const size_t at = src.find(call);
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = at + call.size();
  const size_t close = src.find(')', open);
  if (close == std::string::npos) {
    return "";
  }
  std::string arg = src.substr(open, close - open);
  const size_t first = arg.find_first_not_of(" \t\n");
  const size_t last = arg.find_last_not_of(" \t\n");
  return first == std::string::npos ? "" : arg.substr(first, last - first + 1);
}

// A bare C identifier - no literal, no arithmetic, no call.
bool is_identifier(const std::string& s) {
  if (s.empty() || (!isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')) {
    return false;
  }
  for (const char c : s) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_') {
      return false;
    }
  }
  return true;
}

/* The widest a format string can render ON THE TARGET, in wire characters.
 *
 * Shared by the two budget tests below. %s here is always a firmware version -
 * esp_app_desc_t::version is a char[32] and BUILD_VERSION is what gets written
 * into one - so 31 characters. unsigned long is 32 bits on every ESP32 variant
 * this firmware builds for, so %lu is at most 4294967295: ten digits, not the
 * twenty a 64-bit host would need. Anything else is a failure rather than a
 * guess, because a width table that silently defaults is a test that passes for
 * the wrong reason.
 */
size_t widest_rendering(const std::string& fmt) {
  size_t worst = fmt.size() - 1;  // the literal "\n" is two characters in the source, one on the wire
  for (size_t c = fmt.find('%'); c != std::string::npos; c = fmt.find('%', c + 1)) {
    if (fmt.compare(c, 2, "%s") == 0) {
      worst += 31 - 2;
    } else if (fmt.compare(c, 3, "%lu") == 0) {
      worst += 10 - 3;
    } else {
      ADD_FAILURE() << "add the widest rendering of " << fmt.substr(c, 4) << " before this line can be judged";
    }
  }
  return worst;
}

// MAX_LINE_LENGTH_PRINTF, read out of logging.cpp rather than repeated here, so
// raising the logger's buffer relaxes both budget tests by itself.
size_t logger_line_budget(const std::string& logger) {
  const size_t def = logger.find("#define MAX_LINE_LENGTH_PRINTF");
  EXPECT_NE(def, std::string::npos) << "logging.cpp no longer names its own line budget";
  if (def == std::string::npos) {
    return 0;
  }
  return static_cast<size_t>(std::stoul(logger.substr(def + 30)));
}

/* The adjacent string literals that make up a call's format, starting at the
 * call's open parenthesis, plus everything that follows them up to the matching
 * close parenthesis (the arguments).
 *
 * __DATE__ and __TIME__ are spliced in because the preprocessor concatenates
 * them into the format, and the splice is what makes the build banner
 * MEASURABLE rather than what keeps it short. A scanner that stopped at the
 * first non-literal token would end that format at `build `, find a token where
 * a comma or a closing parenthesis belongs, and report the call as unreadable -
 * so the line would not be under-counted, it would not be judged at all. The
 * two macros and the separator between them are worth 22 characters, and the
 * banner budgets 76 against a 128 byte buffer either way; it is the reading that
 * needs them, not the room. Any OTHER token appearing where a literal is
 * expected is reported, for the same reason the width table refuses to guess.
 */
struct LogCall {
  std::string format;
  std::string arguments;     // everything after the format, to the closing parenthesis
  bool format_is_literal{};  // false for a runtime format (a function returning const char*)
};
LogCall parse_log_call(const std::string& src, size_t open_paren) {
  LogCall call;

  // The parenthesis that closes the call. String literals are skipped whole so
  // a bracket inside one cannot unbalance the count.
  int depth = 1;
  size_t end = open_paren + 1;
  for (; end < src.size() && depth > 0; ++end) {
    if (src[end] == '"') {
      end = src.find('"', end + 1);
      if (end == std::string::npos) {
        ADD_FAILURE() << "unterminated string in a log call";
        return call;
      }
    } else if (src[end] == '(') {
      ++depth;
    } else if (src[end] == ')') {
      --depth;
    }
  }
  if (depth != 0) {
    ADD_FAILURE() << "a log call runs off the end of the file";
    return call;
  }
  --end;  // the closing parenthesis itself

  size_t i = src.find_first_not_of(" \t\n", open_paren + 1);
  while (i != std::string::npos && i < end) {
    if (src[i] == '"') {
      const size_t close = src.find('"', i + 1);
      if (close == std::string::npos) {
        ADD_FAILURE() << "unterminated format string";
        return call;
      }
      call.format += src.substr(i + 1, close - i - 1);
      i = src.find_first_not_of(" \t\n", close + 1);
      continue;
    }
    if (src.compare(i, 8, "__DATE__") == 0) {
      call.format += "Mmm dd yyyy";  // 11 characters, and never a conversion
      i = src.find_first_not_of(" \t\n", i + 8);
      continue;
    }
    if (src.compare(i, 8, "__TIME__") == 0) {
      call.format += "hh:mm:ss";  // 8
      i = src.find_first_not_of(" \t\n", i + 8);
      continue;
    }
    break;
  }
  call.format_is_literal = i != std::string::npos && (i >= end || src[i] == ',' || src[i] == ')');
  const size_t args_from = call.format_is_literal ? std::min(i, end) : open_paren + 1;
  call.arguments = src.substr(args_from, end - args_from);
  return call;
}

// Every .cpp and .h under Software/, except the vendored trees in src/lib,
// whose content is not ours to edit.
std::vector<std::filesystem::path> firmware_sources() {
  const std::string self = __FILE__;
  const std::filesystem::path root = std::filesystem::path(self.substr(0, self.find_last_of('/'))) / ".." / "Software";
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    const std::string path = entry.path().string();
    if (path.find("/src/lib/") != std::string::npos) {
      continue;
    }
    /* logging.h DEFINES these names - the macro bodies and the host mock both
     * spell `printf(fmt, ...)`, which is a forwarding signature and not a call
     * with a format anyone can measure. Every other file that spells them is a
     * caller, and one that cannot be parsed there is reported rather than
     * skipped. */
    if (path.size() >= 10 && path.compare(path.size() - 10, 10, "/logging.h") == 0) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (entry.is_regular_file() && (ext == ".cpp" || ext == ".h")) {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

class OtaConfirmGate : public ::testing::Test {
 protected:
  void SetUp() override { ota_confirm_gate_reset(); }
  void TearDown() override { ota_confirm_gate_reset(); }
};

}  // namespace

/* The boundary, stated as the ruling states it: nothing is owed at
 * 42 * 1000 - 1 ms, and the confirmation is owed at 42 * 1000 exactly. The
 * constant is the interesting part of the change, so it is pinned rather than
 * left to be read off a comment.
 */
TEST_F(OtaConfirmGate, NothingIsOwedOneMillisecondShortOfTheWindow) {
  EXPECT_EQ(OTA_CONFIRM_UPTIME_MS, 42u * 1000u);

  ota_confirm_check(0);
  ota_confirm_check(OTA_CONFIRM_UPTIME_MS - 1);
  EXPECT_FALSE(ota_confirm_take_pending()) << "an image was confirmed before it had survived the window";
}

TEST_F(OtaConfirmGate, TheConfirmationIsOwedAtTheWindowExactly) {
  ota_confirm_check(OTA_CONFIRM_UPTIME_MS);
  EXPECT_TRUE(ota_confirm_take_pending());
}

/* The write happens once per boot. The check runs on every 1 ms tick and the
 * take runs on every pass of loop(), so "once" is the property that keeps this
 * from becoming a flash write per millisecond for the rest of the board's
 * uptime.
 */
TEST_F(OtaConfirmGate, TheConfirmationIsTakenExactlyOnce) {
  for (uint32_t t = OTA_CONFIRM_UPTIME_MS; t < OTA_CONFIRM_UPTIME_MS + 10; ++t) {
    ota_confirm_check(t);
  }
  EXPECT_TRUE(ota_confirm_take_pending());

  for (uint32_t t = OTA_CONFIRM_UPTIME_MS + 10; t < OTA_CONFIRM_UPTIME_MS + 20; ++t) {
    ota_confirm_check(t);
    EXPECT_FALSE(ota_confirm_take_pending()) << "a second confirmation was owed at uptime " << t;
  }
}

/* A deliberate restart inside the window keeps the update. Without this, the
 * most ordinary thing a user does after updating - press restart - is what
 * reverts it.
 */
TEST_F(OtaConfirmGate, AnIntentionalRestartConfirmsBeforeTheWindowElapses) {
  ota_confirm_check(1000);
  ASSERT_FALSE(ota_confirm_take_pending());

  ota_confirm_request();
  EXPECT_TRUE(ota_confirm_take_pending());
}

/* ...and once confirmed, a restart does not arm a second write. */
TEST_F(OtaConfirmGate, AnIntentionalRestartAfterConfirmationOwesNothing) {
  ota_confirm_check(OTA_CONFIRM_UPTIME_MS);
  ASSERT_TRUE(ota_confirm_take_pending());

  ota_confirm_request();
  EXPECT_FALSE(ota_confirm_take_pending());
}

/* The read the restart deadline makes. It is a new public function and the
 * predicate tests cannot see it: they are handed `confirm_owed` as an argument,
 * so a gate that answered "nothing owed" to everyone would leave every one of
 * them green with the fix entirely dead.
 */
TEST_F(OtaConfirmGate, PendingSaysWhetherTheWriteIsStillOwed) {
  EXPECT_FALSE(ota_confirm_pending()) << "a confirmation was owed before anything asked for one";

  ota_confirm_request();
  EXPECT_TRUE(ota_confirm_pending()) << "the write is armed and unserviced - this is the deferral's whole input";

  ASSERT_TRUE(ota_confirm_take_pending());
  EXPECT_FALSE(ota_confirm_pending()) << "the write has been taken, so the deferred restart must be released";
}

/* graceful_restart() is the one restart path in the tree (#2551), so it is the
 * one place this has to be wired. Testing it through safety.cpp rather than by
 * reading the source, because safety.cpp IS in this binary.
 */
TEST_F(OtaConfirmGate, GracefulRestartArmsTheConfirmation) {
  reset_all_events();
  ota_confirm_check(1000);
  ASSERT_FALSE(ota_confirm_take_pending());

  graceful_restart();
  EXPECT_TRUE(ota_confirm_take_pending()) << "a requested restart inside the window would revert the update";
}

/* WHERE the confirmation lands (measured on the bench by the OTA-then-revert
 * acceptance run; the target-aware write side is the fix).
 *
 * esp_ota_mark_app_valid_cancel_rollback() writes the ACTIVE otadata entry,
 * and esp_ota_set_boot_partition() has made that the TARGET slot's before
 * graceful_restart() arms the gate - on a revert and on a finished OTA alike.
 * The verdict is the pure half of the fix: pending AND still selected, or no
 * write. Pinned per outcome, because a version that ignores the second flag
 * confirms an image that has never run.
 */
TEST(OtaConfirmVerdictTest, AnOrdinaryBootHasNothingToConfirm) {
  EXPECT_EQ(ota_confirm_verdict(false, true), OtaConfirmVerdict::NOT_PENDING);
  EXPECT_EQ(ota_confirm_verdict(false, false), OtaConfirmVerdict::NOT_PENDING)
      << "not pending is not pending, whichever slot is selected - a revert from a confirmed image owes nothing";
}

TEST(OtaConfirmVerdictTest, APendingImageStillSelectedIsConfirmed) {
  EXPECT_EQ(ota_confirm_verdict(true, true), OtaConfirmVerdict::CONFIRM);
}

TEST(OtaConfirmVerdictTest, APendingImageWhoseBootSelectionMovedIsNotConfirmedOnBehalfOfTheTarget) {
  EXPECT_EQ(ota_confirm_verdict(true, false), OtaConfirmVerdict::BOOT_SELECTION_MOVED)
      << "the write would land on the slot being booted next, which has not run - the bench saw it arrive VALID";
}

/* ---- the source-readable half ---- */

/* The mark is in runtime cadence, not in setup(). Two things would be wrong if
 * it went back: an image confirmed at the end of setup() has survived driver
 * construction and nothing else, and the previous placement sat immediately
 * after the core_loop task was created, so whether the first loop pass was
 * covered was decided by a race of microseconds.
 */
TEST(OtaConfirmPlacement, SetupDoesNotConfirmTheImage) {
  const std::string setup = function_body(read_source("../Software/Software.cpp"), "void setup() {");
  ASSERT_FALSE(setup.empty());

  EXPECT_EQ(setup.find("mark_ota_image_valid"), std::string::npos)
      << "setup() confirms the image again - reaching the end of setup() is not evidence the image works";
  EXPECT_EQ(setup.find("ota_confirm_service"), std::string::npos)
      << "setup() performs the confirmation write - it belongs in runtime cadence";
}

/* The check has to be unconditional inside the core tick. Inside the 10 ms or
 * 1 s sub-task it would still fire, but it would then be witnessing that ONE
 * sub-task ran, which is a weaker statement than the tick itself coming round.
 */
TEST(OtaConfirmPlacement, TheCoreTickChecksUnconditionally) {
  const std::string core_loop = function_body(read_source("../Software/Software.cpp"), "void core_loop(void*) {");
  ASSERT_FALSE(core_loop.empty());
  ASSERT_NE(core_loop.find("ota_confirm_check("), std::string::npos)
      << "the core tick no longer checks - in normal mode nothing would ever confirm the image";

  // Depth 1 is the while-loop body; anything deeper is inside one of the
  // conditional sub-tasks.
  int depth = 0;
  int depth_at_check = -1;
  for (size_t i = 0; i < core_loop.size(); ++i) {
    if (core_loop[i] == '{') {
      ++depth;
    } else if (core_loop[i] == '}') {
      --depth;
    } else if (core_loop.compare(i, 18, "ota_confirm_check(") == 0) {
      depth_at_check = depth;
    }
  }
  EXPECT_EQ(depth_at_check, 2) << "the confirmation check sits inside a conditional in core_loop - it must run on "
                                  "every tick, because the tick coming round is the thing it is measuring";
}

/* The write side is reached from ordinary main-task context. */
TEST(OtaConfirmPlacement, LoopPerformsTheConfirmationWrite) {
  const std::string loop = function_body(read_source("../Software/Software.cpp"), "void loop() {");
  EXPECT_NE(loop.find("ota_confirm_service("), std::string::npos)
      << "loop() no longer services the gate - the check would set a flag nobody acts on";
}

/* THE COMPOSITION GUARD, and the reason this item exists.
 *
 * `setup()` returning early means the core tick is never started, so the whole
 * of the normal-mode confirmation path is skipped. On the wizard branches that
 * is exactly what happens, and wizard mode is a normal operating state - its
 * own comment calls it "a strict PREFIX of a normal boot".
 *
 * So: any early exit from setup() has to be matched by a steady state that
 * checks for itself. connectivity_loop() is the loop that keeps running when
 * setup() stops early - it is the task that serves the wizard UI - so that is
 * where the second check belongs.
 *
 * On a tree whose setup() runs straight through, this asserts nothing. It goes
 * red the moment an early return is added without the matching check, which is
 * the merge these two lanes must not make unnoticed.
 */
TEST(OtaConfirmPlacement, AnEarlyReturnFromSetupBringsItsOwnCheck) {
  const std::string src = read_source("../Software/Software.cpp");
  const std::string setup = function_body(src, "void setup() {");
  ASSERT_FALSE(setup.empty());

  if (setup.find("return;") == std::string::npos) {
    GTEST_SKIP() << "setup() has no early exit on this branch - nothing to compose with yet";
  }

  const std::string service = function_body(src, "void connectivity_loop(void*) {");
  ASSERT_FALSE(service.empty());
  ASSERT_NE(service.find("ota_confirm_check("), std::string::npos)
      << "setup() can now return before the core tick is started, and the loop that keeps running when it does "
         "never confirms the image. A board that boots into that state has a good update reverted on its next "
         "reset, repeatedly.";

  /* And it has to be GATED on wizard mode. Presence alone was what this
     asserted, and an ungated check passes that while quietly weakening the
     normal boot: connectivity_loop runs there too, so whichever of the two
     loops reaches 42 s first would confirm, and the witness degrades from "the
     core tick has been coming round" to "networking survived". Both are true
     of a healthy board; only the first is true of a board whose core tick has
     stopped. */
  const size_t gate = service.find("if (wizard_mode_active()) {");
  ASSERT_NE(gate, std::string::npos) << "the wizard-mode check in connectivity_loop is not gated on "
                                        "wizard_mode_active() - in a normal boot it would confirm on the "
                                        "networking task's liveness instead of the core tick's";
  EXPECT_NE(brace_block_at(service, gate).find("ota_confirm_check("), std::string::npos)
      << "connectivity_loop checks outside the wizard_mode_active() gate - see above";
}

/* WHAT the core tick passes to the check, not just that it calls it.
 *
 * The placement tests above pin that a call exists and that it is
 * unconditional, and both survive `ota_confirm_check(0)` and
 * `ota_confirm_check(currentMillis - previousMillis10ms)` - a plausible
 * copy-paste from the two lines under it. Either one compiles, keeps every
 * other test green, and means the gate never fires in normal mode: nothing
 * confirms, and the bootloader reverts a good update on every reset. That is
 * the whole failure this item exists to prevent, so the argument is pinned to
 * absolute uptime the same way the constant is.
 */
TEST(OtaConfirmPlacement, TheCoreTickChecksAgainstAbsoluteUptime) {
  const std::string core_loop = function_body(read_source("../Software/Software.cpp"), "void core_loop(void*) {");
  ASSERT_FALSE(core_loop.empty());

  const std::string argument = call_argument(core_loop, "ota_confirm_check(");
  ASSERT_FALSE(argument.empty()) << "no ota_confirm_check(...) call in core_loop";

  EXPECT_TRUE(is_identifier(argument)) << "the core tick passes `" << argument
                                       << "` to ota_confirm_check. It takes absolute uptime, not a literal and not "
                                          "an interval - an elapsed-time expression never reaches 42 s and nothing "
                                          "would ever confirm";
  EXPECT_NE(core_loop.find(argument + " = millis();"), std::string::npos)
      << "`" << argument << "` is passed as the uptime but is not assigned from millis() in core_loop";
}

/* The write side actually writes.
 *
 * LoopPerformsTheConfirmationWrite pins that loop() CALLS ota_confirm_service();
 * nothing pins what that function does, and it lives in ota_rollback.cpp, which
 * has no host build - the file the change's own note calls out as the one where
 * a mistake "could sit unnoticed". Gutting the body leaves all 216 tests green.
 * Read it, then, the same way Software.cpp is read.
 */
TEST(OtaConfirmPlacement, TheServiceWritesExactlyWhenTheGateSaysSo) {
  const std::string service =
      function_body(read_source("../Software/src/devboard/utils/ota_rollback.cpp"), "void ota_confirm_service(void) {");
  ASSERT_FALSE(service.empty());

  const size_t gate = service.find("if (ota_confirm_take_pending()) {");
  ASSERT_NE(gate, std::string::npos) << "ota_confirm_service() no longer performs the write on exactly the pass the "
                                        "gate hands it - the check would set a flag nobody acts on";
  EXPECT_NE(brace_block_at(service, gate).find("mark_ota_image_valid()"), std::string::npos)
      << "the gate is taken but the image is never marked valid - every update would be reverted on the next reset";
}

/* The restart deadline's interplay with an unserviced confirmation.
 *
 * graceful_restart() arms both a confirmation request and a restart deadline;
 * the confirm WRITE runs in the lowest-priority task on the same core as the
 * tick that fires the deadline, so a starved main task used to lose the race
 * and the bootloader reverted a good update. restart_may_fire() is that whole
 * policy as one pure predicate, tested here the way the 42 s boundary is.
 */
TEST(RestartConfirmRace, TheNormalPathIsUnchanged) {
  // Nothing owed: the historical behaviour to the millisecond - fire strictly
  // after 5 s when paused, strictly after 10 s regardless.
  EXPECT_FALSE(restart_may_fire(RESTART_PAUSED_DEADLINE_MS, true, false)) << "5 s exactly is not yet the deadline";
  EXPECT_TRUE(restart_may_fire(RESTART_PAUSED_DEADLINE_MS + 1, true, false));
  EXPECT_FALSE(restart_may_fire(RESTART_HARD_DEADLINE_MS, false, false)) << "10 s exactly is not yet the deadline";
  EXPECT_TRUE(restart_may_fire(RESTART_HARD_DEADLINE_MS + 1, false, false));
  EXPECT_FALSE(restart_may_fire(RESTART_HARD_DEADLINE_MS - 1, false, false)) << "not paused, under 10 s: wait";
}

TEST(RestartConfirmRace, AnOwedConfirmDefersTheRestartOnBothDeadlinePaths) {
  // The race itself: deadline reached, write not yet serviced -> hold the
  // restart. Both the paused 5 s path and the hard 10 s path defer, because
  // the 5 s path is the COMMON one - graceful_restart() commands the pause and
  // an idle board reaches PAUSED almost immediately.
  EXPECT_FALSE(restart_may_fire(RESTART_PAUSED_DEADLINE_MS + 1, true, true))
      << "the paused-path deadline fired with the confirm still owed - the 5 s window is the one "
         "an idle board actually hits";
  EXPECT_FALSE(restart_may_fire(RESTART_HARD_DEADLINE_MS + 1, false, true));
  EXPECT_FALSE(restart_may_fire(RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS, true, true))
      << "still inside the grace: the main task keeps its chance to write";
}

TEST(RestartConfirmRace, TheMomentTheConfirmIsServicedTheRestartFires) {
  const uint32_t mid_grace = RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS / 2;
  EXPECT_FALSE(restart_may_fire(mid_grace, true, true));
  EXPECT_TRUE(restart_may_fire(mid_grace, true, false))
      << "the write landed (owed cleared) and the deferred restart must now go through on the next tick";
}

TEST(RestartConfirmRace, AWedgedMainTaskStillRestartsAndTheRollbackIsHonest) {
  // The cap. A main task that could not run once in twenty further seconds
  // while charge/discharge sat paused is wedged; deferring forever would trade
  // a wrong rollback for a board that never restarts. Past the cap the restart
  // fires with the image unconfirmed, and that rollback is the truthful one.
  const uint32_t cap = RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS;
  EXPECT_FALSE(restart_may_fire(cap, false, true)) << "the boundary itself still defers";
  EXPECT_TRUE(restart_may_fire(cap + 1, false, true)) << "one past the cap: restart, and roll back honestly";
  // And on the paused path, where the cap is the same instant and so the grace
  // is 25 s rather than 20 - the board sits with its contactors open for the
  // whole of it either way, which is what the one bound is protecting.
  EXPECT_FALSE(restart_may_fire(cap, true, true)) << "the cap is on elapsed time, not on the deferral";
  EXPECT_TRUE(restart_may_fire(cap + 1, true, true)) << "a paused board with a wedged main task must still restart";
}

TEST(RestartConfirmRace, TheTickCallsThePredicateAndPassesTheGate) {
  /* The SHAPE of the wiring: that the tick passes the pause state and the gate
   * to the predicate rather than re-deriving either. The predicate being right
   * proves nothing if update_restart_progress() never asks it - but unlike
   * loop(), that is also testable by driving it, and RestartDeadlineOnTheTick
   * below does exactly that. This stays as the cheap guard on the argument
   * list; it is no longer the only one. */
  const std::string body =
      function_body(read_source("../Software/src/devboard/safety/safety.cpp"), "void update_restart_progress() {");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("restart_may_fire(elapsed, emulator_pause_status == PAUSED, ota_confirm_pending())"),
            std::string::npos)
      << "update_restart_progress() no longer consults the race predicate - the deadline can again "
         "fire with the confirm unserviced";
  EXPECT_EQ(body.find("INTERVAL_5_S"), std::string::npos)
      << "a hand-written deadline is back beside the predicate - the policy must have one home";
}

/* The tick's own behaviour, and not only the predicate's.
 *
 * TheTickCallsThePredicateAndPassesTheGate above pins the CALL by reading
 * safety.cpp as text. That was justified there by "safety.cpp is not in the
 * host build", which is not so - test/CMakeLists.txt links it, which is exactly
 * why GracefulRestartArmsTheConfirmation can call graceful_restart() directly.
 * So the wiring can be tested by DRIVING it, and these do: emulated time, the
 * real update_restart_progress(), and ESP.restart() counted rather than
 * scanned for. The scan is kept as a cheap second guard on the shape of the
 * call, but it is no longer the only thing standing between the fix and a
 * silent no-op.
 */
class RestartDeadlineOnTheTick : public ::testing::Test {
 protected:
  // Far enough from zero that graceful_restart()'s `now > 0 ? now : 1` guard is
  // not the thing under test.
  static const uint32_t kArmedAt = 100000;

  void SetUp() override {
    ota_confirm_gate_reset();
    reset_all_events();
    ESP.clear_restarts();
    set_millis64(kArmedAt);
    emulator_pause_status = NORMAL;
  }

  void TearDown() override {
    ota_confirm_gate_reset();
    emulator_pause_status = NORMAL;
    set_millis64(0);
  }

  /* Arm a graceful restart INSIDE the confirmation window, on the paused path
     the RED bench run actually took: graceful_restart() commands the pause, and
     an idle board reaches PAUSED almost at once. */
  void arm_restart_inside_the_window() {
    ota_confirm_check(1000);  // well short of the window: nothing owed yet
    ASSERT_FALSE(ota_confirm_pending());
    graceful_restart();
    ASSERT_TRUE(ota_confirm_pending()) << "the restart did not arm a confirmation - nothing below means anything";
    emulator_pause_status = PAUSED;
  }

  void tick_at_elapsed(uint32_t elapsed_ms) {
    set_millis64(kArmedAt + elapsed_ms);
    update_restart_progress();
  }
};

TEST_F(RestartDeadlineOnTheTick, AnUnservicedConfirmHoldsTheRestartPastBothDeadlines) {
  arm_restart_inside_the_window();

  tick_at_elapsed(RESTART_PAUSED_DEADLINE_MS + 1);
  EXPECT_EQ(ESP.restarts_requested(), 0u) << "the paused deadline restarted the board with the image unconfirmed - "
                                             "this is the wild rollback, and it is the one the fix exists for";
  tick_at_elapsed(RESTART_HARD_DEADLINE_MS + 1);
  EXPECT_EQ(ESP.restarts_requested(), 0u) << "the hard deadline fired with the write still owed";
  tick_at_elapsed(RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS);
  EXPECT_EQ(ESP.restarts_requested(), 0u) << "the cap is not reached yet - the main task still has its chance";
}

TEST_F(RestartDeadlineOnTheTick, TheWriteReleasesTheDeferredRestart) {
  arm_restart_inside_the_window();

  tick_at_elapsed(RESTART_PAUSED_DEADLINE_MS + 1000);
  ASSERT_EQ(ESP.restarts_requested(), 0u);

  // What loop() does: take the gate, perform the write. On silicon this landed
  // 24 ms after the stall ended.
  ASSERT_TRUE(ota_confirm_take_pending());

  tick_at_elapsed(RESTART_PAUSED_DEADLINE_MS + 1001);
  EXPECT_EQ(ESP.restarts_requested(), 1u) << "the confirmation is written and the restart is still being held";
}

TEST_F(RestartDeadlineOnTheTick, AWedgedMainTaskStillRestartsOnThePausedPathToo) {
  /* The cap, on the path the board actually takes. The predicate's own cap test
     asks it unpaused; a deferral written as `confirm_owed && (paused || within
     the cap)` would pass every other test in this file and leave a paused board
     with a wedged main task never restarting at all - contactors open, waiting
     for a write that is never coming. */
  arm_restart_inside_the_window();

  tick_at_elapsed(RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS);
  ASSERT_EQ(ESP.restarts_requested(), 0u);

  tick_at_elapsed(RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS + 1);
  EXPECT_EQ(ESP.restarts_requested(), 1u) << "past the cap the restart must fire even paused, and the rollback that "
                                             "follows is the honest one";
}

TEST_F(RestartDeadlineOnTheTick, WithNothingOwedTheDeadlineIsTheOldOneToTheMillisecond) {
  /* The other half of "the normal path is unchanged": once the gate has latched
     - which on a healthy board is the first loop() pass of the boot - no
     restart inherits the grace. */
  ota_confirm_check(OTA_CONFIRM_UPTIME_MS);
  ASSERT_TRUE(ota_confirm_take_pending());
  graceful_restart();
  ASSERT_FALSE(ota_confirm_pending()) << "a second confirmation was owed after the gate latched";
  emulator_pause_status = PAUSED;

  tick_at_elapsed(RESTART_PAUSED_DEADLINE_MS);
  EXPECT_EQ(ESP.restarts_requested(), 0u) << "5 s exactly is not yet the deadline";
  tick_at_elapsed(RESTART_PAUSED_DEADLINE_MS + 1);
  EXPECT_EQ(ESP.restarts_requested(), 1u) << "an ordinary graceful restart is now being delayed by the race fix";
}

/* ...and it writes only where the write would land on the running image.
 *
 * ota_rollback.cpp cannot be built here, so what the host can hold is the
 * shape: the boot selection is READ (esp_ota_get_boot_partition, not a cached
 * pointer), COMPARED against the running partition, handed to the verdict, and
 * the mark is reached only through the verdict - a BOOT_SELECTION_MOVED arm
 * that returns without marking. Each of the four is a mutation that kept every
 * other test green: a constant `true` for "still selected", a verdict read
 * and ignored, the mark hoisted above the switch, the MOVED arm falling
 * through.
 */
TEST(OtaConfirmPlacement, TheMarkIsGatedOnTheBootSelectionStillBeingTheRunningImage) {
  const std::string mark = function_body(read_source("../Software/src/devboard/utils/ota_rollback.cpp"),
                                         "void mark_ota_image_valid(void) {");
  ASSERT_FALSE(mark.empty());

  const size_t boot = mark.find("esp_ota_get_boot_partition()");
  ASSERT_NE(boot, std::string::npos) << "mark_ota_image_valid() no longer reads the boot selection - a revert or an "
                                        "OTA inside the window confirms the slot it moved to";
  const size_t verdict = mark.find("ota_confirm_verdict(");
  ASSERT_NE(verdict, std::string::npos) << "the write side no longer asks the verdict";
  const size_t write = mark.find("esp_ota_mark_app_valid_cancel_rollback()");
  ASSERT_NE(write, std::string::npos);

  EXPECT_NE(mark.find("switch (ota_confirm_verdict("), std::string::npos)
      << "the verdict is computed but the write is not switched on it";
  EXPECT_LT(boot, verdict) << "the boot selection is read after the verdict is taken";
  EXPECT_LT(verdict, write) << "the image is marked before the verdict is taken";
  EXPECT_NE(mark.substr(boot, verdict - boot).find("running->address"), std::string::npos)
      << "the boot selection is read but never compared with the running partition";

  const size_t moved = mark.find("case OtaConfirmVerdict::BOOT_SELECTION_MOVED:");
  ASSERT_NE(moved, std::string::npos) << "no arm declines the write when the selection moved";
  const std::string arm = mark.substr(moved, mark.find("case OtaConfirmVerdict::CONFIRM:", moved) - moved);
  EXPECT_NE(arm.find("return;"), std::string::npos) << "the MOVED arm falls through to the write";
  EXPECT_EQ(arm.find("esp_ota_mark_app_valid_cancel_rollback"), std::string::npos)
      << "the MOVED arm marks the image anyway";
}

/* ...and the lines it writes survive the logger.
 *
 * Logging::printf renders through vsnprintf into a MAX_LINE_LENGTH_PRINTF buffer and, when the
 * result would not fit, overwrites the LAST character with a newline - so an over-long line loses
 * its whole tail and says nothing about having done so. These lines put the operative half at the
 * end (which image, how long the next window is), which is exactly the half that goes.
 *
 * The BOOT_SELECTION_MOVED notice was 159 characters with a five-character version and 173 with a
 * realistic one: everything from "the image booting next runs its own 42 s window" was cut, and
 * nothing failed. It is the longest logging.printf format string in the firmware by fifty
 * characters, so the budget is a real constraint here and not a hypothetical.
 *
 * The limit is READ from logging.cpp rather than repeated, so raising the buffer relaxes this by
 * itself; and an unknown conversion is a hard failure rather than a guess, because a width table
 * that silently defaults is a test that passes for the wrong reason.
 */
TEST(OtaConfirmPlacement, EveryOtaLogLineFitsTheLoggersOwnBuffer) {
  const size_t limit = logger_line_budget(read_source("../Software/src/devboard/utils/logging.cpp"));
  ASSERT_GT(limit, 1u);

  const std::string src = read_source("../Software/src/devboard/utils/ota_rollback.cpp");
  size_t at = 0;
  int checked = 0;
  while ((at = src.find("logging.printf(", at)) != std::string::npos) {
    const LogCall call = parse_log_call(src, at + 14);
    ASSERT_TRUE(call.format_is_literal) << "an OTA log line grew a format this test cannot read";
    ASSERT_FALSE(call.format.empty());
    EXPECT_LE(widest_rendering(call.format), limit - 1) << "this line is truncated and loses its tail: " << call.format;
    ++checked;
    at += 15;
  }
  EXPECT_GE(checked, 2) << "the OTA log lines are no longer where this test looks for them";
}

/* ...and so does every other line in the firmware that renders a version string.
 *
 * The truncation defect was not special to ota_rollback.cpp: it was a format long
 * enough that a 31-character version pushed it past the buffer, and a version
 * string is the one argument in this tree wide enough to do that on its own. A
 * scan of EVERY logging.printf under the naive worst-case model gives three
 * false positives - a pack voltage and a SOC rendered at full unsigned width -
 * so the sweep is deliberately narrowed to the calls that carry a version, where
 * the widest rendering is a fact (esp_app_desc_t::version is a char[32]) rather
 * than a theoretical bound nobody's data reaches.
 *
 * Discovered rather than listed. A hand-written list of call sites is a list
 * that goes stale the first time somebody adds a fourth one, and the whole point
 * of the defect that prompted this is that nothing noticed the third.
 */
TEST(OtaConfirmPlacement, EveryLogLineCarryingAVersionStringFitsTheLoggersOwnBuffer) {
  const size_t limit = logger_line_budget(read_source("../Software/src/devboard/utils/logging.cpp"));
  ASSERT_GT(limit, 1u);

  int checked = 0;
  int outside_ota_rollback = 0;
  for (const auto& path : firmware_sources()) {
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open()) << path.string();
    const std::string src =
        strip_comments(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));

    for (const std::string& call_name : {std::string("logging.printf("), std::string("DEBUG_PRINTF(")}) {
      size_t at = 0;
      while ((at = src.find(call_name, at)) != std::string::npos) {
        const size_t open_paren = at + call_name.size() - 1;
        at += call_name.size();
        const LogCall call = parse_log_call(src, open_paren);
        /* Only the calls whose ARGUMENTS carry a version. The format's own words
         * are not the test - "version" in prose proves nothing about what gets
         * rendered - and a runtime format (a couple of calls pass a function
         * returning const char*) carries no version and is not measurable here
         * anyway. One that DID carry a version would be, and is reported. */
        if (call.arguments.find("version") == std::string::npos) {
          continue;
        }
        ASSERT_TRUE(call.format_is_literal)
            << path.string() << ": a version is rendered through a format this test cannot read: " << call.arguments;
        EXPECT_LE(widest_rendering(call.format), limit - 1)
            << path.string() << ": a version string truncates this line and it loses its tail: " << call.format;
        ++checked;
        if (path.filename() != "ota_rollback.cpp") {
          ++outside_ota_rollback;
        }
      }
    }
  }
  EXPECT_GE(checked, 3) << "the version-carrying log lines are no longer being found - the sweep found " << checked;
  /* The count alone would stay green with a sweep that never left the file the
   * defect was found in, which is the one thing this test adds over the one
   * above it. Today it reaches the bootup line in wifi.cpp and the banner in
   * Software.cpp - the latter only because __DATE__ and __TIME__ are spliced
   * into the format rather than stopping the scan. */
  EXPECT_GE(outside_ota_rollback, 2)
      << "the sweep is not reaching outside ota_rollback.cpp any more - it found " << outside_ota_rollback
      << " such lines, and the whole point of it is the call sites nobody thought to list";
}

/* ---- the OTA upload confirms the image it is about to replace ---- */

/* The half the target-aware write side left on purpose. It declines once the
 * boot selection has moved, which is what stops the fresh image being confirmed
 * on the strength of nothing; but the image being REPLACED then stays
 * PENDING_VERIFY, the bootloader rewrites it to ABORTED on the way past, and
 * the arrival logs a rollback for an image that did not fail - both measured on
 * the bench. It also leaves the arrival with no confirmed sibling, so if it dies
 * in its own window the bootloader's both-invalid fallback picks a slot by
 * index rather than by state.
 *
 * onOTAStart() is the last moment the running image is still the boot selection:
 * Update.end() moves it. Serving a multi-megabyte upload is the same class of
 * liveness witness as serving a restart request, which the gate already accepts.
 *
 * webserver.cpp is not in this binary, so this is read from the source - which
 * is also exactly how the call could be deleted with every other test green.
 */
TEST(OtaConfirmPlacement, AnOtaUploadArmsTheConfirmationBeforeTheSelectionMoves) {
  const std::string webserver = read_source("../Software/src/devboard/webserver/webserver.cpp");
  const std::string start = function_body(webserver, "void onOTAStart() {");
  ASSERT_FALSE(start.empty());

  EXPECT_NE(start.find("ota_confirm_request()"), std::string::npos)
      << "onOTAStart() no longer confirms the running image: the image this upload replaces will be "
         "left PENDING_VERIFY, and the arrival will report a rollback nobody asked for";

  /* And it ARMS rather than writes. onOTAStart() runs in the async TCP task; the
   * otadata write belongs on the ordinary main-task path, which is the whole
   * routing rule in ota_confirm_gate.h. */
  EXPECT_EQ(start.find("mark_ota_image_valid"), std::string::npos)
      << "the otadata write is initiated from the async TCP task instead of being handed to loop()";
  EXPECT_EQ(start.find("esp_ota_mark_app_valid_cancel_rollback"), std::string::npos)
      << "the confirmation is written straight from the async TCP task";

  // ...and onOTAStart is still what ElegantOTA calls when an upload begins;
  // arming in a function nothing is wired to would pass every check above.
  EXPECT_NE(webserver.find("ElegantOTA.onStart(onOTAStart)"), std::string::npos)
      << "onOTAStart is no longer the upload's start callback";
}

/* The revert path is deliberately NOT given the same treatment (user ruling:
 * OTA start only). Confirming the image a user has just asked to leave would
 * make an in-window revert reversible, which is a change to a documented
 * user-facing contract - the revert dialog promises the trip is one-way and two
 * tests pin that promise - and it would mean confirming an image BECAUSE the
 * user said they did not want it.
 *
 * So the route sets the boot partition and restarts, and the gate's own
 * BOOT_SELECTION_MOVED arm is what stops the arming that graceful_restart() does
 * from landing on the slot being reverted into. Adding ota_confirm_request()
 * here is the mutation this test exists to catch.
 */
TEST(OtaConfirmPlacement, TheRevertRouteDoesNotConfirmTheImageItIsLeaving) {
  const std::string webserver = read_source("../Software/src/devboard/webserver/webserver.cpp");
  const size_t route = webserver.find("def_route_with_auth(\"/revertFirmware\"");
  ASSERT_NE(route, std::string::npos) << "the revert route is no longer where this test looks for it";
  const std::string handler = brace_block_at(webserver, route);
  ASSERT_FALSE(handler.empty());

  const size_t select = handler.find("esp_ota_set_boot_partition(");
  ASSERT_NE(select, std::string::npos) << "the revert route no longer moves the boot selection";

  EXPECT_EQ(handler.find("ota_confirm_request"), std::string::npos)
      << "the revert route arms a confirmation: an in-window revert would stop being one-way, and the "
         "dialog still promises that it is";
  EXPECT_EQ(handler.find("mark_ota_image_valid"), std::string::npos)
      << "the revert route confirms the image the user asked to leave";

  // The restart it does perform arms the gate, and must stay AFTER the
  // selection moves - that ordering is what makes the write side decline.
  const size_t restart = handler.find("graceful_restart()");
  ASSERT_NE(restart, std::string::npos);
  EXPECT_LT(select, restart) << "the restart is requested before the boot selection moves, so the confirmation "
                                "it arms would land on the image being left behind after all";
}

/* The scenario the row is about, played through the gate: an update lands, and a
 * second OTA arrives while the first is still inside its window.
 *
 * Before this change the running image was never confirmed - the arming happened
 * at onOTAEnd() via graceful_restart(), by which time Update.end() had moved the
 * selection and the write side declined. Now the arming happens at the start, so
 * the confirmation is owed while the running image is still the selection, and
 * the verdict says CONFIRM. The second half matters as much as the first: once
 * taken, the restart at the end of the upload owes nothing, so nothing tries to
 * confirm the freshly written image on its behalf.
 */
TEST_F(OtaConfirmGate, ASecondOtaInsideTheWindowConfirmsTheRunningImageAndNotTheIncomingOne) {
  ota_confirm_check(5000);  // five seconds in: the window has not elapsed
  ASSERT_FALSE(ota_confirm_take_pending());

  ota_confirm_request();  // onOTAStart(), while the running image is still the selection
  ASSERT_TRUE(ota_confirm_take_pending()) << "the upload did not arm a confirmation for the image it replaces";
  EXPECT_EQ(ota_confirm_verdict(true, true), OtaConfirmVerdict::CONFIRM)
      << "the running image is pending and still selected at this point - this is the write that was missing";

  // Update.end() has now moved the selection, and onOTAEnd() requests the restart.
  ota_confirm_request();
  EXPECT_FALSE(ota_confirm_take_pending())
      << "a second confirmation is owed after the selection moved; it would be declined, but owing it at all "
         "means the gate stopped being once-per-boot";
  EXPECT_EQ(ota_confirm_verdict(true, false), OtaConfirmVerdict::BOOT_SELECTION_MOVED);
}

/* The boot-time report says what the stored state supports, and no more.
 *
 * ABORTED in the passive slot means only that the image was PENDING_VERIFY when
 * a reset went past it. Two things produce that once the OTA path confirms
 * before it moves the selection: the image really did fail to start, and a
 * deliberate in-window revert, where the reboot the user asked for is what fails
 * the image they left. otadata records the state, not the reason, so a line that
 * names one of them is wrong half the time - and it is the revert case that gets
 * libelled, because that user did nothing wrong.
 *
 * ota_rollback.cpp is not in this binary; the wording is a user-facing decision
 * with no host-observable behaviour behind it, so it is read from the source.
 * The event text is checked through events.cpp, which IS linked here.
 */
TEST(OtaConfirmPlacement, TheRollbackReportDoesNotClaimAFailureItCannotSee) {
  const std::string report =
      function_body(read_source("../Software/src/devboard/utils/ota_rollback.cpp"), "void report_ota_rollback(void) {");
  ASSERT_FALSE(report.empty());

  EXPECT_EQ(report.find("failed to start"), std::string::npos)
      << "the boot report names a cause the otadata state cannot distinguish - an in-window revert "
         "produces exactly the same ABORTED entry";
  EXPECT_NE(report.find("did not confirm itself"), std::string::npos)
      << "the report no longer says what actually happened: the image never confirmed";

  const std::string event = get_event_message_string(EVENT_OTA_ROLLBACK);
  EXPECT_NE(event.find("reverted"), std::string::npos)
      << "the event is the surface with room to name both causes, and it names only one: " << event;
  EXPECT_NE(event.find("failed to start"), std::string::npos) << event;
}
