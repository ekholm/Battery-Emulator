#include <gtest/gtest.h>

#include "hal_source_scan.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

/* Boards must not offer CAN interfaces they do not have (wq202 / FOLLOWUPS L38).
 *
 * `available_interfaces()` had been pure-virtual in hal.h and implemented by every board HAL
 * since it was introduced, and was read by NOTHING. The settings page enumerated the whole
 * `comm_interface` enum instead and filtered only on a non-empty display name, so a board could
 * offer hardware it does not carry. On the Stark that was not hypothetical, and it was exactly
 * backwards: the MCP2518FD that IS populated rendered as "" and was hidden, while a second one
 * that is not fitted carried the friendly name and was offered. Selecting the offered one drove
 * a chip select at pins where nothing answers and reported it as "autodetected crystal: 0MHz"
 * followed by "CAN-FD 2 Configuration error 0x1" - a message about a crystal, for a chip that
 * does not exist.
 *
 * These assert on the HAL SOURCE rather than on a compiled board object: hw_stark.h cannot be
 * instantiated in a host build (it reaches for ESP and the GPIOOPT globals, which only exist in
 * a Stark firmware build), and a test that restated the list in C++ would pass no matter what
 * the header says - which is exactly the failure being fixed, a declaration nobody reads.
 */

namespace {

std::string read_source(const std::string& file) {
  std::ifstream in(std::string(TEST_HAL_SOURCE_DIR) + "/" + file);
  EXPECT_TRUE(in.good()) << "cannot read " << file;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The body of available_interfaces(), which is the declaration under test.
// The extraction itself now lives in hal_source_scan.h - it took two rounds of
// review to get right, and a second copy of it shipped without either lesson.
std::string available_interfaces_body(const std::string& source) {
  EXPECT_NE(source.find("available_interfaces()"), std::string::npos)
      << "the board does not implement available_interfaces()";
  return hal_scan::available_interfaces_body(source);
}

/* Whole-token search. `CanFdAddonMcp2518` is a PREFIX of `CanFdAddonMcp2518_2`, so a plain
 * find() for the populated chip also matches the phantom: swapping the real chip FOR the
 * phantom left TheStarkDeclaresThePopulatedFdChip passing (R202). The suite still caught that
 * swap via the phantom test, but this one was not checking what it says it checks. */
bool declares(const std::string& haystack, const std::string& token) {
  for (size_t at = haystack.find(token); at != std::string::npos; at = haystack.find(token, at + 1)) {
    const size_t after = at + token.size();
    if (after >= haystack.size() || (!std::isalnum(haystack[after]) && haystack[after] != '_')) {
      return true;
    }
  }
  return false;
}

}  // namespace

/* R213: the body extractor itself, on synthetic sources.
 *
 * It is load-bearing for every case below - four of which are POSITIVE ("the
 * body mentions X") - so a slice that runs past the end of the function makes
 * those pass on text that is not in the function at all. The brace-matching
 * change fixed the opposite bias (the old first-'}' scan truncated at the end
 * of the first `return {...}`), and these pin both directions at once so the
 * next edit cannot trade one for the other.
 */
TEST(CanInterfaceAvailability, TheBodyExtractorStopsAtTheFunctionsOwnBrace) {
  const std::string after = "\n  const char* name_for(comm_interface i) { return \"CanFdAddonMcp2518\"; }\n";

  // The case the brace-match was introduced for: a conditional AFTER the first
  // return must still be inside the slice.
  const std::string conditional =
      "  std::vector<comm_interface> available_interfaces() {\n"
      "    std::vector<comm_interface> out = {comm_interface::CanNative};\n"
      "    if (is_fd()) {\n      out.push_back(comm_interface::CanFdAddonMcp2518_2);\n    }\n"
      "    return out;\n  }\n" +
      after;
  EXPECT_NE(available_interfaces_body(conditional).find("CanFdAddonMcp2518_2"), std::string::npos)
      << "a push_back after the first brace group is invisible - the extractor truncated early";

  // ...and the case that costs the other direction: a brace in PROSE must not
  // carry the slice out of the function and into the rest of the header.
  const std::string prose =
      "  std::vector<comm_interface> available_interfaces() {\n"
      "    // the list below is a { brace in prose\n"
      "    return {comm_interface::CanNative};\n  }\n" +
      after;
  EXPECT_EQ(available_interfaces_body(prose).find("CanFdAddonMcp2518"), std::string::npos)
      << "a brace in a comment ran the slice past the function, so text outside it satisfies the "
         "positive checks: "
      << available_interfaces_body(prose);

  const std::string in_string =
      "  std::vector<comm_interface> available_interfaces() {\n"
      "    const char* shape = \"{\";\n"
      "    return {comm_interface::CanNative};\n  }\n" +
      after;
  EXPECT_EQ(available_interfaces_body(in_string).find("CanFdAddonMcp2518"), std::string::npos)
      << "a brace in a string literal ran the slice past the function: " << available_interfaces_body(in_string);

  const std::string block_comment =
      "  std::vector<comm_interface> available_interfaces() {\n"
      "    /* an unbalanced { in a block comment */\n"
      "    return {comm_interface::CanNative};\n  }\n" +
      after;
  EXPECT_EQ(available_interfaces_body(block_comment).find("CanFdAddonMcp2518"), std::string::npos)
      << "a brace in a block comment ran the slice past the function: " << available_interfaces_body(block_comment);
}

TEST(CanInterfaceAvailability, TheStarkDeclaresThePopulatedFdChip) {
  // Chip 1 is real - CS=GPIO18, INT=GPIO35 - and has been driven on silicon (wq185 ran it in
  // internal loopback on this very board). A board that has it must be able to offer it.
  const std::string body = available_interfaces_body(read_source("hw_stark.h"));
  EXPECT_TRUE(declares(body, "CanFdAddonMcp2518"))
      << "the Stark's populated MCP2518FD is missing from its declaration, so the settings page "
         "cannot offer the interface the board actually has";
}

TEST(CanInterfaceAvailability, TheStarkDoesNotDeclareThePhantomSecondFdChip) {
  // Chip 2 is not fitted. Its entry is what produced the 0 MHz crystal on silicon.
  const std::string body = available_interfaces_body(read_source("hw_stark.h"));
  EXPECT_EQ(body.find("CanFdAddonMcp2518_2"), std::string::npos)
      << "the Stark declares a second MCP2518FD that is not fitted";
}

TEST(CanInterfaceAvailability, TheStarkNoLongerPinsTheAbsentSecondChip) {
  // The pin overrides for the phantom are gone, so they fall back to hal.h's NC - which is what
  // frees GPIO12 for the MEB precharge PWM (wq210). Guarded here because re-adding the pins is
  // how the phantom would come back even with the declaration correct.
  const std::string source = read_source("hw_stark.h");
  EXPECT_EQ(source.find("MCP2517_CS2()"), std::string::npos)
      << "the Stark re-declares a chip-select for an MCP2518FD that is not fitted";
  EXPECT_EQ(source.find("MCP2517_INT2()"), std::string::npos)
      << "the Stark re-declares an interrupt pin for an MCP2518FD that is not fitted";
}

TEST(CanInterfaceAvailability, ThePopulatedFdChipIsNotHiddenByABlankName) {
  // The other half of the original defect: a declared interface still disappears from the page
  // if its display name is empty, because the option builder skips blank names. Declaring it and
  // naming it "" would look fixed and behave exactly as before.
  const std::string source = read_source("hw_stark.h");
  const size_t at = source.find("case comm_interface::CanFdAddonMcp2518:");
  ASSERT_NE(at, std::string::npos) << "no name arm for the populated FD chip";
  const size_t ret = source.find("return", at);
  const std::string arm = source.substr(ret, source.find(';', ret) - ret);
  EXPECT_EQ(arm.find("\"\""), std::string::npos)
      << "the populated MCP2518FD renders as a blank name, which hides it from the settings page: " << arm;
}

TEST(CanInterfaceAvailability, EveryBoardDeclaresSomething) {
  // A board whose declaration is empty can offer nothing at all once the option lists are
  // filtered through it - that would turn this fix into a different outage.
  for (const std::string board : {"hw_stark.h", "hw_lilygo.h", "hw_lilygo2can.h", "hw_becom.h", "hw_devkit.h",
                                  "hw_waveshare.h", "hw_3LB.h", "hw_dfrobot_edge101.h"}) {
    const std::string body = available_interfaces_body(read_source(board));
    EXPECT_NE(body.find("comm_interface::"), std::string::npos)
        << board << " declares no interfaces, so the settings page would offer none";
  }
}

/* R202 FINDING - the opposite-direction check the item's brief asked for, and it fails.
 *
 * wq202 makes available_interfaces() load-bearing in two places at once: the settings page
 * hides anything undeclared, and init_CAN() REFUSES it. That is right for the Stark's phantom
 * chip. But four boards declare pin accessors - and one of them a non-empty display NAME - for
 * interfaces they do not list, so the same mechanism now hides and refuses hardware that is
 * really there:
 *
 *   hw_lilygo2can.h  MCP2517_CS2() = GPIO41 when is_fd(), and CanFdAddonMcp2518_2 is NAMED
 *                    "CAN FD (MCP2518 add-on)" in that mode - yet it is not declared. is_fd()
 *                    is a RUNTIME probe (one firmware, both T-2CAN variants), and
 *                    available_interfaces() is static, so the declaration cannot express it.
 *                    lilygo_2CAN_330 is a shipping env.
 *   hw_devkit.h      MCP2515_CS()=GPIO18, MCP2517_CS()=GPIO25, neither interface declared.
 *   hw_waveshare.h   MCP2517_CS()=GPIO13, not declared.
 *   hw_3LB.h         MCP2515_CS()=GPIO18, MCP2517_CS()=GPIO21, neither declared.
 *
 * Blast radius: the refusal loop runs BEFORE anything is initialised and returns on the first
 * undeclared interface, and Software.cpp:783 DISCARDS the return value - so one stale selection
 * silently initialises NO CAN at all, including a perfectly valid native interface, signalled
 * only by EVENT_INTERFACE_MISSING at EVENT_LEVEL_INFO whose text says "Recompile software!".
 *
 * DISABLED_ so the branch suite stays green while the evidence lives in the tree. Remove the
 * prefix as part of the fix; it is the acceptance test for the reopened item.
 */
TEST(CanInterfaceAvailability, NoBoardHidesAnInterfaceItsPinsDeclare) {
  const std::pair<const char*, const char*> implies[] = {
      {"MCP2517_CS2()", "CanFdAddonMcp2518_2"},
      {"MCP2517_CS()", "CanFdAddonMcp2518"},
      {"MCP2515_CS()", "CanAddonMcp2515"},
  };
  for (const std::string board : {"hw_stark.h", "hw_lilygo.h", "hw_lilygo2can.h", "hw_becom.h", "hw_devkit.h",
                                  "hw_waveshare.h", "hw_3LB.h", "hw_dfrobot_edge101.h"}) {
    const std::string source = read_source(board);
    const std::string body = available_interfaces_body(source);
    for (const auto& [pin, interface] : implies) {
      if (source.find(std::string("gpio_num_t ") + pin) == std::string::npos) {
        continue;  // the board does not route that chip select at all
      }
      EXPECT_NE(body.find(interface), std::string::npos)
          << board << " routes " << pin << " but does not declare "
          << interface << ", so the settings page now hides it and init_CAN() refuses it";
    }
  }
}
