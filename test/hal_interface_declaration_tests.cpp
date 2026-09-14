#include <gtest/gtest.h>

#include "hal_source_scan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>

/* Every board's CAN declaration, checked against that board's own wiring.
 *
 * available_interfaces() is not decoration: init_CAN() refuses anything absent
 * from it, and the settings page offers what is in it. The Stark declared an
 * MCP2515 add-on while routing no chip select for one; its empty name hid the
 * entry from the dropdown, which hides it from a READER but not from a value
 * already in NVS, and NVS is the path init_CAN() takes.
 *
 * WHAT GOES WRONG IS NOT A DRIVEN PIN, and the difference matters because the
 * true consequence is the worse one (established in review of this change). alloc_pins()
 * rejects any pin < 0 before anything is driven, so an unrouted chip select
 * never reaches the chip: it raises EVENT_GPIO_NOT_DEFINED and the MCP2515
 * block does `return false` out of init_CAN(). That block sits ABOVE the
 * MCP2518FD block, so one stale MCP2515 selection takes the board's FD
 * interfaces down with it. That is the same failure the availability guard at
 * the top of init_CAN() was rewritten to stop - one stale selection leaving the
 * board with no CAN at all - reappearing one block further down, where that
 * guard's erase-and-continue cannot reach it. "autodetected crystal: 0MHz" is a
 * different failure: that is a chip select that IS routed with no chip on the
 * other end, which is what the Stark's phantom SECOND MCP2518FD did.
 *
 * WHY THIS IS A SOURCE SCAN, which is the weaker instrument. The natural test
 * constructs every HAL and asks it - that evaluates runtime probes and cannot
 * drift from the code. It does not compile: types.h declares each board's
 * GPIO-option enums inside #ifdef HW_<BOARD>, so a translation unit can hold
 * exactly one board, and defining two board macros risks ODR mismatches
 * against every other TU. That one-board-per-TU structure is precisely why a
 * cross-board disagreement can sit in the tree unnoticed - no test can see two
 * boards at once. Filed as its own item; until it is fixed, this scan is what
 * can be had, and it is written to REFUSE to judge what it cannot evaluate
 * rather than to guess.
 */
namespace {

std::string hal_dir() {
  const std::string self = __FILE__;
  return self.substr(0, self.find_last_of('/')) + "/../Software/src/devboard/hal";
}

std::string webserver_dir() {
  const std::string self = __FILE__;
  return self.substr(0, self.find_last_of('/')) + "/../Software/src/devboard/webserver";
}

std::string read(const std::string& path) {
  std::ifstream f(path);
  EXPECT_TRUE(f.is_open()) << path;
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// name_for_comm_interface's cases, as written in one file. A value containing
// '?' is a runtime conditional and is recorded as such, never as a name.
std::map<std::string, std::string> names_in(const std::string& raw) {
  /* Comments blanked first, literals kept. The arm a case returns is separated
     from its label by nothing but whitespace ONLY until someone writes a comment
     there, and the commit that introduced the naming rule below wrote one on all
     three boards it changed - which took those three interfaces out of the map,
     and so out of every check that reads it. Blanking the comments keeps the
     match strict (whitespace only) without letting prose decide what is scanned. */
  const std::string text = hal_scan::without_comments(raw);
  std::map<std::string, std::string> out;
  const std::regex re(R"(case comm_interface::(\w+):\s*return ([^;]+);)");
  for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
    out[(*it)[1]] = (*it)[2];
  }
  return out;
}

bool is_conditional(const std::string& expr) {
  return expr.find('?') != std::string::npos;
}

// The chip-select accessor whose routing makes an add-on usable at all.
const char* cs_accessor_for(const std::string& iface) {
  if (iface == "CanAddonMcp2515") {
    return "MCP2515_CS";
  }
  if (iface == "CanFdAddonMcp2518") {
    return "MCP2517_CS";
  }
  if (iface == "CanFdAddonMcp2518_2") {
    return "MCP2517_CS2";
  }
  return nullptr;  // Native / Modbus / RS485 are not chip-select gated.
}

struct Hal {
  std::string file;
  std::string text;
  std::set<std::string> declared;
  bool declaration_is_conditional = false;
};

// Every hw_*.h on disk, so a board that is DROPPED by the scan is countable
// rather than merely absent.
std::vector<std::string> halFiles() {
  std::vector<std::string> files;
  for (const auto& e : std::filesystem::directory_iterator(hal_dir())) {
    const std::string name = e.path().filename().string();
    if (name.rfind("hw_", 0) == 0) {
      files.push_back(name);
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<Hal> halsWithDeclarations() {
  std::vector<Hal> out;
  for (const auto& name : halFiles()) {
    Hal h;
    h.file = name;
    h.text = read(hal_dir() + "/" + name);
    // Brace-matched, comments and literals skipped - the extraction the sibling
    // test already had to get right twice, now shared instead of re-derived (see
    // hal_source_scan.h for why the regex this replaces was not good enough).
    const std::string body = hal_scan::available_interfaces_body(h.text);
    if (body.empty()) {
      continue;  // Counted by TheScanSeesEveryBoard, which fails if this happens.
    }
    /* The "is it conditional" question is asked of CODE, never of prose. Asking
       the raw body meant a comment containing the characters `if (` switched the
       whole board off - and both checks below skip a conditional board. */
    const std::string code = hal_scan::code_only(body);
    h.declaration_is_conditional = code.find("push_back") != std::string::npos ||
                                   code.find("if (") != std::string::npos || code.find("?") != std::string::npos;
    const std::regex ifr(R"(comm_interface::(\w+))");
    for (std::sregex_iterator it(code.begin(), code.end(), ifr), end; it != end; ++it) {
      h.declared.insert((*it)[1]);
    }
    out.push_back(h);
  }
  return out;
}

}  // namespace

namespace {

/* Spans of the declaration body that a runtime condition controls.
 *
 * Needed because "the declaration is conditional" is not a property of a BOARD,
 * it is a property of each interface in it. hw_lilygo2can declares five
 * interfaces flat and pushes a sixth behind `if (is_fd())`; treating the whole
 * board as unjudgeable because of that one push_back is what let a real defect
 * sit inside it (see TheDeclarationAgreesWithConditionalRouting).
 */
std::vector<std::pair<size_t, size_t>> conditional_spans(const std::string& code) {
  std::vector<std::pair<size_t, size_t>> spans;
  for (size_t at = code.find("if ("); at != std::string::npos; at = code.find("if (", at + 1)) {
    const size_t open = code.find('{', at);
    if (open == std::string::npos) {
      continue;
    }
    size_t k = open;
    /* Walk the whole if/else-if/else chain, not just the first block. An `else`
       arm is as conditional as its `if`, and missing that reported a correctly
       guarded declaration as unconditional - caught by this test failing on the
       very fix it had just asked for. */
    while (k < code.size()) {
      int depth = 0;
      for (; k < code.size(); ++k) {
        if (code[k] == '{') {
          ++depth;
        } else if (code[k] == '}' && --depth == 0) {
          break;
        }
      }
      size_t next = std::min(k + 1, code.size());
      while (next < code.size() && std::isspace((unsigned char)code[next])) {
        ++next;
      }
      if (code.compare(next, 4, "else") != 0) {
        break;
      }
      const size_t arm = code.find('{', next);
      if (arm == std::string::npos) {
        k = code.size();
        break;
      }
      k = arm;
    }
    spans.emplace_back(open, std::min(k, code.size()));
  }
  // A ternary controls everything up to the statement's end.
  for (size_t at = code.find('?'); at != std::string::npos; at = code.find('?', at + 1)) {
    const size_t end = code.find(';', at);
    spans.emplace_back(at, end == std::string::npos ? code.size() : end);
  }
  return spans;
}

bool declared_unconditionally(const std::string& code, const std::string& iface) {
  const std::string token = "comm_interface::" + iface;
  const auto spans = conditional_spans(code);
  for (size_t at = code.find(token); at != std::string::npos; at = code.find(token, at + 1)) {
    const size_t after = at + token.size();
    if (after < code.size() && (std::isalnum((unsigned char)code[after]) || code[after] == '_')) {
      continue;  // A prefix of a longer name: Mcp2518 inside Mcp2518_2.
    }
    bool guarded = false;
    for (const auto& sp : spans) {
      if (at >= sp.first && at <= sp.second) {
        guarded = true;
        break;
      }
    }
    if (!guarded) {
      return true;
    }
  }
  return false;
}

/* The if/else ARMS of the declaration body, each with the condition that
 * controls it and which side of it the arm is.
 *
 * conditional_spans() above answers "is this declaration guarded at all", which
 * is what the flat-declaration check needs. It is not enough for the board that
 * is two boards: guarding a declaration with the WRONG branch of the right probe
 * is the original defect exactly - the interface is declared on the fitment
 * whose chip select is GPIO_NUM_NC - and a check that only asks "is it guarded"
 * passes it. Mutation-checked: see AnInvertedGuardIsNotAgreement.
 */
struct Arm {
  std::string cond;
  bool positive;  // true: the `if` arm; false: its bare `else`.
  size_t begin;
  size_t end;
};

std::vector<Arm> conditional_arms(const std::string& code) {
  std::vector<Arm> arms;
  for (size_t at = code.find("if ("); at != std::string::npos; at = code.find("if (", at + 1)) {
    const size_t paren = code.find('(', at);
    if (paren == std::string::npos) {
      continue;
    }
    size_t k = paren;
    int depth = 0;
    for (; k < code.size(); ++k) {
      if (code[k] == '(') {
        ++depth;
      } else if (code[k] == ')' && --depth == 0) {
        break;
      }
    }
    if (k >= code.size()) {
      continue;
    }
    const std::string cond = code.substr(paren + 1, k - paren - 1);
    bool positive = true;
    size_t open = code.find('{', k);
    while (open != std::string::npos) {
      size_t close = open;
      int d = 0;
      for (; close < code.size(); ++close) {
        if (code[close] == '{') {
          ++d;
        } else if (code[close] == '}' && --d == 0) {
          break;
        }
      }
      if (close >= code.size()) {
        break;
      }
      arms.push_back({cond, positive, open, close});
      size_t next = close + 1;
      while (next < code.size() && std::isspace((unsigned char)code[next])) {
        ++next;
      }
      if (code.compare(next, 4, "else") != 0) {
        break;
      }
      size_t after = next + 4;
      while (after < code.size() && std::isspace((unsigned char)code[after])) {
        ++after;
      }
      if (code.compare(after, 3, "if ") == 0) {
        break;  // `else if` is its own condition; the outer loop reaches it.
      }
      positive = false;
      open = code.find('{', next);
    }
  }
  return arms;
}

const Arm* innermost_arm(const std::vector<Arm>& arms, size_t pos) {
  const Arm* best = nullptr;
  for (const auto& a : arms) {
    if (pos < a.begin || pos > a.end) {
      continue;
    }
    if (best == nullptr || (a.end - a.begin) < (best->end - best->begin)) {
      best = &a;
    }
  }
  return best;
}

std::string squeeze(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (!std::isspace((unsigned char)c)) {
      out += c;
    }
  }
  return out;
}

// `COND ? A : B`, split at the top level. Anything else is not a ternary.
struct Ternary {
  bool ok = false;
  std::string cond;
  std::string when_true;
  std::string when_false;
};

Ternary as_ternary(const std::string& expr) {
  Ternary t;
  int depth = 0;
  size_t q = std::string::npos;
  for (size_t i = 0; i < expr.size(); ++i) {
    if (expr[i] == '(') {
      ++depth;
    } else if (expr[i] == ')') {
      --depth;
    } else if (expr[i] == '?' && depth == 0) {
      q = i;
      break;
    }
  }
  if (q == std::string::npos) {
    return t;
  }
  depth = 0;
  for (size_t i = q + 1; i < expr.size(); ++i) {
    if (expr[i] == '(') {
      ++depth;
    } else if (expr[i] == ')') {
      --depth;
    } else if (expr[i] == '?' && depth == 0) {
      return t;  // Nested ternary: not the narrow shape this judges.
    } else if (expr[i] == ':' && depth == 0) {
      if (i + 1 < expr.size() && expr[i + 1] == ':') {
        ++i;  // A scope operator, not the ternary's colon.
        continue;
      }
      t.ok = true;
      t.cond = expr.substr(0, q);
      t.when_true = expr.substr(q + 1, i - q - 1);
      t.when_false = expr.substr(i + 1);
      return t;
    }
  }
  return t;
}

/* Does a conditionally-routed interface's DECLARATION agree with its routing?
 *
 * Decidable without evaluating the probe whenever both sides are written
 * against the same expression: if MCP2515_CS() is `is_fd() ? GPIO_NUM_NC :
 * GPIO_NUM_10`, the chip exists exactly when is_fd() is false, so the
 * declaration must sit on the `else` of `if (is_fd())`. Same expression, other
 * branch, and the board declares an add-on whose chip select is not routed.
 */
enum class Agreement { NotJudged, DeclaredFlat, Agrees, WrongBranch, Undecidable };

Agreement declaration_agreement(const std::string& code, const std::string& iface, const std::string& cs_value,
                                std::string* why) {
  const Ternary cs = as_ternary(cs_value);
  if (!cs.ok) {
    return Agreement::NotJudged;
  }
  const bool nc_when_true = cs.when_true.find("GPIO_NUM_NC") != std::string::npos;
  const bool nc_when_false = cs.when_false.find("GPIO_NUM_NC") != std::string::npos;
  if (nc_when_true == nc_when_false) {
    return Agreement::NotJudged;  // Routed on both branches, or on neither.
  }
  const bool routed_when_true = nc_when_false;

  const std::vector<Arm> arms = conditional_arms(code);
  const std::string token = "comm_interface::" + iface;
  Agreement verdict = Agreement::NotJudged;
  for (size_t at = code.find(token); at != std::string::npos; at = code.find(token, at + 1)) {
    const size_t after = at + token.size();
    if (after < code.size() && (std::isalnum((unsigned char)code[after]) || code[after] == '_')) {
      continue;  // A prefix of a longer name: Mcp2518 inside Mcp2518_2.
    }
    const Arm* arm = innermost_arm(arms, at);
    if (arm == nullptr) {
      if (why != nullptr) {
        *why = "declared outside any guard";
      }
      return Agreement::DeclaredFlat;
    }
    /* A leading `!` is polarity, not a different probe. Without this, the
       correct guard written as `if (!is_fd())` compares unequal to the routing's
       `is_fd()` and the pair is called undecidable - so a legitimate restyle of
       working code turns the scan's "nothing I cannot judge" assertion red.
       Measured during review: rewriting hw_lilygo2can's guard as `!is_fd()`,
       arms swapped and behaviour identical, failed TheScanReportsWhatItCannotJudge.
       Strip it from both sides and carry it in the polarity instead. */
    bool arm_positive = arm->positive;
    std::string arm_cond = squeeze(arm->cond);
    while (!arm_cond.empty() && arm_cond[0] == '!') {
      arm_cond.erase(0, 1);
      arm_positive = !arm_positive;
    }
    bool routed_positive = routed_when_true;
    std::string routing_cond = squeeze(cs.cond);
    while (!routing_cond.empty() && routing_cond[0] == '!') {
      routing_cond.erase(0, 1);
      routed_positive = !routed_positive;
    }
    if (arm_cond != routing_cond) {
      if (why != nullptr) {
        *why = "guarded by `" + squeeze(arm->cond) + "`, routed on `" + squeeze(cs.cond) + "`";
      }
      return Agreement::Undecidable;
    }
    if (arm_positive != routed_positive) {
      if (why != nullptr) {
        *why = std::string("declared on the `") + (arm->positive ? "if" : "else") + "` arm of `" + squeeze(cs.cond) +
               "`, where the chip select is GPIO_NUM_NC";
      }
      return Agreement::WrongBranch;
    }
    verdict = Agreement::Agrees;
  }
  return verdict;
}

// The accessor's written expression, or "" when the board does not override it.
std::string cs_expression(const Hal& h, const char* accessor) {
  std::smatch m;
  const std::regex re(std::string("gpio_num_t ") + accessor + R"(\(\)\s*\{\s*return ([^;]+);)");
  return std::regex_search(h.text, m, re) ? std::string(m[1]) : std::string();
}

}  // namespace

TEST(HalInterfaceDeclaration, TheScanSeesEveryBoard) {
  /* The old guard was ASSERT_GE(hals.size(), 7u) against eight HALs on disk, so
     one board could be dropped by the extractor and the audit would still pass
     while silently covering seven. Compare against what is actually there. */
  const auto files = halFiles();
  const auto hals = halsWithDeclarations();
  ASSERT_GE(files.size(), 8u) << "the HAL directory scan found too few boards to be auditing anything";
  std::set<std::string> seen;
  for (const auto& h : hals) {
    seen.insert(h.file);
  }
  for (const auto& f : files) {
    EXPECT_TRUE(seen.count(f) == 1) << f
                                    << " has no available_interfaces() the scan could extract, so it is "
                                       "not being audited at all - fix the board or the extractor, but do "
                                       "not let it drop out quietly";
  }
}

TEST(HalInterfaceDeclaration, EveryDeclaredAddOnHasItsChipSelectRouted) {
  const auto hals = halsWithDeclarations();
  ASSERT_GE(hals.size(), 8u) << "the HAL scan found too few boards to be auditing anything";

  for (const auto& h : hals) {
    for (const auto& iface : h.declared) {
      const char* accessor = cs_accessor_for(iface);
      if (accessor == nullptr) {
        continue;
      }
      if (!declared_unconditionally(hal_scan::code_only(hal_scan::available_interfaces_body(h.text)), iface)) {
        continue;  // Guarded by a runtime probe; judged by the test below.
      }
      const std::string value = cs_expression(h, accessor);
      const bool overridden = !value.empty();
      const std::string shown = overridden ? value : std::string("GPIO_NUM_NC (inherited)");
      if (overridden && is_conditional(value)) {
        continue;  // Judged by the test below.
      }
      if (!overridden || value.find("GPIO_NUM_NC") != std::string::npos) {
        ADD_FAILURE() << h.file << " declares " << iface << " but " << accessor << "() is " << shown
                      << ". init_CAN() trusts this list, so a selection already stored in NVS passes "
                         "the availability guard, alloc_pins() then refuses the unrouted pin, and the "
                         "block returns false out of init_CAN() - taking every interface declared "
                         "below it down too.";
      }
    }
  }
}

TEST(HalInterfaceDeclaration, TheDeclarationAgreesWithConditionalRouting) {
  /* THE CASE THE ORIGINAL SCAN COULD NOT SEE, and it was not hypothetical.
     hw_lilygo2can is two boards behind one runtime probe, so its declaration
     contains a push_back and the old audit skipped the WHOLE board. Inside that
     skip sat the same defect this branch removes from the Stark: it declared
     CanAddonMcp2515 flat, while MCP2515_CS() is `is_fd() ? GPIO_NUM_NC :
     GPIO_NUM_10` - so on the FD fitment the board declared an add-on whose chip
     select is not routed, and named it "" so the dropdown hid it too.

     The rule is narrow and decidable from source, and it is decidable further
     than "is it guarded at all": when the declaration is guarded by the SAME
     expression that picks the pins, which branch it sits on says whether the two
     agree. Guarding it with the wrong branch declares the add-on on exactly the
     fitment that does not route it - the original defect, wearing a guard. */
  for (const auto& h : halsWithDeclarations()) {
    const std::string code = hal_scan::code_only(hal_scan::available_interfaces_body(h.text));
    for (const auto& iface : h.declared) {
      const char* accessor = cs_accessor_for(iface);
      if (accessor == nullptr) {
        continue;
      }
      const std::string value = cs_expression(h, accessor);
      if (value.empty() || !is_conditional(value) || value.find("GPIO_NUM_NC") == std::string::npos) {
        continue;
      }
      EXPECT_FALSE(declared_unconditionally(code, iface))
          << h.file << " declares " << iface << " unconditionally, but " << accessor << "() is " << value
          << " - on the branch where it is GPIO_NUM_NC the board declares an add-on it does not route. "
             "Guard the declaration with the same probe that guards the pins.";

      std::string why;
      const Agreement verdict = declaration_agreement(code, iface, value, &why);
      EXPECT_NE(verdict, Agreement::WrongBranch)
          << h.file << " guards " << iface << " with the same probe as " << accessor << "() = " << value
          << ", but on the other branch: " << why
          << ". The board declares an add-on it does not route on that fitment, which is the defect a guard "
             "was supposed to remove.";
    }
  }
}

TEST(HalInterfaceDeclaration, AnInvertedGuardIsNotAgreement) {
  /* The mutation this check exists for, run as a test rather than trusted: the
     real hw_lilygo2can shapes, with the declaration moved to the other arm of
     the same probe. Before the polarity check, the audit passed this - it is
     guarded, and a guard was all it asked for. */
  const std::string cs = "is_fd() ? GPIO_NUM_NC : GPIO_NUM_10";
  const std::string right =
      "{ std::vector<comm_interface> out = {comm_interface::CanNative};\n"
      "  if (is_fd()) {\n    out.push_back(comm_interface::CanFdAddonMcp2518_2);\n"
      "  } else {\n    out.push_back(comm_interface::CanAddonMcp2515);\n  }\n  return out; }";
  const std::string inverted =
      "{ std::vector<comm_interface> out = {comm_interface::CanNative};\n"
      "  if (is_fd()) {\n    out.push_back(comm_interface::CanAddonMcp2515);\n"
      "  } else {\n  }\n  return out; }";
  const std::string other_probe =
      "{ std::vector<comm_interface> out = {comm_interface::CanNative};\n"
      "  if (has_addon()) {\n    out.push_back(comm_interface::CanAddonMcp2515);\n  }\n  return out; }";

  std::string why;
  EXPECT_EQ(declaration_agreement(right, "CanAddonMcp2515", cs, &why), Agreement::Agrees);
  EXPECT_EQ(declaration_agreement(inverted, "CanAddonMcp2515", cs, &why), Agreement::WrongBranch) << why;
  EXPECT_EQ(declaration_agreement(other_probe, "CanAddonMcp2515", cs, &why), Agreement::Undecidable) << why;
  EXPECT_EQ(declaration_agreement("{ return {comm_interface::CanAddonMcp2515}; }", "CanAddonMcp2515", cs, &why),
            Agreement::DeclaredFlat)
      << why;

  /* The same two boards written with the negation on the guard instead of on
     the arms. `!is_fd()` is the SAME probe, so these must land on the same two
     verdicts - otherwise a correct board is reported as unjudgeable the day
     someone restyles it, and the empty "cannot judge" list is a property of
     today's spelling rather than of the code. */
  const std::string negated_right =
      "{ std::vector<comm_interface> out = {comm_interface::CanNative};\n"
      "  if (!is_fd()) {\n    out.push_back(comm_interface::CanAddonMcp2515);\n"
      "  } else {\n    out.push_back(comm_interface::CanFdAddonMcp2518_2);\n  }\n  return out; }";
  const std::string negated_inverted =
      "{ std::vector<comm_interface> out = {comm_interface::CanNative};\n"
      "  if (!is_fd()) {\n  } else {\n    out.push_back(comm_interface::CanAddonMcp2515);\n  }\n  return out; }";
  EXPECT_EQ(declaration_agreement(negated_right, "CanAddonMcp2515", cs, &why), Agreement::Agrees) << why;
  EXPECT_EQ(declaration_agreement(negated_inverted, "CanAddonMcp2515", cs, &why), Agreement::WrongBranch) << why;

  /* And the negation on the ROUTING side, which is the same argument from the
     other end: `!is_fd() ? GPIO_NUM_10 : GPIO_NUM_NC` routes the chip on
     exactly the boards `is_fd() ? GPIO_NUM_NC : GPIO_NUM_10` does. */
  const std::string negated_cs = "!is_fd() ? GPIO_NUM_10 : GPIO_NUM_NC";
  EXPECT_EQ(declaration_agreement(right, "CanAddonMcp2515", negated_cs, &why), Agreement::Agrees) << why;
  EXPECT_EQ(declaration_agreement(inverted, "CanAddonMcp2515", negated_cs, &why), Agreement::WrongBranch) << why;
}

TEST(HalInterfaceDeclaration, ACommentBetweenACaseAndItsNameDoesNotHideTheInterface) {
  /* The name scan matched a case label and its `return` only when nothing but
     whitespace separated them. Three boards acquired a comment in exactly that
     position - written by the change that added the naming rule - and dropped
     out of the map, so the rule stopped covering the boards it was written for
     and every mutation of their names survived. */
  const std::string commented =
      "    switch (comm) {\n"
      "      case comm_interface::CanAddonMcp2515:\n"
      "        /* Named, not blanked: the page drops blank names before it\n"
      "           checks the declaration. */\n"
      "        return \"CAN (MCP2515 add-on)\";\n"
      "    }\n";
  const auto names = names_in(commented);
  ASSERT_EQ(names.count("CanAddonMcp2515"), 1u)
      << "a comment between the case and its return took the interface out of the name scan";
  EXPECT_EQ(names.at("CanAddonMcp2515"), "\"CAN (MCP2515 add-on)\"");

  const std::string blanked =
      "    switch (comm) {\n"
      "      case comm_interface::CanAddonMcp2515:\n"
      "        // Kept blank on purpose - return \"CAN (MCP2515 add-on)\" if this changes.\n"
      "        return \"\";\n"
      "    }\n";
  ASSERT_EQ(names_in(blanked).count("CanAddonMcp2515"), 1u);
  EXPECT_EQ(names_in(blanked).at("CanAddonMcp2515"), "\"\"")
      << "the name a comment MENTIONS is not the name the arm returns";
}

TEST(HalInterfaceDeclaration, EveryDeclaredInterfaceIsNamed) {
  /* The settings page drops blank-named options BEFORE it consults the
     declaration, so a declared-but-unnamed interface is not shown blank - it
     is silently absent, while init_CAN() still permits it. That also defeats
     the promise documented directly above that filter, that the CURRENTLY
     SELECTED value is always kept in the list: for a blank name it is not, so
     the one user who needs to correct it is the one who cannot see it. */
  const auto base_names = names_in(read(hal_dir() + "/hal.h"));
  for (const auto& h : halsWithDeclarations()) {
    const auto local = names_in(h.text);
    const std::string code = hal_scan::code_only(hal_scan::available_interfaces_body(h.text));
    for (const auto& iface : h.declared) {
      std::string expr;
      auto it = local.find(iface);
      if (it != local.end()) {
        expr = it->second;
      } else {
        auto b = base_names.find(iface);
        expr = (b == base_names.end()) ? "" : b->second;
      }
      if (is_conditional(expr)) {
        /* A conditional name is only acceptable if no branch of it is blank
           while the interface is declared on that branch. The narrow, decidable
           form: a name that can be "" must not belong to an interface declared
           unconditionally. */
        if (expr.find("\"\"") != std::string::npos && declared_unconditionally(code, iface)) {
          ADD_FAILURE() << h.file << " declares " << iface << " unconditionally and names it " << expr
                        << ", so on the blank branch the settings page hides it while init_CAN() still "
                           "allows it - and a stored selection of it cannot be corrected from the page.";
        }
        continue;
      }
      EXPECT_NE(expr, "\"\"") << h.file << " declares " << iface
                              << " and names it the empty string, so the settings page hides it while "
                                 "init_CAN() still allows it";
      EXPECT_NE(expr, "") << h.file << " declares " << iface << " and nothing names it";
    }
  }
}

TEST(HalInterfaceDeclaration, NoBoardBlanksAnInterfaceTheBaseClassNames) {
  /* Hiding an interface is the mechanism that kept BOTH defects on this branch
     invisible, so the mechanism itself is what this pins.

     `options_for_comm_interface` drops blank-named options BEFORE it consults
     the declaration - and therefore before the exemption documented directly
     above it, that the CURRENTLY SELECTED value is always kept in the list so an
     existing configuration is "visible and correctable rather than silently
     rewritten". Override a name to "" and that promise is void for exactly the
     user it was written for: the one with a stale value stored.

     So a board may DECLINE to declare an interface - that is the whole point of
     available_interfaces() - but it may not un-name one the base class names.
     The page already has the right vocabulary for "declared elsewhere, not here":
     it appends "(not available on this board)".

     The blank base names (CanFdNative, CanFdAddonMcp2518_2) are not covered:
     nothing is being hidden there, the enum simply has no universal name. */
  const auto base_names = names_in(read(hal_dir() + "/hal.h"));
  std::set<std::string> named_by_base;
  for (const auto& [iface, expr] : base_names) {
    if (expr != "\"\"" && !expr.empty()) {
      named_by_base.insert(iface);
    }
  }
  ASSERT_FALSE(named_by_base.empty()) << "hal.h names nothing - the base-name scan is broken, not the boards";

  for (const auto& h : halsWithDeclarations()) {
    for (const auto& [iface, expr] : names_in(h.text)) {
      if (named_by_base.count(iface) == 0) {
        continue;
      }
      EXPECT_EQ(expr.find("\"\""), std::string::npos)
          << h.file << " overrides the name of " << iface << " to " << expr << ", but hal.h names it "
          << base_names.at(iface)
          << ". A blank name is dropped by the settings page before it checks the declaration, so it also "
             "hides a value already stored in NVS from the one user who needs to correct it.";
    }
  }
}

TEST(HalInterfaceDeclaration, TheScanReportsWhatItCannotJudge) {
  /* A skip that nobody can see is indistinguishable from a pass - the original
     version of this test said exactly that and then used SUCCEED() << ..., whose
     message gtest does not print on a passing test. Verified during review: the
     board list never reached any output. So the inventory goes to stdout, which
     gtest does show, AND is asserted against an explicit expectation, so a NEW
     unjudgeable board fails here instead of quietly widening the blind spot. */
  std::set<std::string> unjudged;
  for (const auto& h : halsWithDeclarations()) {
    const std::string code = hal_scan::code_only(hal_scan::available_interfaces_body(h.text));
    for (const auto& iface : h.declared) {
      const char* accessor = cs_accessor_for(iface);
      if (accessor == nullptr) {
        continue;
      }
      const std::string value = cs_expression(h, accessor);
      const bool cs_conditional = !value.empty() && is_conditional(value);
      if (!cs_conditional || declared_unconditionally(code, iface)) {
        continue;  // Decided by the two checks above.
      }
      std::string why;
      const Agreement verdict = declaration_agreement(code, iface, value, &why);
      if (verdict != Agreement::Agrees && verdict != Agreement::WrongBranch) {
        unjudged.insert(h.file + ":" + iface + " (" + (why.empty() ? "shape not judged" : why) + ")");
      }
    }
  }
  std::string listed;
  for (const auto& u : unjudged) {
    listed += u + " ";
  }
  std::cout << "[ SCAN GAP ] declaration and routing both runtime-conditional, agreement not statically "
               "decidable: "
            << (listed.empty() ? "(none)" : listed) << std::endl;

  /* Empty, and that is a change of instrument rather than of the boards. The two
     entries this list used to carry - hw_lilygo2can's 2515 and its second FD
     channel - were called undecidable because deciding them looked like
     evaluating is_fd(). It is not: both declarations are guarded by the same
     written expression that picks the pins, so the BRANCH they sit on settles
     the question without evaluating anything, and declaration_agreement() does
     that. What lands here now is a declaration guarded by a DIFFERENT expression
     from its routing, which is the case that genuinely needs a human. */
  const std::set<std::string> expected = {};
  EXPECT_EQ(unjudged, expected) << "the set of statically undecidable declarations changed. That is not "
                                   "automatically wrong, but it is never routine: either a board gained a "
                                   "runtime probe this scan cannot pair with its routing (update this list "
                                   "and say why) or one lost its cover.";
}

TEST(HalInterfaceDeclaration, ThePageDropsBlankNamesBeforeItChecksTheDeclaration) {
  /* The premise every naming rule above rests on, asserted instead of assumed.
     Three boards carry a comment saying the settings page drops blank-named
     options BEFORE it consults the declaration, and that is why they must name
     an interface they do not declare. Nothing tested it: the webserver is not
     linked into the host suite (it reaches for FS.h and the async server), so
     the page's behaviour was a claim in a comment, and a reordering there would
     leave those comments describing code that no longer exists while the rule
     they justify stayed in force.

     Scanned rather than executed, for the same reason as everything else in this
     file, and narrow: the blank-name filter runs first, and the exemption that
     keeps a SELECTED-but-undeclared value in the list runs after it. */
  const std::string source = read(webserver_dir() + "/settings_html.cpp");
  const std::string body = hal_scan::code_only(hal_scan::body_of(source, "String options_for_comm_interface("));
  ASSERT_FALSE(body.empty()) << "options_for_comm_interface() not found - the page moved, and the naming rules "
                                "in this file cite its behaviour as their reason";

  const size_t blank_filter = body.find("name[0]");
  const size_t declared_check = body.find("declared");
  /* These two say "not found in the spelling this scan matches", not "gone": a
     source scan cannot tell a removed filter from a rewritten one, and saying
     the stronger thing would send the next reader looking for a defect that is
     not there. Measured during review: respelling `name[0]` as `*name` fails
     this test with the old message, which asserted the filter had been removed
     while it sat three lines away. */
  ASSERT_NE(blank_filter, std::string::npos)
      << "no `name[0]` blank-name filter in the option builder. Either it was removed - in which case the "
         "naming rules in this file have lost their reason - or it was respelled, in which case update this "
         "scan: "
      << body;
  ASSERT_NE(declared_check, std::string::npos)
      << "no `declared` declaration check in the option builder. Removed, or respelled - this scan cannot "
         "tell which, and both need a human: "
      << body;
  EXPECT_LT(blank_filter, declared_check)
      << "the option builder now consults the declaration before it drops blank names. If a blank-named "
         "option that IS the stored selection now survives, the rule that no board may un-name an interface "
         "the base class names has lost its reason - revisit NoBoardBlanksAnInterfaceTheBaseClassNames and "
         "the comments on hw_becom, hw_waveshare and hw_lilygo2can before relaxing anything.";

  EXPECT_NE(squeeze(body).find("!declared&&type!=selected"), std::string::npos)
      << "the exemption that keeps the currently SELECTED value in the list is not written as "
         "`!declared && type != selected` any more. If it was REMOVED, a stale stored interface can no longer "
         "be corrected from the page at all and naming it - which is what the three boards above do - buys "
         "nothing. If it was only reordered or respelled, update this scan; it matches one spelling and "
         "cannot tell the two apart: "
      << body;
}

TEST(HalInterfaceDeclaration, ThePageRepresentsAStoredCommInterfaceItCannotOffer) {
  /* The builders in select_options.h all guarantee that the stored value is
     represented in the rendered options, and SelectOptionsTest exercises that
     directly. options_for_comm_interface() cannot be exercised that way - it
     needs esp32hal and the page's own name_for_comm_interface - so it is the
     one builder where the guarantee can quietly not hold, and it is also the
     one where breaking it costs the most: an unmatched select submits its first
     option, which here is CAN 1 Native, and the next save writes it.

     Two ways to reach that state and neither is hypothetical: a stored number
     outside the enum (a bad write, or 0), and a valid member whose name is
     blank on this build - the filter asserted above drops it BEFORE the
     exemption that keeps a selected value can save it.

     Scanned, not executed, like everything else in this file. Note this does
     not weaken the naming rules above: a real name is still what a person can
     act on, and the sentinel only stops the page from guessing. */
  const std::string source = read(webserver_dir() + "/settings_html.cpp");
  const std::string body = hal_scan::code_only(hal_scan::body_of(source, "String options_for_comm_interface("));
  ASSERT_FALSE(body.empty()) << "options_for_comm_interface() not found - the page moved";

  const std::string tight = squeeze(body);
  EXPECT_NE(tight.find("represented=represented||(selected==type)"), std::string::npos)
      << "the comm-interface builder never records that it rendered the stored value, so it cannot tell "
         "whether the select has a selected option: "
      << body;
  EXPECT_NE(tight.find("if(!represented)"), std::string::npos)
      << "nothing acts on the stored value being unrepresented; the select renders with nothing selected and "
         "the browser submits the first option instead: "
      << body;
  EXPECT_NE(tight.find("unrepresented_option(static_cast<int>(selected))"), std::string::npos)
      << "the unrepresented stored value is not carried into the options by its own number, so a save cannot "
         "round-trip it: "
      << body;
}
