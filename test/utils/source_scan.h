#ifndef TEST_UTILS_SOURCE_SCAN_H
#define TEST_UTILS_SOURCE_SCAN_H

#include <gtest/gtest.h>

#include <string>

/* Comment text, removed from a source file a test reads as TEXT.
 *
 * Several suites here read a source file rather than call it. Some have no
 * choice - mcp2515_lite.cpp reaches for the ESP32 drivers, and types.h holds
 * one board per translation unit. comm_can.cpp IS compiled in, and the scans
 * over it stay because they ask about every arm of a switch at once, which no
 * single board arrangement exercises; behaviour that can be driven is driven,
 * from comm_can_tests.cpp. Either way a comment inside the region being
 * searched is indistinguishable from the code, and both directions of that are
 * live:
 *
 *   - a positive assertion (`the failure branch clears the flag`) is satisfied
 *     by a comment MENTIONING the assignment, so the fix can be deleted and
 *     the guard still passes - graded, and it survived;
 *   - a negative assertion (`init_CAN() no longer returns false`) is failed by
 *     a comment mentioning what it forbids, so a comment edit reds a suite
 *     about code nobody touched.
 *
 * Comment bytes are replaced by SPACES rather than removed, because half of
 * these assertions are about ORDER - the clear comes after the failure is
 * known, and before the branch's own return - and those compare offsets into
 * the string. Same length in, same length out, so every offset keeps meaning
 * what it meant. Newlines are kept for the same reason line numbers survive.
 *
 * String and character literals are tracked, or a `"//"` inside one would
 * start a comment that never ends.
 */
inline std::string strip_comments(const std::string& src) {
  enum class State { kCode, kLineComment, kBlockComment, kString, kChar };

  std::string out = src;
  State state = State::kCode;
  bool escaped = false;

  for (size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];
    const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';

    switch (state) {
      case State::kCode:
        if (c == '/' && next == '/') {
          state = State::kLineComment;
          out[i] = ' ';
          out[i + 1] = ' ';
          ++i;
        } else if (c == '/' && next == '*') {
          state = State::kBlockComment;
          out[i] = ' ';
          out[i + 1] = ' ';
          ++i;
        } else if (c == '"') {
          state = State::kString;
        } else if (c == '\'') {
          state = State::kChar;
        }
        break;

      case State::kLineComment:
        if (c == '\n') {
          state = State::kCode;
        } else {
          out[i] = ' ';
        }
        break;

      case State::kBlockComment:
        if (c == '*' && next == '/') {
          out[i] = ' ';
          out[i + 1] = ' ';
          ++i;
          state = State::kCode;
        } else if (c != '\n') {
          out[i] = ' ';
        }
        break;

      case State::kString:
      case State::kChar:
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if ((state == State::kString && c == '"') || (state == State::kChar && c == '\'')) {
          state = State::kCode;
        }
        break;
    }
  }

  return out;
}

/* The body of a function DEFINITION, braces matched, comments and literals
 * skipped while matching. Empty when there is no definition to read.
 *
 * Three properties, each of which was a defect before it was a property, and
 * each of which a hand-rolled copy has got wrong here:
 *
 *   - Brace-MATCH, do not slice to the first '}'. A body's own initializer
 *     list is a nested brace, so a naive slice stops at the end of the first
 *     `return {...}` and everything after it is invisible.
 *   - Skip comments and string/char literals WHILE matching, or a '{' in prose
 *     runs the slice past the end of the function.
 *   - Find the DEFINITION, not a forward declaration of the same name.
 *     comm_can.cpp declares several statics at the top of the file, so a plain
 *     find() lands on the declaration and the next '{' opens whatever function
 *     follows it: the scan then reads a body that is not the one it names and
 *     reports a present property missing. Measured on init_native_can() and
 *     begin_canfd(), where two tests said a property was absent that was there.
 *
 * Four copies of this grew up in the CAN and OTA suites and only one of them
 * ever learned the third; this is the one that is maintained.
 */
inline std::string function_body(const std::string& source, const std::string& signature) {
  size_t at = source.find(signature);
  size_t open = std::string::npos;
  while (at != std::string::npos) {
    const size_t brace = source.find('{', at);
    const size_t semicolon = source.find(';', at);
    if (brace != std::string::npos && brace < semicolon) {
      open = brace;
      break;
    }
    at = source.find(signature, at + 1);
  }
  if (open == std::string::npos) {
    return "";
  }

  size_t close = open;
  int depth = 0;
  while (close < source.size()) {
    const char c = source[close];
    if (c == '/' && close + 1 < source.size() && source[close + 1] == '/') {
      const size_t eol = source.find('\n', close);
      if (eol == std::string::npos) {
        break;
      }
      close = eol;
      continue;
    }
    if (c == '/' && close + 1 < source.size() && source[close + 1] == '*') {
      const size_t end = source.find("*/", close + 2);
      if (end == std::string::npos) {
        break;
      }
      close = end + 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      ++close;
      while (close < source.size() && source[close] != quote) {
        close += (source[close] == '\\') ? 2 : 1;
      }
      ++close;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}' && --depth == 0) {
      break;
    }
    ++close;
  }
  return source.substr(open, close - open);
}

/* function_body() for a caller that KNOWS the function is there.
 *
 * An empty body is not a neutral answer: every "the body must not contain X"
 * assertion passes on one, so a signature that has been renamed silently turns
 * a guard into a test that can no longer fail. Callers that legitimately ask
 * the question of a source that may not answer it - the HAL scans, which run
 * over boards that need not declare the function at all - want the quiet form
 * above.
 */
inline std::string required_function_body(const std::string& source, const std::string& signature) {
  const std::string body = function_body(source, signature);
  if (body.empty()) {
    ADD_FAILURE() << signature << " has no definition where this test looks - re-check this test";
  }
  return body;
}

#endif  // TEST_UTILS_SOURCE_SCAN_H
