#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

/* A frame too long for a classic CAN interface must not be reported as a full buffer.
 *
 * The CAN_NATIVE and CAN_ADDON_MCP2515 transmit cases already refuse a frame whose DLC
 * exceeds the 8 bytes classic CAN carries - a CAN-FD battery configured on a classic
 * interface produces these, and copying one would overrun the driver frame on the stack.
 * The refusal was reported by setting the same flag a failed send sets, so it surfaced as
 * "CAN failed to send. Buffer full or no one on the bus to ACK the message!".
 *
 * It is none of those things. It is not a buffer, not the bus, and not transient: nothing
 * validates the battery/interface pairing (comm_nvm.cpp's readIf() maps the stored BATTCOMM
 * to an interface without consulting the battery, and no driver declares that it needs FD),
 * so the setting drops EVERY frame that driver emits for as long as it stands. The user is
 * sent to check wiring for a fault that lives in the settings page.
 *
 * The reporting half links into this binary and is tested for real. The guards themselves
 * live in comm_can.cpp, which is not part of this build - test/emul SUPPLIES
 * transmit_can_frame_to_interface, so the real one is not merely absent, it is replaced - so
 * they are read from the source.
 */
namespace {

/* Comments carry the words these assertions match on, and each guard here sits under a
 * rationale block that describes it in those words - so without this a guard deleted from
 * the code but left in prose would satisfy its own test. Stripped at the source rather than
 * at each call site, so an assertion added later cannot forget to. It also makes
 * brace_block() below count only real braces.
 */
std::string without_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src.compare(i, 2, "/*") == 0) {
      const size_t end = src.find("*/", i + 2);
      i = (end == std::string::npos) ? src.size() : end + 2;
    } else if (src.compare(i, 2, "//") == 0) {
      const size_t end = src.find('\n', i);
      i = (end == std::string::npos) ? src.size() : end;
    } else {
      out += src[i++];
    }
  }
  return out;
}

std::string comm_can_source() {
  // Located relative to this file rather than through a CMake define, so the test needs no
  // build-system plumbing to run.
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/../Software/src/communication/can/comm_can.cpp";
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "comm_can.cpp is where this test looks: " << path;
  return without_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
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

// Everything in one transmit case that runs before its driver is called.
std::string case_before_driver(const std::string& src, const std::string& case_label, const std::string& driver_call) {
  const size_t fn = src.find("void transmit_can_frame_to_interface(");
  EXPECT_NE(fn, std::string::npos) << "transmit_can_frame_to_interface() is not where this test looks";
  if (fn == std::string::npos) {
    return "";
  }
  const size_t at = src.find(case_label, fn);
  EXPECT_NE(at, std::string::npos) << case_label << " is gone from the transmit switch";
  const size_t driver = src.find(driver_call, at);
  EXPECT_NE(driver, std::string::npos) << driver_call << " is gone - re-check this test";
  if (at == std::string::npos || driver == std::string::npos) {
    return "";
  }
  return src.substr(at, driver - at);
}

struct ClassicPath {
  const char* case_label;                   // where the interface's case starts
  const char* driver_call;                  // the send an overlong frame must never reach
  const char* payload;                      // the driver payload whose size is the limit
  const char* report;                       // the flag the guard raises
  const char* wrong_report;                 // the flag it used to raise instead, still correct for a real send failure
  bool DATALAYER_SYSTEM_INFO_TYPE::* flag;  // the same flag, to drive the reporting half
  bool DATALAYER_SYSTEM_INFO_TYPE::* wrong_flag;
  EVENTS_ENUM_TYPE event;
  EVENTS_ENUM_TYPE wrong_event;
  CAN_Interface interface;
};

/* The two classic interfaces, driven as data so the same properties are checked the same way
 * for each rather than two near-identical cases drifting apart. The CAN-FD cases are absent on
 * purpose: they memcpy under std::min(DLC, sizeof(data)) against a 64-byte payload, which is
 * the whole FD range, so they have no overlong frame to refuse.
 */
const std::vector<ClassicPath>& classic_paths() {
  static const std::vector<ClassicPath> paths = {
      {"case CAN_NATIVE:", "ACAN_ESP32::can.tryToSend", "sizeof(CANMessage::data)", "can_native_frame_too_long",
       "can_native_send_fail", &DATALAYER_SYSTEM_INFO_TYPE::can_native_frame_too_long,
       &DATALAYER_SYSTEM_INFO_TYPE::can_native_send_fail, EVENT_CAN_NATIVE_FRAME_TOO_LONG, EVENT_CAN_NATIVE_BUFFER_FULL,
       CAN_NATIVE},
      {"case CAN_ADDON_MCP2515:", "can2515->sendFrame", "sizeof(MCP2515_Lite_Frame::data)", "can_2515_frame_too_long",
       "can_2515_send_fail", &DATALAYER_SYSTEM_INFO_TYPE::can_2515_frame_too_long,
       &DATALAYER_SYSTEM_INFO_TYPE::can_2515_send_fail, EVENT_CANMCP2515_FRAME_TOO_LONG, EVENT_CANMCP2515_BUFFER_FULL,
       CAN_ADDON_MCP2515},
  };
  return paths;
}

class CanFrameTooLongReportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    datalayer = DataLayer();
    // init_events() is what assigns the levels; reset_all_events() only clears state.
    init_events();
    reset_all_events();
    init_hal();
    // Avoid tripping the low-heap check (CPU_free_heap defaults to 0)
    datalayer.system.info.CPU_free_heap = 200000;
    /* The BMS-reset ignore windows are file-static in events.cpp and reset_all_events() does
     * NOT clear them, so the window one test opens suppresses another test's buffer-full event
     * if it happens to run afterwards. Found by the shuffle seeds, which is what they are for.
     * Closing them here rather than in the one test that opens one, because any test in this
     * binary can leave one open.
     */
    for (uint8_t i = 0; i < NO_CAN_INTERFACE; i++) {
      ignore_can_errors_for((CAN_Interface)i, 0);
    }
  }
};

}  // namespace

// --- The guards, read from the source ----------------------------------------

TEST(CanFrameTooLongGuardSource, EachClassicPathRefusesAnOverlongFrameBeforeCallingItsDriver) {
  const std::string src = comm_can_source();
  for (const ClassicPath& path : classic_paths()) {
    const std::string before = case_before_driver(src, path.case_label, path.driver_call);
    EXPECT_NE(before.find(std::string("if (tx_frame->DLC > ") + path.payload + ")"), std::string::npos)
        << path.case_label << ": the DLC must be checked against the driver's own payload size before the "
        << "frame is built, because copying an FD-length frame into it overruns the stack";
  }
}

/* The whole point of the change: the branch an overlong frame takes must report a
 * configuration error and nothing else.
 *
 * Scoped to the guard's OWN block rather than to everything before the send, because the case
 * still contains a legitimate `can_*_send_fail` for a send that genuinely failed - which is
 * the next test.
 */
TEST(CanFrameTooLongGuardSource, TheOverlongPathReportsItselfAndNotAFullBuffer) {
  const std::string src = comm_can_source();
  for (const ClassicPath& path : classic_paths()) {
    const std::string before = case_before_driver(src, path.case_label, path.driver_call);
    const size_t guard = before.find(std::string("if (tx_frame->DLC > ") + path.payload + ")");
    ASSERT_NE(guard, std::string::npos) << path.case_label;
    const std::string body = brace_block(before, guard);
    EXPECT_NE(body.find(path.report), std::string::npos)
        << path.case_label << ": an overlong frame must be reported as one";
    EXPECT_EQ(body.find(path.wrong_report), std::string::npos)
        << path.case_label << ": an overlong frame must not raise " << path.wrong_report
        << " - that is the buffer-full message, and this is neither a buffer nor the bus";
  }
}

/* This change NARROWS what the buffer-full event means. It must not stop meaning anything: a
 * send that actually failed is still the one condition that event is for.
 */
TEST(CanFrameTooLongGuardSource, AGenuineSendFailureStillReportsAFullBuffer) {
  const std::string src = comm_can_source();
  for (const ClassicPath& path : classic_paths()) {
    const size_t fn = src.find("void transmit_can_frame_to_interface(");
    const size_t at = src.find(path.case_label, fn);
    ASSERT_NE(at, std::string::npos) << path.case_label;
    const size_t driver = src.find(path.driver_call, at);
    ASSERT_NE(driver, std::string::npos) << path.driver_call;
    // The send check and its report, i.e. everything from the driver call to the end of the case.
    const std::string after = src.substr(driver, brace_block(src, at).size());
    EXPECT_NE(after.find(path.wrong_report), std::string::npos)
        << path.case_label << ": a failed send must still raise " << path.wrong_report;
  }
}

// --- The reporting half, for real --------------------------------------------

TEST_F(CanFrameTooLongReportTest, TheFlagIsConsumedAndRaisesItsOwnEvent) {
  for (const ClassicPath& path : classic_paths()) {
    reset_all_events();
    datalayer.system.info.*(path.flag) = true;
    update_machineryprotection();
    EXPECT_EQ(get_event_pointer(path.event)->state, EVENT_STATE_ACTIVE)
        << "the dropped frame must reach the user as " << get_event_enum_string(path.event);
    EXPECT_FALSE(datalayer.system.info.*(path.flag))
        << "the flag must be consumed, so the event clears when the condition stops";
    EXPECT_NE(get_event_pointer(path.wrong_event)->state, EVENT_STATE_ACTIVE)
        << "an overlong frame must not also report a full buffer";
  }
}

TEST_F(CanFrameTooLongReportTest, TheEventClearsOnceNoFrameIsRefused) {
  for (const ClassicPath& path : classic_paths()) {
    reset_all_events();
    datalayer.system.info.*(path.flag) = true;
    update_machineryprotection();
    ASSERT_EQ(get_event_pointer(path.event)->state, EVENT_STATE_ACTIVE);
    update_machineryprotection();
    EXPECT_NE(get_event_pointer(path.event)->state, EVENT_STATE_ACTIVE)
        << "a settings fix must stop showing, or the events page keeps a fault nobody has";
  }
}

/* The level decision, pinned. WARNING is not a shrug: EVENT_LEVEL_ERROR is what
 * update_bms_status() turns into system_status = FAULT, which comm_contactorcontrol.cpp
 * counts into MAX_ALLOWED_FAULT_TICKS and answers with SHUTDOWN_REQUESTED - it opens the
 * contactors, ten seconds later.
 *
 * The reason is NOT that EVENT_CAN_BATTERY_MISSING delivers that shutdown instead; see
 * NoOtherEventShutsDownWhileThePackKeepsBroadcasting below for why that backstop does not
 * exist. It is that the frames being dropped are the ones that would bring the pack online at
 * all, so the contactors never close and there is nothing to shut down - while escalating
 * would put every inverter protocol that reads system_status into FAULT behaviour.
 */
TEST_F(CanFrameTooLongReportTest, TheDiagnosisDoesNotOpenContactorsOnItsOwn) {
  for (const ClassicPath& path : classic_paths()) {
    reset_all_events();
    datalayer.system.info.*(path.flag) = true;
    update_machineryprotection();
    EXPECT_EQ(get_event_pointer(path.event)->level, EVENT_LEVEL_WARNING)
        << get_event_enum_string(path.event)
        << " must stay WARNING: ERROR here would drive system_status = FAULT and request a "
           "contactor shutdown over a settings mistake that has already left the pack inert";
    EXPECT_NE(datalayer.system.status.system_status, FAULT);
  }
}

/* The backstop the level argument was first written around does not exist, and this is what
 * says so rather than a comment claiming it.
 *
 * EVENT_CAN_BATTERY_MISSING is raised by check_can_component_alive() when
 * CAN_battery_still_alive reaches zero, and every battery driver refreshes that counter from
 * RECEIVED frames. Losing our TRANSMIT frames therefore silences the pack only if the pack is
 * purely request/response - and the driver this condition is most reachable through is not:
 * KIA-E-GMP refreshes it on 0x055, 0x150, 0x1F5, 0x215, 0x21A and 0x235, none of which it ever
 * transmits. So a pack that keeps broadcasting keeps the counter alive, and nothing escalates,
 * however long the misconfiguration stands.
 */
TEST_F(CanFrameTooLongReportTest, NoOtherEventShutsDownWhileThePackKeepsBroadcasting) {
  /* check_can_component_alive() runs inside safety.cpp's `if (battery)` block, so without a
   * real battery this test would pass by never reaching the code it is about. */
  if (!battery) {
    user_selected_battery_type = BatteryType::BmwI3;
    setup_battery();
  }
  ASSERT_NE(battery, nullptr) << "no battery: the alive check would not run and this would prove nothing";
  battery_detected = true;

  for (const ClassicPath& path : classic_paths()) {
    reset_all_events();
    // A pack that broadcasts unprompted: every pass refreshes the counter, as a received frame
    // does, while every frame WE send is being refused.
    for (int pass = 0; pass < 120; pass++) {
      datalayer.battery.status.CAN_battery_still_alive = CAN_STILL_ALIVE;
      datalayer.system.info.*(path.flag) = true;
      update_machineryprotection();
    }

    EXPECT_NE(get_event_pointer(EVENT_CAN_BATTERY_MISSING)->state, EVENT_STATE_ACTIVE)
        << "after 120 passes - twice the 60-count window - a broadcasting pack has kept "
           "CAN_battery_still_alive refreshed, so there is no second event to escalate on our "
           "behalf and "
        << get_event_enum_string(path.event) << " is the only thing the user gets";
    EXPECT_NE(datalayer.system.status.system_status, FAULT);
  }
}

// The message has to send the user to the settings page, not to the wiring.
TEST_F(CanFrameTooLongReportTest, TheMessageNamesTheConfigurationRatherThanTheBus) {
  for (const ClassicPath& path : classic_paths()) {
    const std::string message = std::string(get_event_message_string(path.event).c_str());
    EXPECT_NE(message.find("CAN-FD"), std::string::npos) << message;
    EXPECT_EQ(message.find("Buffer full"), std::string::npos)
        << "this is the message the change exists to stop showing: " << message;
  }
}

/* A genuine send failure must still be reported as a full buffer, for real and not only in the
 * source. This narrows that event; it must not empty it.
 */
TEST_F(CanFrameTooLongReportTest, AFailedSendStillReportsAFullBuffer) {
  for (const ClassicPath& path : classic_paths()) {
    reset_all_events();
    datalayer.system.info.*(path.wrong_flag) = true;
    update_machineryprotection();
    EXPECT_EQ(get_event_pointer(path.wrong_event)->state, EVENT_STATE_ACTIVE)
        << "a send that actually failed is still what " << get_event_enum_string(path.wrong_event) << " is for";
    EXPECT_NE(get_event_pointer(path.event)->state, EVENT_STATE_ACTIVE);
  }
}

/* Deliberately NOT in is_can_error_of_interface(): that maps each interface to its two
 * TRANSIENT comm errors so a deliberate BMS reset can suppress them while the peer is away. A
 * frame that cannot fit the interface is neither transient nor caused by the reset, and hiding
 * it during the window someone is most likely watching is the opposite of the point.
 */
TEST_F(CanFrameTooLongReportTest, ABmsResetWindowDoesNotHideAConfigurationError) {
  for (const ClassicPath& path : classic_paths()) {
    reset_all_events();
    ignore_can_errors_for(path.interface, 10000);
    datalayer.system.info.*(path.flag) = true;
    update_machineryprotection();
    EXPECT_EQ(get_event_pointer(path.event)->state, EVENT_STATE_ACTIVE)
        << get_event_enum_string(path.event)
        << " must survive a BMS-reset ignore window - it is not a transient comm error";
  }
}
