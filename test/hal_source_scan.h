#pragma once

/* Reading a board HAL as SOURCE, shared by the two tests that have to.
 *
 * Both `can_interface_availability_tests.cpp` and
 * `hal_interface_declaration_tests.cpp` scan the HAL headers as text, because
 * they cannot do the obvious thing and construct the boards: `types.h` declares
 * each board's GPIO-option enums inside `#ifdef HW_<BOARD>`, so a translation
 * unit holds exactly one board (the host suite builds `HW_LILYGO`), and
 * defining two board macros risks ODR mismatches against every other TU.
 *
 * The extraction lives here rather than once per test because getting it right
 * took two review rounds and the second file did not inherit either of them:
 *
 *   - Brace-MATCH, do not scan to the first '}'. The body's own
 *     initializer list is a nested brace, so a naive slice stops at the end of
 *     the first `return {...}` and a conditional `push_back` after it is
 *     invisible - which made the check unable to see exactly the boards whose
 *     declaration is interesting.
 *   - And skip comments and string/char literals while
 *     matching, or a '{' in PROSE runs the slice past the end of the function.
 *
 * A second copy of that reasoning is a second chance to miss half of it, and
 * that is what happened: the declaration audit shipped with a regex
 * (`available_interfaces\(\)\s*\{([\s\S]*?)\n  \}`) that has neither property.
 */

#include <gtest/gtest.h>

#include <string>

namespace hal_scan {

/* The body of the function whose signature contains `signature`, braces matched,
 * comments and literals skipped while matching.
 *
 * Parameterised because the page-side check needs the same extraction over
 * settings_html.cpp, and re-deriving it a third time is precisely the mistake
 * this header was created to stop.
 */
inline std::string body_of(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = source.find('{', at);
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

// The body of available_interfaces(), braces matched, comments and literals skipped.
inline std::string available_interfaces_body(const std::string& source) {
  return body_of(source, "available_interfaces()");
}

/* Everything outside comments and literals, with the removed spans blanked so
 * offsets and line structure survive.
 *
 * Any heuristic that asks "does this code do X" must run on THIS, not on the
 * raw text. The declaration audit decided a board was "conditional, do not
 * judge" by searching the raw body for `if (` and `push_back` - so a comment
 * that merely MENTIONS a condition switched the board off. Demonstrated during
 * review: adding the sentence "Kept flat on purpose: if (one day) a variant
 * probe appears, make this conditional." to hw_stark.h, together with the
 * phantom MCP2515 declaration this branch removes, left all three audit tests
 * green. House style here is long explanatory comments, so that is not an
 * exotic input - it is the most likely next edit.
 */
inline std::string code_only(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        out += ' ';
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const size_t end = text.find("*/", i + 2);
      const size_t stop = (end == std::string::npos) ? text.size() : end + 2;
      for (; i < stop; ++i) {
        out += (text[i] == '\n') ? '\n' : ' ';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      out += ' ';
      ++i;
      while (i < text.size() && text[i] != quote) {
        const size_t step = (text[i] == '\\') ? 2 : 1;
        for (size_t k = 0; k < step && i < text.size(); ++k, ++i) {
          out += ' ';
        }
      }
      if (i < text.size()) {
        out += ' ';
        ++i;
      }
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

/* Comments blanked, literals kept - for the scans that need the string a case
 * arm RETURNS, which code_only() would blank along with the comment beside it.
 *
 * The name scan had the raw-text version of the same defect code_only() exists
 * to stop, one function over: it matched `case ...:` and `return ...;` only when
 * nothing but whitespace separated them, so a comment written between the label
 * and the arm removed that interface from the map entirely - the check then had
 * nothing to judge and passed. Three boards acquired exactly such a comment in
 * the commit that added the rule those checks enforce, so all three stopped
 * being covered by it. House style here is long explanatory comments; a scan
 * that reads raw text will keep meeting them.
 */
inline std::string without_comments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') {
        out += ' ';
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const size_t end = text.find("*/", i + 2);
      const size_t stop = (end == std::string::npos) ? text.size() : end + 2;
      for (; i < stop; ++i) {
        out += (text[i] == '\n') ? '\n' : ' ';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      // Copied through verbatim: a comment marker inside a literal is not a
      // comment, and the literal itself is what the caller came for.
      const char quote = c;
      out += c;
      ++i;
      while (i < text.size() && text[i] != quote) {
        if (text[i] == '\\' && i + 1 < text.size()) {
          out += text[i];
          ++i;
        }
        out += text[i];
        ++i;
      }
      if (i < text.size()) {
        out += text[i];
        ++i;
      }
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

}  // namespace hal_scan
