#pragma once

/* Reading a board HAL as SOURCE, shared by the two tests that have to.
 *
 * The extraction and the comment stripper are utils/source_scan.h's - the whole
 * suite's, pinned by source_scan_tests.cpp. What is left here is what is about
 * the HAL: which function the board scans read, and code_only(), which blanks
 * literals as well as comments because these checks ask whether a board
 * DECLARES an interface and never what string it returns.
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

#include "utils/source_scan.h"

namespace hal_scan {

/* The body of the function whose signature contains `signature`.
 *
 * The extraction itself lives in utils/source_scan.h, which the comm_can.cpp
 * scans share: brace-matched, comments and literals skipped while matching,
 * and a forward declaration of the same name skipped rather than read. This
 * used to be a second implementation of the first two of those and never had
 * the third.
 *
 * Parameterised because the page-side check needs the same extraction over
 * settings_html.cpp, and re-deriving it a third time is precisely the mistake
 * this header was created to stop.
 */
inline std::string body_of(const std::string& source, const std::string& signature) {
  return ::function_body(source, signature);
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
 *
 * The implementation is utils/source_scan.h's, which source_scan_tests.cpp
 * pins; this was a second copy of it with the same intent.
 */
inline std::string without_comments(const std::string& text) {
  return strip_comments(text);
}

}  // namespace hal_scan
