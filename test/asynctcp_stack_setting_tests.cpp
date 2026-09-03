#include <gtest/gtest.h>

#include <fstream>
#include <string>

/* The project asks AsyncTCP for a 4 KB task stack instead of the library's 16384,
 * and it gets one. What was missing was anything that NOTICES if it stops.
 *
 * AsyncTCP.cpp creates its task with CONFIG_ASYNC_TCP_STACK, and AsyncTCP.h supplies
 * that from CONFIG_ASYNC_TCP_STACK_SIZE behind an #ifndef whose own fallback is
 * 16384. system_settings.h wins only by having been seen first.
 *
 * That is checked in AsyncTCP.h itself now, by an #error on the value the
 * preprocessor actually arrived at - so any route that loses the setting fails the
 * FIRMWARE build, which is a stronger and more honest guard than a host test could
 * be. Note what it deliberately does NOT check: which include supplied the value.
 * Two paths do today (the direct include, and hal.h's chain through datalayer.h),
 * either is sufficient, and a test that pinned one of them would cry wolf when a
 * tidy-up removed the redundant one while the 4096 stayed intact.
 *
 * All this file does is make sure that guard is still there, since a host test
 * cannot evaluate it. It reads the source because neither file is compiled into the
 * host binary.
 */
namespace {

std::string source(const char* relative) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../" + relative;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

const char* kAsyncTcpHeader = "Software/src/lib/mathieucarbou-AsyncTCPSock/src/AsyncTCP.h";

}  // namespace

TEST(AsyncTcpStackSetting, TheProjectStillAsksForItsOwnStackSize) {
  const std::string settings = source("Software/src/system_settings.h");
  EXPECT_NE(settings.find("#define BE_ASYNC_TCP_STACK_SIZE"), std::string::npos)
      << "system_settings.h no longer names the stack size it wants, so AsyncTCP.h's guard has nothing "
         "to compare against and the task silently takes the library default of 16384";
  EXPECT_NE(settings.find("#define CONFIG_ASYNC_TCP_STACK_SIZE BE_ASYNC_TCP_STACK_SIZE"), std::string::npos)
      << "system_settings.h no longer feeds its own value to the library's macro";
}

TEST(AsyncTcpStackSetting, TheLibraryHeaderStillRefusesAnyOtherStackSize) {
  const std::string header = source(kAsyncTcpHeader);
  const size_t fallback = header.find("#ifndef CONFIG_ASYNC_TCP_STACK_SIZE");
  const size_t guard = header.find("#elif CONFIG_ASYNC_TCP_STACK != BE_ASYNC_TCP_STACK_SIZE");
  const size_t unseen = header.find("#if !defined(BE_ASYNC_TCP_STACK_SIZE)");
  ASSERT_NE(fallback, std::string::npos) << "the CONFIG_ASYNC_TCP_STACK_SIZE fallback is gone - retarget this test";
  ASSERT_NE(unseen, std::string::npos)
      << "AsyncTCP.h no longer fails the build when system_settings.h has not been seen, so losing the "
         "setting is silent again";
  ASSERT_NE(guard, std::string::npos)
      << "AsyncTCP.h no longer compares the stack size it arrived at against the one the project asked "
         "for - the 16384 fallback can win with nothing to say so";
  EXPECT_LT(fallback, guard) << "the guard must sit AFTER the fallback, or it checks a value not yet decided";
}
