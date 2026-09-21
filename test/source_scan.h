#ifndef TEST_SOURCE_SCAN_H
#define TEST_SOURCE_SCAN_H

#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <string>

/* Reading firmware source as text, for the files the host build does not link.
 *
 * test/CMakeLists.txt carries a curated source list - "the subset that links in
 * a host build" - and a file outside it cannot be exercised by any ordinary
 * test. Software.cpp and comm_can.cpp are both outside it, and both hold
 * placements whose ORDER is the whole point: a mark that must be reached, a
 * teardown that must precede the pointer it protects. These helpers let a test
 * assert about that order, which is the only handle there is.
 *
 * Extracted from ota_confirm_tests.cpp when a second file needed them; the
 * behaviour is unchanged, and the comments below are the originals because
 * each of them records a way this kind of test can go quietly wrong.
 */
namespace source_scan {

/* Strip comments out, before anything is asserted about the code.
 *
 * Every caller decides whether the firmware does something by looking for the
 * text of a call, and firmware is surrounded by comments that NAME those calls
 * - the placement comment in core_loop says `ota_confirm_check`, and the
 * teardown comment in begin_canfd() says `end()`. Commenting a call out leaves
 * its name in place and the assertion passes on dead code, which is exactly
 * how an earlier mutation harness certified a mutation it never ran. Newlines
 * are kept so brace depth and ordering still mean what they meant.
 */
inline std::string strip_comments(const std::string& src) {
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

inline std::string read_source(const std::string& relative_to_test_dir) {
  // Located relative to this HEADER rather than through a CMake define, so the
  // test needs no build-system plumbing to run. __FILE__ names this file, not
  // the includer, which is why the paths stay relative to test/ for every
  // caller regardless of where the caller lives.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/" + relative_to_test_dir;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

// The body of a function, by brace depth from its signature line.
inline std::string function_body(const std::string& src, const std::string& signature) {
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
inline std::string brace_block_at(const std::string& src, size_t at) {
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
inline std::string call_argument(const std::string& src, const std::string& call) {
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
inline bool is_identifier(const std::string& s) {
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

}  // namespace source_scan

#endif  // TEST_SOURCE_SCAN_H
