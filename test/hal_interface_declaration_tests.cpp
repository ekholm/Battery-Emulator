#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>

/* Every board's CAN declaration, checked against that board's own wiring.
 *
 * available_interfaces() is not decoration: init_CAN() refuses anything absent
 * from it, and the settings page offers what is in it. A board that declares
 * an interface it does not route therefore re-creates the exact failure the
 * declaration mechanism was added to stop - a chip select driven at pins where
 * nothing answers, reported as "autodetected crystal: 0MHz" for a chip that is
 * not fitted. The Stark declared an MCP2515 add-on while routing no chip
 * select for one; its empty name hid the entry from the dropdown, which hides
 * it from a READER but not from a value already in NVS, and NVS is the path
 * init_CAN() takes.
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

std::vector<Hal> halsWithDeclarations() {
  std::vector<Hal> out;
  for (const auto& e : std::filesystem::directory_iterator(hal_dir())) {
    const std::string name = e.path().filename().string();
    if (name.rfind("hw_", 0) != 0) {
      continue;
    }
    Hal h;
    h.file = name;
    h.text = read(e.path().string());
    std::smatch m;
    if (!std::regex_search(h.text, m, std::regex(R"(available_interfaces\(\)\s*\{([\s\S]*?)\n  \})"))) {
      continue;
    }
    const std::string body = m[1];
    // A board whose list is built conditionally (push_back behind a probe) is
    // recorded and skipped rather than half-read.
    h.declaration_is_conditional =
        body.find("push_back") != std::string::npos || body.find("if (") != std::string::npos;
    const std::regex ifr(R"(comm_interface::(\w+))");
    for (std::sregex_iterator it(body.begin(), body.end(), ifr), end; it != end; ++it) {
      h.declared.insert((*it)[1]);
    }
    out.push_back(h);
  }
  return out;
}

}  // namespace

TEST(HalInterfaceDeclaration, EveryDeclaredAddOnHasItsChipSelectRouted) {
  const std::string base = read(hal_dir() + "/hal.h");
  const auto hals = halsWithDeclarations();
  ASSERT_GE(hals.size(), 7u) << "the HAL scan found too few boards to be auditing anything";

  for (const auto& h : hals) {
    if (h.declaration_is_conditional) {
      continue;  // Reported by the companion test below, not guessed at here.
    }
    for (const auto& iface : h.declared) {
      const char* accessor = cs_accessor_for(iface);
      if (accessor == nullptr) {
        continue;
      }
      // Routed means this board overrides the accessor with something other
      // than the base's GPIO_NUM_NC.
      std::smatch m;
      const std::regex re(std::string("gpio_num_t ") + accessor + R"(\(\)\s*\{\s*return ([^;]+);)");
      const bool overridden = std::regex_search(h.text, m, re);
      const std::string value = overridden ? std::string(m[1]) : std::string("GPIO_NUM_NC (inherited)");
      if (!overridden || value.find("GPIO_NUM_NC") != std::string::npos) {
        if (overridden && is_conditional(value)) {
          continue;  // Routed on some runtime paths; not statically decidable.
        }
        ADD_FAILURE() << h.file << " declares " << iface << " but " << accessor << "() is " << value
                      << ". init_CAN() trusts this list, so a selection already stored in NVS passes "
                         "the availability guard and drives an unconnected pin - the failure the "
                         "declaration exists to prevent.";
      }
    }
  }
}

TEST(HalInterfaceDeclaration, EveryDeclaredInterfaceIsNamed) {
  /* The settings page drops blank-named options BEFORE it consults the
     declaration, so a declared-but-unnamed interface is not shown blank - it
     is silently absent, while init_CAN() still permits it. */
  const auto base_names = names_in(read(hal_dir() + "/hal.h"));
  for (const auto& h : halsWithDeclarations()) {
    if (h.declaration_is_conditional) {
      continue;
    }
    const auto local = names_in(h.text);
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
        continue;
      }
      EXPECT_NE(expr, "\"\"") << h.file << " declares " << iface
                              << " and names it the empty string, so the settings page hides it while "
                                 "init_CAN() still allows it";
      EXPECT_NE(expr, "") << h.file << " declares " << iface << " and nothing names it";
    }
  }
}

TEST(HalInterfaceDeclaration, TheScanReportsWhatItCannotJudge) {
  /* A skip that nobody can see is indistinguishable from a pass. Boards whose
     declaration is built at runtime are listed here so the gap is visible in
     the test output rather than implied by an early `continue`. */
  std::string skipped;
  for (const auto& h : halsWithDeclarations()) {
    if (h.declaration_is_conditional) {
      skipped += h.file + " ";
    }
  }
  RecordProperty("boards_not_statically_checkable", skipped);
  SUCCEED() << "declaration built at runtime, not covered by the two checks above: "
            << (skipped.empty() ? "(none)" : skipped);
}
