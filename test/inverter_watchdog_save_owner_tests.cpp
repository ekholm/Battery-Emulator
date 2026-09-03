#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "utils/source_scan.h"

/* The inverter watchdog period is persisted, and WHICH TASK persists it is the
 * property under test.
 *
 * A settings write blocks the task that issues it for as long as the flash
 * operation takes - measured on a T-CAN485 at 4.6 ms typically and 98.6 ms when
 * the NVS page compacts, the same magnitude as a flash block erase. The core
 * task drives CAN, so that stall is a receive gap there; the connectivity loop
 * drives WiFi and the webserver, where the same stall delays a poll.
 *
 * So the save belongs on the connectivity loop, and it must not drift back. The
 * test is a source scan because neither file is in this binary: Software.cpp is
 * excluded from the host build by construction, and comm_nvm.cpp reaches for
 * the webserver, WiFi and MQTT headers the host build deliberately omits. That
 * is precisely why the placement could go wrong with nothing noticing.
 */

namespace {

// `path` is relative to the test directory.
std::string read_source_at(const std::string& caller_file, const std::string& path) {
  const std::string dir = caller_file.substr(0, caller_file.find_last_of('/'));
  const std::string full = dir + "/" + path;
  std::ifstream src(full);
  EXPECT_TRUE(src.is_open()) << "this test reads " << full;
  return strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

// The `{ ... }` block that follows `at`, by brace depth.
std::string brace_block_at(const std::string& src, size_t at) {
  const size_t open = src.find('{', at);
  if (open == std::string::npos) {
    return "";
  }
  int depth = 0;
  for (size_t i = open; i < src.size(); ++i) {
    if (src[i] == '{') {
      ++depth;
    } else if (src[i] == '}') {
      if (--depth == 0) {
        return src.substr(open, i - open + 1);
      }
    }
  }
  return "";
}

std::string software_cpp() {
  return read_source_at(__FILE__, "../Software/Software.cpp");
}

std::string comm_nvm_cpp() {
  return read_source_at(__FILE__, "../Software/src/communication/nvm/comm_nvm.cpp");
}

std::string inverters_h() {
  return read_source_at(__FILE__, "../Software/src/inverter/INVERTERS.h");
}

constexpr const char* kStore = "store_settings_inverter_watchdog";

}  // namespace

// The point of the change, stated as an absence. core_loop is the CAN owner.
TEST(InverterWatchdogSaveOwner, TheCoreLoopDoesNotPersistTheWatchdogPeriod) {
  const std::string body = required_function_body(software_cpp(), "void core_loop(");
  ASSERT_FALSE(body.empty());
  EXPECT_EQ(body.find(kStore), std::string::npos)
      << "the settings write is back on the CAN owner; a flash stall there is a receive gap";
}

// ...and the same point stated as a presence, so deleting the call outright
// fails too rather than satisfying the test above.
TEST(InverterWatchdogSaveOwner, TheConnectivityLoopPersistsTheWatchdogPeriod) {
  const std::string body = required_function_body(software_cpp(), "void connectivity_loop(");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find(kStore), std::string::npos)
      << "nothing drains the flag any more, so a period an inverter declared is lost on reboot";
}

// Exactly one task performs it. Two drainers would race the flag and could
// write the same value twice, and the "not on core_loop" test alone allows it.
TEST(InverterWatchdogSaveOwner, ExactlyOneTaskPerformsTheSave) {
  const std::string src = software_cpp();
  size_t count = 0;
  for (size_t at = src.find(kStore); at != std::string::npos; at = src.find(kStore, at + 1)) {
    ++count;
  }
  EXPECT_EQ(count, 1u) << "Software.cpp names the save " << count << " times; it is one task's job";
}

/* The flag crosses a CORE boundary now: the inverter driver raises it on the
 * core task (pinned to CORE_FUNCTION_CORE) and the connectivity loop takes it
 * (pinned to WIFICORE). What has to hold is an ORDERING, not just a fresh read
 * - the driver writes the period and THEN raises the flag, and the drainer must
 * see the period the driver published rather than the previous one. A volatile
 * flag orders itself only against other volatile accesses, so the plain store
 * to the period may be observed after it; the drainer would then persist the
 * old period and clear the flag, losing the new one until an inverter declares
 * a different value again. Three tests: the flag's type, the driver's half of
 * the release/acquire pairing, and the drainer's.
 */
TEST(InverterWatchdogSaveOwner, TheFlagIsAtomicBecauseTwoCoresTouchIt) {
  EXPECT_NE(inverters_h().find("extern std::atomic<bool> inverter_modbus_watchdog_changed;"), std::string::npos)
      << "volatile is not enough here: it does not order the period's write against the flag's";
}

TEST(InverterWatchdogSaveOwner, TheDriverPublishesThePeriodBeforeRaisingTheFlag) {
  const std::string src = read_source_at(__FILE__, "../Software/src/inverter/BYD-MODBUS.cpp");
  const size_t period = src.find("inverter_modbus_watchdog_timeout_s = declared_timeout_s;");
  ASSERT_NE(period, std::string::npos) << "the driver no longer publishes the period here";
  const size_t flag = src.find("inverter_modbus_watchdog_changed.store(true, std::memory_order_release)");
  ASSERT_NE(flag, std::string::npos) << "the flag is raised without a release store; the period may trail it";
  EXPECT_LT(period, flag) << "the flag is raised before the period is written, which inverts the pairing";
}

TEST(InverterWatchdogSaveOwner, TheStoreTakesTheFlagWithAcquireInOneOperation) {
  const std::string body = required_function_body(comm_nvm_cpp(), "void store_settings_inverter_watchdog(");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("inverter_modbus_watchdog_changed.exchange(false, std::memory_order_acquire)"), std::string::npos)
      << "a separate test-then-clear reopens the window the exchange closes, and drops the acquire "
         "that pairs with the driver's release";
}

/* What the move must NOT change, part one: the store still clears the flag
 * itself. The caller's check is an optimisation, not the guard - if clearing
 * moved out to the caller, a value arriving mid-write would be swallowed.
 */
TEST(InverterWatchdogSaveOwner, TheStoreStillClearsTheFlagItself) {
  const std::string body = required_function_body(comm_nvm_cpp(), "void store_settings_inverter_watchdog(");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("inverter_modbus_watchdog_changed.exchange(false"), std::string::npos)
      << "the store no longer clears the flag itself; a value arriving mid-write would be swallowed";
}

/* What the move must NOT change, part two: the never-write-an-unchanged-value
 * guard. saveUInt() skips an unchanged key on its own, but it WRITES when the
 * key is missing, which would put the default into flash the first time an
 * inverter declares a period.
 */
TEST(InverterWatchdogSaveOwner, TheStoreStillRefusesToWriteAValueNvmAlreadyHolds) {
  const std::string body = required_function_body(comm_nvm_cpp(), "void store_settings_inverter_watchdog(");
  ASSERT_FALSE(body.empty());
  const size_t guard = body.find("!= inverter_modbus_watchdog_timeout_s");
  ASSERT_NE(guard, std::string::npos) << "the unchanged-value guard is gone";
  const std::string guarded = brace_block_at(body, guard);
  EXPECT_NE(guarded.find("saveUInt"), std::string::npos) << "saveUInt is no longer inside the guard";
}

// What the move must NOT change, part three: the boot-time restore.
TEST(InverterWatchdogSaveOwner, TheBootTimeRestoreStillReadsTheStoredPeriod) {
  const std::string src = comm_nvm_cpp();
  EXPECT_NE(src.find("inverter_modbus_watchdog_timeout_s = settings.getUInt(\"INVWDTMO\""), std::string::npos);
}
