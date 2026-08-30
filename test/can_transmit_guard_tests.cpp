#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

/* A frame handed to the native CAN interface before it came up must not reach the driver.
 *
 * ACAN_ESP32::tryToSend() writes TWAI registers from inside portENTER_CRITICAL. If the
 * peripheral was never taken out of reset - init_native_can() failed, or the interface was
 * never configured - that access faults with interrupts off, which is a double exception, and
 * the watchdog reboots straight back into the same transmit. Measured on silicon: ~45 resets a
 * minute, forever, and on S3 boards every boot is another chance to lose the USB port.
 *
 * Two halves are pinned here. The guard itself lives in comm_can.cpp, which is not part of this
 * binary - it reaches for the ESP32 CAN drivers - so it is read from the source. That is also
 * why the defect lived there unnoticed: the file has no host build to fail. The reporting half
 * runs for real, because safety.cpp does link here.
 */
namespace {

std::string comm_can_source() {
  // Located relative to this file rather than through a CMake define, so the test needs no
  // build-system plumbing to run.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/communication/can/comm_can.cpp";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "comm_can.cpp is where this test looks: " << path;
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

// The brace-balanced block that opens at the first '{' at or after 'from'.
std::string brace_block(const std::string& src, size_t from) {
  const size_t open = src.find('{', from);
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

// Everything in the CAN_NATIVE transmit case that runs before the driver is called. A guard
// that is not in here cannot protect the register writes, whatever else it does.
std::string native_case_before_driver(const std::string& src) {
  const size_t fn = src.find("void transmit_can_frame_to_interface(");
  EXPECT_NE(fn, std::string::npos) << "transmit_can_frame_to_interface() is not where this test looks";
  if (fn == std::string::npos) {
    return "";
  }
  const size_t at = src.find("case CAN_NATIVE:", fn);
  EXPECT_NE(at, std::string::npos) << "the CAN_NATIVE transmit case is gone";
  const size_t driver = src.find("ACAN_ESP32::can.tryToSend", at);
  EXPECT_NE(driver, std::string::npos) << "the native transmit no longer calls tryToSend - re-check this test";
  if (at == std::string::npos || driver == std::string::npos) {
    return "";
  }
  return src.substr(at, driver - at);
}

// Everything inside `function` that runs before it calls `driver_call`. Same idea as
// native_case_before_driver() above, for the functions that reach the native driver outside
// the transmit switch.
std::string body_before_call(const std::string& src, const std::string& function, const std::string& driver_call) {
  const size_t fn = src.find(function);
  EXPECT_NE(fn, std::string::npos) << function << " is not where this test looks";
  if (fn == std::string::npos) {
    return "";
  }
  const std::string body = brace_block(src, fn);
  const size_t call = body.find(driver_call);
  EXPECT_NE(call, std::string::npos) << function << " no longer calls " << driver_call << " - re-check this test";
  if (call == std::string::npos) {
    return "";
  }
  return body.substr(0, call);
}

class CanNativeGuardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    // init_events() is what assigns the levels; reset_all_events() only clears state.
    init_events();
    reset_all_events();
    init_hal();
    // Avoid tripping the low-heap check (CPU_free_heap defaults to 0)
    datalayer.system.info.CPU_free_heap = 200000;
  }
};

}  // namespace

// The load-bearing property: the check happens BEFORE the driver call, not after it and not
// somewhere else in the file. Placing it after tryToSend() would read as a guard and prevent
// nothing.
TEST(CanNativeTransmitGuardSource, UninitializedNativeInterfaceIsRefusedBeforeTheDriverIsCalled) {
  const std::string before_driver = native_case_before_driver(comm_can_source());
  EXPECT_NE(before_driver.find("if (!native_can_initialized)"), std::string::npos)
      << "the native transmit path must refuse an uninitialized interface before touching the TWAI "
         "registers - tryToSend() writes them inside portENTER_CRITICAL, so the fault becomes a "
         "double exception and the board boot-loops";
}

// A guard that reports but falls through still reaches the driver, so the early exit is a
// separate property from the check.
TEST(CanNativeTransmitGuardSource, TheGuardLeavesTheCaseRatherThanFallingThrough) {
  const std::string before_driver = native_case_before_driver(comm_can_source());
  const size_t guard = before_driver.find("if (!native_can_initialized)");
  ASSERT_NE(guard, std::string::npos);
  const std::string body = brace_block(before_driver, guard);
  EXPECT_NE(body.find("break;"), std::string::npos)
      << "the guard must break out of the case; reporting and then falling through into "
         "tryToSend() is the same crash with an event attached";
}

// The interface below it holds a pointer and short-circuits on null; this one has a flag. Both
// must say so rather than dropping frames silently - these boards log nothing without USBENABLED.
TEST(CanNativeTransmitGuardSource, TheRefusalIsCountedRatherThanSilent) {
  const std::string before_driver = native_case_before_driver(comm_can_source());
  const size_t guard = before_driver.find("if (!native_can_initialized)");
  ASSERT_NE(guard, std::string::npos);
  const std::string body = brace_block(before_driver, guard);
  EXPECT_NE(body.find("can_native_not_initialized = true"), std::string::npos)
      << "a dropped frame must be reported; a silent no-op trades a boot loop for an unexplained "
         "dead interface";
}

// The reporting half links here, so it runs rather than being read.
TEST_F(CanNativeGuardTest, DroppedFramesRaiseTheEvent) {
  datalayer.system.info.can_native_not_initialized = true;

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_CAN_NATIVE_NOT_INITIALIZED)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CAN_NATIVE_NOT_INITIALIZED)->level, EVENT_LEVEL_WARNING);
}

// The flag is a latch the safety pass drains, matching the send-fail and bus-error flags beside
// it. If it were not consumed, the event would stay up for the rest of the boot after one drop.
TEST_F(CanNativeGuardTest, TheFlagIsConsumedAndTheEventClearsWhenTransmitsStop) {
  datalayer.system.info.can_native_not_initialized = true;
  update_machineryprotection();
  ASSERT_FALSE(datalayer.system.info.can_native_not_initialized);

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_CAN_NATIVE_NOT_INITIALIZED)->state, EVENT_STATE_INACTIVE);
}

// A healthy interface must not report anything.
TEST_F(CanNativeGuardTest, NothingIsRaisedWhileTheInterfaceIsUp) {
  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_CAN_NATIVE_NOT_INITIALIZED)->occurences, 0);
  EXPECT_EQ(get_event_pointer(EVENT_CAN_NATIVE_NOT_INITIALIZED)->state, EVENT_STATE_INACTIVE);
}

/* The transmit switch was not the only place that reaches the native driver, and the other two
 * were guarded on the WRONG condition.
 *
 * stop_can() and restart_can() - the emulator's CAN pause/resume, reachable from the webserver -
 * gated on `can_receivers.find(CAN_NATIVE) != can_receivers.end()`. A driver REGISTERS on
 * CAN_NATIVE before init_CAN() ever runs, so on exactly the board this item is about - native
 * init failed, a driver bound to it - that condition is true while the peripheral was never
 * enabled. ACAN_ESP32::end() writes TWAI_CMD_REG and TWAI_INT_ENA_REG before disabling the
 * module, which is the same register-access-on-a-dead-peripheral fault as tryToSend().
 *
 * restart_can() was worse than that and needs no assumption about peripheral behaviour to see:
 * settingsespcan is assigned only inside init_native_can(), and both alloc_pins() failures -
 * the pin-conflict class this item names as the trigger - return before reaching it. So the
 * pointer was still nullptr and `*settingsespcan` dereferenced it.
 */
TEST(CanNativeTransmitGuardSource, StoppingCanDoesNotEndAnInterfaceThatNeverStarted) {
  const std::string before = body_before_call(comm_can_source(), "void stop_can()", "ACAN_ESP32::can.end");
  EXPECT_NE(before.find("if (native_can_initialized)"), std::string::npos)
      << "stop_can() must gate on initialization, not on registration - a driver registers on "
         "CAN_NATIVE before init_CAN() runs, so registration is true on a board whose native "
         "interface never came up, and end() writes TWAI registers before disabling the module";
}

TEST(CanNativeTransmitGuardSource, RestartingCanDoesNotDereferenceSettingsThatWereNeverAllocated) {
  const std::string before = body_before_call(comm_can_source(), "void restart_can()", "ACAN_ESP32::can.begin");
  EXPECT_NE(before.find("if (native_can_initialized)"), std::string::npos)
      << "restart_can() must gate on initialization: settingsespcan is only assigned inside "
         "init_native_can(), which the alloc_pins() failure paths return before reaching, so "
         "resuming a paused emulator dereferenced a null pointer";
}
