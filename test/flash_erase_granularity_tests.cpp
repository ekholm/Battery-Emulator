#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/* The erase COMMAND is the unit of CAN starvation, and which command the flash
 * driver issues is decided by one line in the shipping sdkconfig overlay.
 *
 * Nothing in this binary can observe that: the option is compile-time, it is
 * consumed by the IDF flash driver rather than by this firmware, and the cost
 * of losing it is invisible on a desk - a firmware upload simply goes back to
 * erasing in 64 KB blocks and holding the CPU for hundreds of milliseconds at
 * a time, which only a second board on the bus can see. So the guard is the
 * only kind available: read the overlay and assert the selection is still made.
 *
 * The second test is not a restatement of the first. The overlay is applied in
 * file order and a later line wins, so a disabling line ANYWHERE below the
 * enabling one silently turns the option back off while the enabling line is
 * still sitting there being read by whoever looks.
 */

namespace {

const char* const kEraseOption = "CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE";

/* Read verbatim - no comment stripping.
 *
 * An sdkconfig line is turned OFF by being written as a comment
 * (`# CONFIG_X is not set`), so a reader that removed comments first would be
 * unable to tell the two states apart and would pass on a disabled option.
 * Located relative to this file so the test needs no build-system plumbing.
 */
std::vector<std::string> read_overlay_lines() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../sdkconfig.be_size.defaults";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(src, line)) {
    lines.push_back(line);
  }
  return lines;
}

TEST(FlashEraseGranularity, TheOverlaySelectsSectorErasesAndNotBlockErases) {
  const std::vector<std::string> lines = read_overlay_lines();
  int selected = 0;
  for (const std::string& line : lines) {
    if (line == std::string(kEraseOption) + "=y") {
      selected++;
    }
  }
  EXPECT_EQ(selected, 1) << "expected exactly one active `" << kEraseOption << "=y` in "
                         << "sdkconfig.be_size.defaults. Without it Arduino's UpdateClass erases "
                         << "the OTA partition in 64 KB blocks, and a block erase parks both "
                         << "cores for long enough to miss a pack keepalive.";
}

TEST(FlashEraseGranularity, NoLaterLineTurnsTheSelectionBackOff) {
  const std::vector<std::string> lines = read_overlay_lines();
  size_t enabled_at = lines.size();
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i] == std::string(kEraseOption) + "=y") {
      enabled_at = i;
      break;
    }
  }
  ASSERT_LT(enabled_at, lines.size()) << "the enabling line is missing entirely";

  for (size_t i = enabled_at + 1; i < lines.size(); i++) {
    const bool disables =
        lines[i].find(kEraseOption) != std::string::npos && lines[i].find("is not set") != std::string::npos;
    EXPECT_FALSE(disables) << "line " << (i + 1) << " disables `" << kEraseOption << "` after it was selected on line "
                           << (enabled_at + 1) << "; the overlay is applied in file order, so the last line wins.";
  }
}

}  // namespace
