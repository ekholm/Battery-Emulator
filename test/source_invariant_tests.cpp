// Source-level invariants, checked as tests.
//
// These exist because the things they check are invisible to the compiler and
// to every behavioural test: code can be *correct for one battery* and silently
// wrong for a second one, and nothing fails until someone runs two packs.
//
// They are deliberately RATCHETS, not absolutes. Each carries an allowlist of
// files that do not satisfy the invariant yet. A new violation fails the build;
// fixing an allowlisted file and forgetting to remove it from the list also
// fails, so the list can only shrink.

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The test binary runs from test/build, so the tree root is two levels up.
fs::path repo_root() {
  return fs::path(__FILE__).parent_path().parent_path();
}

std::vector<std::string> lines_of(const fs::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  for (std::string l; std::getline(in, l);) {
    out.push_back(l);
  }
  return out;
}

// Strip // comments so a violation mentioned in prose does not count.
std::string code_of(const std::string& line) {
  auto pos = line.find("//");
  return pos == std::string::npos ? line : line.substr(0, pos);
}

struct Violation {
  std::string file;
  int line;
  std::string text;
};

std::vector<Violation> scan(const fs::path& dir, const std::string& needle, const std::set<std::string>& skip_files) {
  std::vector<Violation> found;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) {
      continue;
    }
    const std::string name = e.path().filename().string();
    const std::string ext = e.path().extension().string();
    if (ext != ".cpp" && ext != ".h") {
      continue;
    }
    if (skip_files.count(name)) {
      continue;
    }
    int n = 0;
    for (const auto& raw : lines_of(e.path())) {
      ++n;
      const auto code = code_of(raw);
      const auto at = code.find(needle);
      if (at == std::string::npos) {
        continue;
      }
      // `&datalayer.batteries[0]...` is the constructor default argument and
      // the primary-instance comparison - both correct, neither is a read of
      // another pack's data.
      if (at > 0 && code[at - 1] == '&') {
        continue;
      }
      found.push_back({name, n, raw});
    }
  }
  return found;
}

}  // namespace

// A battery driver must reach its own instance through datalayer_battery, never
// by indexing the array. `datalayer.batteries[0].field` in a driver body means
// a second pack of that type reads and writes battery 1's data - the failure is
// silent, and only appears when someone configures a dual pack.
//
// `&datalayer.batteries[0]` is NOT a violation: it is the constructor default
// argument and the primary-instance comparison, both correct.
TEST(SourceInvariants, DriversDoNotIndexTheBatteryArrayDirectly) {
  // Files that predate the per-instance conversion. Each is a latent
  // multi-instance defect for a type that IS multi-capable; shrink this list,
  // never grow it.
  // Empty by design: the per-instance conversion is complete. This is now a
  // hard assertion, not a ratchet - a new violation fails immediately.
  const std::set<std::string> known_unconverted = {};
  // Infrastructure, not drivers: the factory and the fake legitimately name
  // instance 0.
  std::set<std::string> skip = {"BATTERIES.cpp", "BATTERIES.h",           "Battery.h",
                                "Battery.cpp",   "TEST-FAKE-BATTERY.cpp", "TEST-FAKE-BATTERY.h"};
  skip.insert(known_unconverted.begin(), known_unconverted.end());

  const auto found = scan(repo_root() / "Software" / "src" / "battery", "datalayer.batteries[0].", skip);

  std::string report;
  for (const auto& v : found) {
    report += "\n  " + v.file + ":" + std::to_string(v.line) + "  " + v.text;
  }
  EXPECT_TRUE(found.empty()) << "Driver(s) index the battery array directly instead of using "
                                "datalayer_battery. A second pack of this type would read and write "
                                "battery 1's data:"
                             << report;

  // The list may only shrink: if an allowlisted file has been converted, it must
  // leave the list, or it silently stops being checked.
  for (const auto& name : known_unconverted) {
    const auto path = repo_root() / "Software" / "src" / "battery" / name;
    if (!fs::exists(path)) {
      continue;  // upstream may delete a driver
    }
    bool still_violates = false;
    for (const auto& raw : lines_of(path)) {
      if (code_of(raw).find("datalayer.batteries[0].") != std::string::npos) {
        still_violates = true;
        break;
      }
    }
    EXPECT_TRUE(still_violates) << name
                                << " no longer indexes the battery array - remove it from "
                                   "known_unconverted so it stays checked.";
  }
}

// The DataLayerExtended singleton was replaced by a per-instance union in the
// battery schema. A reference to the old global means a driver is writing
// shared state again - it compiles only if someone reintroduces the global.
TEST(SourceInvariants, TheRetiredExtendedDataSingletonStaysRetired) {
  for (const auto* dir : {"battery", "devboard", "communication", "inverter"}) {
    const auto base = repo_root() / "Software" / "src" / dir;
    if (!fs::exists(base)) {
      continue;
    }
    for (const auto& e : fs::recursive_directory_iterator(base)) {
      if (!e.is_regular_file()) {
        continue;
      }
      const auto ext = e.path().extension().string();
      if (ext != ".cpp" && ext != ".h") {
        continue;
      }
      int n = 0;
      for (const auto& raw : lines_of(e.path())) {
        ++n;
        const auto code = code_of(raw);
        // Skip the include line: "datalayer_extended.h" is the header that
        // declares the per-instance union types and is legitimately included.
        if (code.find("#include") != std::string::npos) {
          continue;
        }
        // only uses of the retired global matter
        if (code.find("datalayer_extended.") != std::string::npos) {
          ADD_FAILURE() << e.path().filename().string() << ":" << n
                        << " uses the retired datalayer_extended singleton; the per-instance slot is "
                           "datalayer_battery->extended (or the driver's own reference to it):\n  "
                        << raw;
        }
      }
    }
  }
}
