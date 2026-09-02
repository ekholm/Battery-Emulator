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

std::string read(const std::string& path) {
  std::ifstream f(path);
  EXPECT_TRUE(f.is_open()) << path;
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// name_for_comm_interface's cases, as written in one file. A value containing
// '?' is a runtime conditional and is recorded as such, never as a name.
std::map<std::string, std::string> names_in(const std::string& text) {
  std::map<std::string, std::string> out;
  const std::regex re(R"(case comm_interface::(\w+):\s*\n\s*return ([^;]+);)");
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
  if (iface == "CanAddonMcp2515")
    return "MCP2515_CS";
  if (iface == "CanFdAddonMcp2518")
    return "MCP2517_CS";
  if (iface == "CanFdAddonMcp2518_2")
    return "MCP2517_CS2";
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

     The rule is narrow and decidable from source: if a chip select is routed on
     only SOME branch, the interface may not be declared on ALL of them. */
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
    }
  }
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
      if (cs_conditional && !declared_unconditionally(code, iface)) {
        unjudged.insert(h.file + ":" + iface);  // Both sides conditional: agreement is not statically decidable.
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

  /* Two entries, both legitimately undecidable rather than merely unchecked, and
     both on the one board that is two boards. hw_lilygo2can guards each of these
     declarations with the same `is_fd()` probe that guards its pins - the 2515
     on the non-FD arm, the second FD channel on the FD arm - so they agree by
     construction. Proving that from source means evaluating the predicate, which
     is the thing a source scan cannot do; what CAN be checked is that neither is
     declared flat, and TheDeclarationAgreesWithConditionalRouting does that.
     Every other board is now fully decided. */
  const std::set<std::string> expected = {
      "hw_lilygo2can.h:CanAddonMcp2515",
      "hw_lilygo2can.h:CanFdAddonMcp2518_2",
  };
  EXPECT_EQ(unjudged, expected) << "the set of statically undecidable declarations changed. That is not "
                                   "automatically wrong, but it is never routine: either a board gained a "
                                   "runtime probe (update this list and say why) or one lost its cover.";
}
