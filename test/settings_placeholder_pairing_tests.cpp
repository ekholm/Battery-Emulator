#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

/* Every %PLACEHOLDER% the settings page emits must have something that answers it,
 * and nothing must answer a placeholder the page never emits.
 *
 * The two sides are coupled ONLY by hand-matched string literals. Nothing checks
 * them and no compiler can see them: a template says %BAL_MAX_TIME% and a handler
 * tests "BALANCING_MAX_TIME", and the value renders empty on a shipping build with
 * every test green. That pair survived fourteen months and was reported from
 * outside before anyone here noticed.
 *
 * This reads the source as TEXT, which is the only coverage this file can have:
 * the webserver is deliberately excluded from the host binary (FS.h, ip_addr), so
 * a compiled test is impossible without extracting the whole processor.
 *
 * IT IS PREFIX-AWARE ON PURPOSE, and that is not decoration. On the i18n tree the
 * answers do not all look like `var == "NAME"`: there is a lookup TABLE of
 * {"TRD01", ...} entries and a `var.startsWith("TRIVB_")` family. A naive
 * set-difference over that tree reports dozens of false positives, which is how a
 * check like this gets switched off. The parsing lives in a function so it can be
 * driven over both shapes rather than only the tree that happens to be checked out.
 */

namespace {

struct Answers {
  std::set<std::string> exact;        // var == "NAME", and {"NAME", ...} table rows
  std::vector<std::string> prefixes;  // var.startsWith("PREFIX")
};

// A line whose first non-space characters begin a comment. The templates are raw
// string literals full of URLs, so stripping "//" anywhere would eat half of
// every https:// in the file; anchoring at the start of the line is enough for
// the case that actually arises - prose ABOUT placeholders, which reads
// "%PLACEHOLDER% substitutions" and is not an emitted placeholder at all. Found
// by running this over the i18n tree, where it was the only false positive.
bool is_comment_line(const std::string& line) {
  size_t i = line.find_first_not_of(" \t");
  if (i == std::string::npos) {
    return false;
  }
  return line.compare(i, 2, "//") == 0 || line.compare(i, 2, "/*") == 0 || line[i] == '*';
}

std::set<std::string> emitted_placeholders(const std::string& src) {
  std::set<std::string> out;
  const std::regex re(R"(%([A-Z0-9_]+)%)");
  std::istringstream lines(src);
  std::string line;
  while (std::getline(lines, line)) {
    if (is_comment_line(line)) {
      continue;
    }
    for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it) {
      out.insert((*it)[1]);
    }
  }
  return out;
}

Answers answers(const std::string& src) {
  Answers a;
  const std::regex eq(R"(var\s*==\s*\"([A-Z0-9_]+)\")");
  for (auto it = std::sregex_iterator(src.begin(), src.end(), eq); it != std::sregex_iterator(); ++it) {
    a.exact.insert((*it)[1]);
  }
  // The i18n dialog-strings table: {"TRD01", TrKey::...}. A placeholder answered
  // by a table row is answered just as much as one answered by an if.
  const std::regex row(R"(\{\s*\"([A-Z0-9_]+)\"\s*,)");
  for (auto it = std::sregex_iterator(src.begin(), src.end(), row); it != std::sregex_iterator(); ++it) {
    a.exact.insert((*it)[1]);
  }
  const std::regex pre(R"(var\.startsWith\(\s*\"([A-Z0-9_]+)\"\s*\))");
  for (auto it = std::sregex_iterator(src.begin(), src.end(), pre); it != std::sregex_iterator(); ++it) {
    a.prefixes.push_back((*it)[1]);
  }
  return a;
}

bool answered(const std::string& name, const Answers& a) {
  if (a.exact.count(name)) {
    return true;
  }
  return std::any_of(a.prefixes.begin(), a.prefixes.end(),
                     [&](const std::string& p) { return name.size() > p.size() && name.compare(0, p.size(), p) == 0; });
}

std::string read_settings_html() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  std::ifstream src(dir + "/../Software/src/devboard/webserver/settings_html.cpp");
  EXPECT_TRUE(src.is_open()) << "settings_html.cpp is not where this test looks";
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(SettingsPlaceholderPairing, EveryEmittedPlaceholderIsAnswered) {
  const std::string src = read_settings_html();
  const Answers a = answers(src);
  std::vector<std::string> unanswered;
  for (const std::string& name : emitted_placeholders(src)) {
    if (!answered(name, a)) {
      unanswered.push_back(name);
    }
  }
  std::string joined;
  for (const std::string& n : unanswered) {
    joined += "\n    %" + n + "%";
  }
  EXPECT_TRUE(unanswered.empty())
      << "the page emits placeholders nothing answers, so they render EMPTY on a shipping build:" << joined;
}

TEST(SettingsPlaceholderPairing, NoAnswerIsWrittenForAPlaceholderThePageNeverEmits) {
  const std::string src = read_settings_html();
  const std::set<std::string> emitted = emitted_placeholders(src);
  const Answers a = answers(src);

  std::vector<std::string> dead;
  for (const std::string& name : a.exact) {
    if (!emitted.count(name)) {
      dead.push_back(name);
    }
  }
  for (const std::string& p : a.prefixes) {
    const bool used = std::any_of(emitted.begin(), emitted.end(), [&](const std::string& n) {
      return n.size() > p.size() && n.compare(0, p.size(), p) == 0;
    });
    if (!used) {
      dead.push_back(p + "*");
    }
  }
  std::string joined;
  for (const std::string& n : dead) {
    joined += "\n    " + n;
  }
  // A dead answer is not a rendering bug, but it is how the live one hid: the
  // renamed handler sat here looking maintained while its placeholder went empty.
  EXPECT_TRUE(dead.empty()) << "answers exist for placeholders the page never emits:" << joined;
}

TEST(SettingsPlaceholderPairing, TheFileStillLooksLikeSomethingThisTestCanRead) {
  // A parser that matches nothing passes both cases above. Pin that it is
  // actually finding the shapes it claims to.
  const std::string src = read_settings_html();
  EXPECT_GT(emitted_placeholders(src).size(), 100u) << "far fewer placeholders than expected - has the form changed?";
  EXPECT_GT(answers(src).exact.size(), 100u) << "far fewer answers than expected - has the form changed?";
}

// ── The prefix shapes, which this tree does not have and the i18n tree does ──
//
// Driven over fixtures rather than the checked-out file, because the whole point
// is that the check must not break when it meets the other tree.

TEST(SettingsPlaceholderPairing, ALookupTableRowCountsAsAnAnswer) {
  const std::string src = R"(
    %TRD01%
    static const struct { const char* placeholder; TrKey key; } S[] = {
        {"TRD01", TrKey::UI_SOMETHING},
    };
  )";
  const Answers a = answers(src);
  EXPECT_TRUE(answered("TRD01", a)) << "a table row answers its placeholder as much as an if does";
}

TEST(SettingsPlaceholderPairing, AStartsWithFamilyAnswersEveryNameUnderIt) {
  const std::string src = R"(
    %TRIVB_FOO% %TRIVB_BAR%
    if (var.startsWith("TRIVB_")) { return something; }
  )";
  const Answers a = answers(src);
  EXPECT_TRUE(answered("TRIVB_FOO", a));
  EXPECT_TRUE(answered("TRIVB_BAR", a));
  EXPECT_FALSE(answered("TRIVB_", a)) << "the bare prefix is not itself a placeholder";
  EXPECT_FALSE(answered("TRIV", a)) << "a shorter name must not be swallowed by the prefix";
}

TEST(SettingsPlaceholderPairing, AnUnansweredNameIsStillCaughtAlongsidePrefixFamilies) {
  // The false-negative that matters: a real orphan must not be hidden by the
  // presence of prefix handlers elsewhere in the file.
  const std::string src = R"(
    %TRIVB_FOO% %BAL_MAX_TIME%
    if (var.startsWith("TRIVB_")) { return something; }
    if (var == "BALANCING_MAX_TIME") { return other; }
  )";
  const Answers a = answers(src);
  EXPECT_TRUE(answered("TRIVB_FOO", a));
  EXPECT_FALSE(answered("BAL_MAX_TIME", a)) << "this is the exact pair that shipped broken for fourteen months";
}
