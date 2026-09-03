#include <gtest/gtest.h>

#include <esp_mac.h>

#include "../Software/src/devboard/network/hostname.h"
#include "alloc_probe.h"

/* The hostname is read on paths that run continuously - syslog_send() calls
 * active_hostname().c_str() once per LINE - and it is already cached. Returning it by value
 * copied 21+ bytes onto the heap every time, because "battery-emulator-" plus four hex digits
 * is well past the 13 characters an Arduino String keeps inline.
 *
 * hostname.cpp is in the host build for these, with esp_read_mac() stubbed in test/emul: the
 * default name is derived from the MAC's last two bytes, so a test that cannot choose them
 * cannot check the formatting at all.
 */
namespace {

class HostnameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    custom_hostname = "";
    emul_set_mac(0x24, 0x6f, 0x28, 0x11, 0xab, 0xcd);
  }
  void TearDown() override { custom_hostname = ""; }
};

}  // namespace

/* The name that goes on the wire, spelled out. mDNS advertises it and every syslog line
 * carries it in the HOSTNAME field, so a change here is visible to anyone's log collector.
 */
TEST_F(HostnameTest, TheDefaultNameIsThePrefixAndTheLastTwoMacBytesInLowercaseHex) {
  EXPECT_EQ(default_hostname(), String("battery-emulator-abcd"));
  EXPECT_EQ(default_hostname().length(), 21) << "21 characters is why this allocated: it is past the SSO line";
}

/* The format, over the MACs that break it. Leading zeroes are where a %x instead of a %02x
 * shows up - and only for some MACs, which is exactly the kind of bug that ships. Checked
 * through hostname_for_mac() because default_hostname() caches its first answer and so cannot
 * be asked twice.
 */
TEST_F(HostnameTest, TheFormatKeepsLeadingZeroesAndUsesLowercaseHex) {
  const uint8_t zeroes[6] = {0x24, 0x6f, 0x28, 0x11, 0x00, 0x0f};
  EXPECT_EQ(hostname_for_mac(zeroes), String("battery-emulator-000f"));

  const uint8_t upper[6] = {0x24, 0x6f, 0x28, 0x11, 0xAB, 0xCD};
  EXPECT_EQ(hostname_for_mac(upper), String("battery-emulator-abcd")) << "lowercase, whatever the byte";

  const uint8_t high[6] = {0x00, 0x00, 0x00, 0x00, 0xff, 0xff};
  EXPECT_EQ(hostname_for_mac(high), String("battery-emulator-ffff"));

  const uint8_t only_last_two[6] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02};
  EXPECT_EQ(hostname_for_mac(only_last_two), String("battery-emulator-0102"))
      << "only the last two bytes are used; the first four must not leak in";

  for (const uint8_t* mac : {zeroes, upper, high, only_last_two}) {
    EXPECT_EQ(hostname_for_mac(mac).length(), 21);
  }
}

/* Both branches of the selection, which is the whole behaviour of active_hostname() and the
 * thing the acceptance asks for: custom set, and custom unset.
 */
TEST_F(HostnameTest, TheCustomNameWinsWhenSetAndTheDefaultWhenNot) {
  EXPECT_EQ(active_hostname(), default_hostname()) << "unset: the MAC-derived default";

  custom_hostname = "garage-pack";
  EXPECT_EQ(active_hostname(), String("garage-pack")) << "set: the user's name, verbatim";

  custom_hostname = "";
  EXPECT_EQ(active_hostname(), default_hostname()) << "cleared: back to the default";
}

/* A reference is only safe if it points at storage the caller cannot outlive. Both do - a
 * function-local static and a global - and this says so in a way that fails if either is ever
 * turned back into a temporary.
 */
TEST_F(HostnameTest, TheReturnedReferenceIsTheStorageItself) {
  EXPECT_EQ(&default_hostname(), &default_hostname()) << "the default must be one object, not a fresh copy";
  EXPECT_EQ(&active_hostname(), &default_hostname()) << "unset, the active name IS the default, not a copy of it";

  custom_hostname = "garage-pack";
  EXPECT_EQ(&active_hostname(), &custom_hostname) << "set, the active name IS custom_hostname, not a copy of it";
}

TEST_F(HostnameTest, TheDefaultIsBuiltOnceAndCached) {
  const String* first = &default_hostname();
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(&default_hostname(), first);
  }
}

/* The point of the item, asserted rather than estimated: reading the hostname must not
 * allocate. syslog_send() does this once per line.
 */
TEST_F(HostnameTest, ReadingTheHostnameDoesNotAllocate) {
#ifdef __SANITIZE_ADDRESS__
  GTEST_SKIP() << "the probe replaces global operator new, which ASAN owns";
#endif
  (void)default_hostname();  // Pay the one-time cache build outside the measurement.

  alloc_probe_reset();
  for (int i = 0; i < 100; i++) {
    volatile size_t len = strlen(active_hostname().c_str());
    (void)len;
  }
  EXPECT_EQ(alloc_probe_count(), 0u) << "the unset path is what syslog_send() takes on a default install";

  custom_hostname = "garage-pack-with-a-long-name";
  alloc_probe_reset();
  for (int i = 0; i < 100; i++) {
    volatile size_t len = strlen(active_hostname().c_str());
    (void)len;
  }
  EXPECT_EQ(alloc_probe_count(), 0u) << "and the custom path must not rebuild a String from bytes it already has";
}
