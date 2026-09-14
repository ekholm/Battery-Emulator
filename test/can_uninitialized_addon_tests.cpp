#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"

/* An add-on CAN chip that never started must not be reported as a full buffer.
 *
 * The three add-on transmit paths dropped the frame correctly - a null driver
 * pointer short-circuited the send - but folded that drop into the same flag a
 * failed send sets, so it surfaced as EVENT_CANMCP2515_BUFFER_FULL and friends:
 * "CAN failed to send. Buffer full or no one on the bus to ACK the message!".
 * A chip that is not there is neither, and that message sends the user to check
 * bus wiring for a fault the boot-time init event already named.
 *
 * The reporting half links into this binary and is tested for real. The guards
 * themselves live in comm_can.cpp, which is not part of this build - test/emul
 * SUPPLIES transmit_can_frame_to_interface, so the real one is not merely
 * absent, it is replaced - so they are read from the source.
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

struct AddonPath {
  const char* case_label;    // where the interface's case starts
  const char* driver_call;   // the send that must not be reached with a null driver
  const char* pointer;       // the driver pointer the guard tests
  const char* report;        // the flag the guard raises
  const char* wrong_report;  // the flag that used to be raised instead
};

// The three add-on interfaces, driven as data so the same properties are checked
// the same way for each rather than three near-identical cases drifting apart.
const std::vector<AddonPath>& addon_paths() {
  static const std::vector<AddonPath> paths = {
      {"case CAN_ADDON_MCP2515:", "can2515->sendFrame", "can2515", "can_2515_not_initialized", "can_2515_send_fail"},
      {"case CANFD_ADDON_MCP2518:", "canfd->tryToSend", "canfd", "can_2518_not_initialized", "can_2518_send_fail"},
      {"case CANFD_ADDON_MCP2518_2:", "canfd_2->tryToSend", "canfd_2", "can_2518_2_not_initialized",
       "can_2518_2_send_fail"},
  };
  return paths;
}

class CanAddonReportTest : public ::testing::Test {
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

// --- The guards, read from the source ----------------------------------------

TEST(CanAddonGuardSource, EachAddonRefusesANullDriverBeforeCallingIt) {
  const std::string src = comm_can_source();
  for (const AddonPath& path : addon_paths()) {
    const std::string before = case_before_driver(src, path.case_label, path.driver_call);
    EXPECT_NE(before.find(std::string("if (") + path.pointer + " == nullptr)"), std::string::npos)
        << path.case_label << ": the null check must stand on its own before the send, so the drop can be "
        << "reported as its own condition";
  }
}

/* The whole point of the change: the branch a null pointer takes must report a
 * missing chip and nothing else.
 *
 * Scoped to the guard's OWN block rather than to everything before the send,
 * because the case still contains a legitimate `can_*_send_fail` for a frame too
 * long to travel over this interface. That one is a different wrong diagnosis of
 * the same family and is deliberately left alone here.
 */
TEST(CanAddonGuardSource, TheNullPathReportsAMissingChipAndNotAFullBuffer) {
  const std::string src = comm_can_source();
  for (const AddonPath& path : addon_paths()) {
    const std::string before = case_before_driver(src, path.case_label, path.driver_call);
    const size_t guard = before.find(std::string("if (") + path.pointer + " == nullptr)");
    ASSERT_NE(guard, std::string::npos) << path.case_label;
    const std::string body = brace_block(before, guard);
    EXPECT_NE(body.find(path.report), std::string::npos)
        << path.case_label << ": the missing chip must be reported as one";
    EXPECT_EQ(body.find(path.wrong_report), std::string::npos)
        << path.case_label << ": a missing chip must not raise " << path.wrong_report
        << " - that is the wrong diagnosis this change removes";
    EXPECT_NE(body.find("break;"), std::string::npos)
        << path.case_label << ": the guard must leave the case rather than fall into the send";
  }
}

// The null short-circuit that used to live inside the send check has to be gone,
// not merely duplicated: leaving it would keep the old flag reachable for the
// same condition and make which report the user gets depend on evaluation order.
TEST(CanAddonGuardSource, TheOldNullShortCircuitIsGoneFromTheSendCheck) {
  const std::string src = comm_can_source();
  for (const AddonPath& path : addon_paths()) {
    EXPECT_EQ(src.find(std::string(path.pointer) + " == nullptr || "), std::string::npos)
        << path.pointer << ": the null case is its own branch now; folding it back into the send check "
        << "restores the misdiagnosis";
  }
}

// --- The reporting half, which links here and runs ---------------------------

TEST_F(CanAddonReportTest, AMissingChipRaisesItsOwnEventAndNotTheBufferFullOne) {
  struct Case {
    bool DATALAYER_SYSTEM_INFO_TYPE::* flag;
    EVENTS_ENUM_TYPE raised;
    EVENTS_ENUM_TYPE not_raised;
  };
  const Case cases[] = {
      {&DATALAYER_SYSTEM_INFO_TYPE::can_2515_not_initialized, EVENT_CANMCP2515_NOT_INITIALIZED,
       EVENT_CANMCP2515_BUFFER_FULL},
      {&DATALAYER_SYSTEM_INFO_TYPE::can_2518_not_initialized, EVENT_CANFD_NOT_INITIALIZED, EVENT_CANFD_BUFFER_FULL},
      {&DATALAYER_SYSTEM_INFO_TYPE::can_2518_2_not_initialized, EVENT_CANFD_2_NOT_INITIALIZED,
       EVENT_CANFD_2_BUFFER_FULL},
  };

  for (const Case& c : cases) {
    datalayer = DataLayer();
    reset_all_events();
    datalayer.system.info.CPU_free_heap = 200000;
    datalayer.system.info.*c.flag = true;

    update_machineryprotection();

    EXPECT_EQ(get_event_pointer(c.raised)->state, EVENT_STATE_ACTIVE)
        << get_event_enum_string(c.raised) << " must be raised";
    EXPECT_EQ(get_event_pointer(c.raised)->level, EVENT_LEVEL_WARNING) << get_event_enum_string(c.raised);
    EXPECT_EQ(get_event_pointer(c.not_raised)->state, EVENT_STATE_INACTIVE)
        << get_event_enum_string(c.not_raised) << " is the wrong diagnosis and must stay down";
  }
}

// The flags are latches the safety pass drains, matching the send-fail and
// bus-error flags beside them; if they were not consumed the event would stay up
// for the rest of the boot after one dropped frame.
TEST_F(CanAddonReportTest, TheFlagsAreConsumedAndTheEventsClearWhenTransmitsStop) {
  datalayer.system.info.can_2515_not_initialized = true;
  datalayer.system.info.can_2518_not_initialized = true;
  datalayer.system.info.can_2518_2_not_initialized = true;

  update_machineryprotection();

  ASSERT_FALSE(datalayer.system.info.can_2515_not_initialized);
  ASSERT_FALSE(datalayer.system.info.can_2518_not_initialized);
  ASSERT_FALSE(datalayer.system.info.can_2518_2_not_initialized);

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_CANMCP2515_NOT_INITIALIZED)->state, EVENT_STATE_INACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CANFD_NOT_INITIALIZED)->state, EVENT_STATE_INACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CANFD_2_NOT_INITIALIZED)->state, EVENT_STATE_INACTIVE);
}

TEST_F(CanAddonReportTest, NothingIsRaisedWhileTheChipsAreThere) {
  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_CANMCP2515_NOT_INITIALIZED)->occurences, 0);
  EXPECT_EQ(get_event_pointer(EVENT_CANFD_NOT_INITIALIZED)->occurences, 0);
  EXPECT_EQ(get_event_pointer(EVENT_CANFD_2_NOT_INITIALIZED)->occurences, 0);
}

// A real send failure still reports as one. The change narrows what the
// buffer-full flag means; it must not stop meaning anything.
TEST_F(CanAddonReportTest, ARealSendFailureStillReportsAsAFullBuffer) {
  datalayer.system.info.can_2515_send_fail = true;

  update_machineryprotection();

  EXPECT_EQ(get_event_pointer(EVENT_CANMCP2515_BUFFER_FULL)->state, EVENT_STATE_ACTIVE);
  EXPECT_EQ(get_event_pointer(EVENT_CANMCP2515_NOT_INITIALIZED)->state, EVENT_STATE_INACTIVE);
}

// The three share one message string, so the event NAME is what tells the user
// which interface - and the message must not read like a bus problem, which is
// the whole complaint against the one they used to get.
TEST_F(CanAddonReportTest, TheMessageNamesTheStartupFailureRatherThanTheBus) {
  const std::string message = std::string(get_event_message_string(EVENT_CANMCP2515_NOT_INITIALIZED).c_str());

  EXPECT_EQ(message, std::string(get_event_message_string(EVENT_CANFD_NOT_INITIALIZED).c_str()));
  EXPECT_EQ(message, std::string(get_event_message_string(EVENT_CANFD_2_NOT_INITIALIZED).c_str()));
  EXPECT_NE(message, std::string(get_event_message_string(EVENT_CANMCP2515_BUFFER_FULL).c_str()))
      << "sharing the buffer-full text would be the defect with extra steps";
  EXPECT_EQ(message.find("Buffer full"), std::string::npos);

  // The names carry the distinction the shared string does not.
  EXPECT_NE(std::string(get_event_enum_string(EVENT_CANMCP2515_NOT_INITIALIZED)),
            std::string(get_event_enum_string(EVENT_CANFD_NOT_INITIALIZED)));
  EXPECT_NE(std::string(get_event_enum_string(EVENT_CANFD_NOT_INITIALIZED)),
            std::string(get_event_enum_string(EVENT_CANFD_2_NOT_INITIALIZED)));
}
