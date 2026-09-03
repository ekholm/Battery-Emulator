#include <gtest/gtest.h>

#include <fstream>
#include <string>

/* The project asks AsyncTCP for a 4 KB task stack, and it gets one - but only
 * because of an include ORDER that nothing else pins.
 *
 * AsyncTCP.cpp creates its task with CONFIG_ASYNC_TCP_STACK, and AsyncTCP.h
 * supplies that from CONFIG_ASYNC_TCP_STACK_SIZE behind an #ifndef whose own
 * fallback is 16384. The 4096 in system_settings.h wins only because
 * AsyncTCP.h includes system_settings.h ABOVE that fallback. Move the include
 * below it - or drop it while tidying a vendored header - and the task
 * silently gets four times the stack the project asked for, with no build
 * error, no warning, and nothing in the source reading any differently.
 *
 * Verified in the emitted code rather than argued from the headers: in
 * _start_asyncsock_task, the third argument to xTaskCreateUniversal is built
 * as `movi.n a12, 1; slli a12, a12, 12` = 4096. Commenting out the
 * system_settings.h define and rebuilding turns that into `slli a12, a12, 14`
 * = 16384, which is what makes this an ordering dependency worth pinning and
 * not a coincidence.
 *
 * This reads the source because neither file is compiled into the host test
 * binary. It is deliberately about the RELATIONSHIP between the two files -
 * asserting the 4096 alone would still pass in the exact arrangement that
 * loses it.
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
  EXPECT_NE(settings.find("#define CONFIG_ASYNC_TCP_STACK_SIZE"), std::string::npos)
      << "system_settings.h no longer sets CONFIG_ASYNC_TCP_STACK_SIZE, so the AsyncTCP task silently "
         "takes the library default of 16384 instead";
}

TEST(AsyncTcpStackSetting, TheLibraryHeaderReadsThatSettingBeforeFallingBack) {
  const std::string header = source(kAsyncTcpHeader);
  const size_t include = header.find("#include \"../../../system_settings.h\"");
  const size_t fallback = header.find("#ifndef CONFIG_ASYNC_TCP_STACK_SIZE");
  ASSERT_NE(include, std::string::npos)
      << "AsyncTCP.h no longer includes system_settings.h - CONFIG_ASYNC_TCP_STACK_SIZE never reaches the "
         "translation unit that creates the task, and the 4096 in system_settings.h becomes decoration";
  ASSERT_NE(fallback, std::string::npos) << "the CONFIG_ASYNC_TCP_STACK_SIZE fallback is gone - retarget this test";
  EXPECT_LT(include, fallback)
      << "AsyncTCP.h includes system_settings.h AFTER its own #ifndef fallback, so the fallback wins and the "
         "task gets 16384 bytes instead of the 4096 the project asks for - the ordering is the whole mechanism";
}

TEST(AsyncTcpStackSetting, NothingElseOverridesTheSizeInThatHeader) {
  const std::string header = source(kAsyncTcpHeader);
  // The fallback must stay guarded: an unconditional #define here would beat
  // system_settings.h no matter where the include sits.
  const size_t fallback = header.find("#ifndef CONFIG_ASYNC_TCP_STACK_SIZE");
  ASSERT_NE(fallback, std::string::npos);
  const size_t define = header.find("#define CONFIG_ASYNC_TCP_STACK_SIZE");
  EXPECT_GT(define, fallback) << "CONFIG_ASYNC_TCP_STACK_SIZE is defined in AsyncTCP.h outside its #ifndef guard, "
                                 "which overrides whatever system_settings.h asked for";
}
