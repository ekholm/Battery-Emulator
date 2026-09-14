#ifndef TEST_UTILS_SOURCE_SCAN_H
#define TEST_UTILS_SOURCE_SCAN_H

#include <string>

/* Comment text, removed from a source file a test reads as TEXT.
 *
 * Several files here cannot be compiled into this binary - comm_can.cpp and
 * mcp2515_lite.cpp reach for the ESP32 drivers - so the tests over them read
 * the source and search it for the code they pin. A comment inside the region
 * being searched is indistinguishable from the code, and both directions of
 * that are live:
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

#endif  // TEST_UTILS_SOURCE_SCAN_H
