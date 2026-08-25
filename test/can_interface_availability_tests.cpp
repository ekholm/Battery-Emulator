#include <gtest/gtest.h>

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
std::string available_interfaces_body(const std::string& source) {
  const size_t at = source.find("available_interfaces()");
  EXPECT_NE(at, std::string::npos) << "the board does not implement available_interfaces()";
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = source.find('{', at);
  const size_t close = source.find('}', open);
  return source.substr(open, close - open);
}

}  // namespace

TEST(CanInterfaceAvailability, TheStarkDeclaresThePopulatedFdChip) {
  // Chip 1 is real - CS=GPIO18, INT=GPIO35 - and has been driven on silicon (wq185 ran it in
  // internal loopback on this very board). A board that has it must be able to offer it.
  const std::string body = available_interfaces_body(read_source("hw_stark.h"));
  EXPECT_NE(body.find("CanFdAddonMcp2518"), std::string::npos)
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
