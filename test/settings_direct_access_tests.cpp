#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

/* Who is still allowed to reach a setting by key.
 *
 * A call site that names a key repeats three facts the table already states - the
 * spelling, the on-flash NVS type and the default - and NVS punishes the middle one
 * silently: a typed read of a key stored under a different tag returns the default
 * rather than the stored value, so the setting is lost with no symptom. That is the
 * failure the boot audit exists to catch after the fact; the accessors remove the
 * chance to make it, because a call site names only the id.
 *
 * This holds the list of places that still do it and refuses to let the list grow.
 * Migrating a file lowers its entry, and dropping to zero is a failure too, so the
 * progress has to be written down rather than quietly absorbed.
 *
 * Exempt, and permanently: the store itself and the loader. The store IS the
 * key-addressed API, and the loader is the one place whose whole job is to walk rows
 * and address keys - it does so through the table, not by hand.
 */

namespace {

namespace fs = std::filesystem;

// Files that may still address settings by key, and how many times. SHRINK-ONLY.
const std::map<std::string, size_t> DIRECT_ACCESS = {
    {"settings_html.cpp", 127},
    {"webserver.cpp", 38},  // the six literal-key reads migrated; the rest are the generic loops
};

// The key-addressed API itself, and the loader that is built on it.
bool is_exempt(const fs::path& file) {
  const std::string name = file.filename().string();
  return name == "comm_nvm.cpp" || name == "comm_nvm.h" || name == "settings_accessors.h";
}

// Comments stripped by hand and the match found by substring: a regex pass over every
// source in the tree cost five seconds, which is ten times the rest of the suite and
// the reason such a check gets switched off.
std::string strip_comments(const std::string& source) {
  std::string out;
  out.reserve(source.size());
  for (size_t i = 0; i < source.size();) {
    if (source.compare(i, 2, "/*") == 0) {
      const size_t end = source.find("*/", i + 2);
      i = (end == std::string::npos) ? source.size() : end + 2;
    } else if (source.compare(i, 2, "//") == 0) {
      const size_t end = source.find('\n', i);
      i = (end == std::string::npos) ? source.size() : end;
    } else {
      out.push_back(source[i++]);
    }
  }
  return out;
}

size_t count_of(const std::string& haystack, const std::string& needle) {
  size_t count = 0, pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

size_t direct_accesses(const fs::path& file) {
  std::ifstream in(file);
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string stripped = strip_comments(source);

  size_t count = 0;
  for (const char* verb : {"settings.get", "settings.save", "prefs.get", "prefs.put"}) {
    count += count_of(stripped, verb);
  }
  count += count_of(stripped, "Preferences ");  // a raw handle of one's own
  return count;
}

}  // namespace

TEST(SettingsDirectAccessTest, NoNewCallSiteReachesASettingByKey) {
  const fs::path root(TEST_FIRMWARE_SOURCE_ROOT);
  ASSERT_TRUE(fs::is_directory(root)) << "cannot read " << TEST_FIRMWARE_SOURCE_ROOT
                                      << " - this guard is worthless if it scans nothing";

  std::map<std::string, size_t> counted;
  size_t scanned = 0;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    const auto ext = entry.path().extension();
    if (ext != ".cpp" && ext != ".h") {
      continue;
    }
    ++scanned;
    if (is_exempt(entry.path())) {
      continue;
    }
    // Recorded even at zero: a file reaching zero is the case worth reporting.
    counted[entry.path().filename().string()] = direct_accesses(entry.path());
  }
  ASSERT_GT(scanned, 100u) << "only scanned " << scanned << " sources - the scan has lost the tree";

  for (const auto& [file, count] : counted) {
    const auto allowed = DIRECT_ACCESS.find(file);
    if (allowed == DIRECT_ACCESS.end()) {
      EXPECT_EQ(count, 0u) << file << " reaches settings by key " << count
                           << " time(s) and is not on the list. Use setting_get<Sid::X>() / "
                              "setting_save<Sid::X>(): the row supplies the key, the type and the default.";
      continue;
    }
    if (count > allowed->second) {
      ADD_FAILURE() << file << " went from " << allowed->second << " to " << count
                    << " key-addressed accesses. This list only shrinks.";
    } else if (count < allowed->second) {
      ADD_FAILURE() << file << " is down to " << count << " from " << allowed->second << " - lower its entry"
                    << (count == 0 ? " (delete it)." : ".") << " The number is what is left to migrate.";
    }
  }

  for (const auto& [file, expected] : DIRECT_ACCESS) {
    EXPECT_TRUE(counted.count(file) == 1)
        << file << " is on the list but was not scanned - delete its entry (expected " << expected << ").";
  }
}
