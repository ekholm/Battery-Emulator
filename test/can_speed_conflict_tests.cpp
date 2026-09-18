#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "../Software/src/communication/can/CanReceiver.h"
#include "../Software/src/communication/can/can_speed_policy.h"
#include "../Software/src/devboard/utils/events.h"
#include "../Software/src/devboard/utils/types.h"

/* One CAN interface is one wire, so it has exactly one bitrate - but every
 * driver on it asks for its own. Whoever registered first used to win, silently:
 * setup runs charger, inverter, battery, shunt, and the charger can only ask for
 * 500 kbit/s, so a 250 kbit/s battery behind it came up deaf on a 500 kbit/s bus
 * with nothing logged. That is the shape behind the "SBRxxx did not work"
 * reports (discussion dalathegreat#2871).
 *
 * The rule under test: the drivers on an interface must agree, and if they do
 * not, that interface is not started and the event names both of them.
 */

namespace {

// A receiver only has to be a distinct address here - nothing calls it.
class FakeReceiver : public CanReceiver {
 public:
  void receive_can_frame(CAN_frame* rx_frame) override {}
};

FakeReceiver charger_receiver;
FakeReceiver battery_receiver;
FakeReceiver inverter_receiver;

void add(CanReceiverRegistry& registry, CAN_Interface interface, CanReceiver* receiver, const char* name,
         CAN_Speed speed) {
  registry.insert({interface, {receiver, speed, name}});
}

bool event_active(EVENTS_ENUM_TYPE event) {
  return get_event_pointer(event)->state != EVENT_STATE_INACTIVE;
}

std::string message_for(EVENTS_ENUM_TYPE event) {
  return std::string(get_event_message_string(event).c_str());
}

class CanSpeedConflictTest : public ::testing::Test {
 protected:
  void SetUp() override { init_events(); }

  CanReceiverRegistry registry;
  CAN_Speed resolved = CAN_Speed::CAN_SPEED_1000KBPS;  // Poisoned: no case expects this value.
};

}  // namespace

// 1. The ordinary shared interface: two drivers, one bitrate, nothing to report.
TEST_F(CanSpeedConflictTest, TwoRegistrationsThatAgreeResolveToThatSpeed) {
  add(registry, CAN_NATIVE, &charger_receiver, "Chevy Volt Charger", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CAN_NATIVE, &battery_receiver, "Tesla", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_TRUE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  EXPECT_EQ(resolved, CAN_Speed::CAN_SPEED_500KBPS);
  EXPECT_FALSE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

// 2. Disagreement: refused, and the event carries enough to act on.
TEST_F(CanSpeedConflictTest, DisagreementIsRefusedAndNamesBothParties) {
  add(registry, CAN_NATIVE, &inverter_receiver, "Sungrow", CAN_Speed::CAN_SPEED_250KBPS);
  add(registry, CAN_NATIVE, &battery_receiver, "Tesla", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_FALSE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  EXPECT_EQ(resolved, CAN_Speed::CAN_SPEED_1000KBPS) << "the speed out-parameter is untouched on refusal";

  ASSERT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));
  EXPECT_EQ(get_event_pointer(EVENT_CAN_SPEED_CONFLICT)->data, (int16_t)CAN_NATIVE);
  EXPECT_STREQ(get_event_level_string(EVENT_CAN_SPEED_CONFLICT), "ERROR");

  const std::string message = message_for(EVENT_CAN_SPEED_CONFLICT);
  EXPECT_NE(message.find("Sungrow"), std::string::npos) << message;
  EXPECT_NE(message.find("250"), std::string::npos) << message;
  EXPECT_NE(message.find("Tesla"), std::string::npos) << message;
  EXPECT_NE(message.find("500"), std::string::npos) << message;
  EXPECT_LT(message.find("Sungrow"), message.find("Tesla")) << "named in registration order: " << message;
}

// 3. Different interfaces are different buses. Two speeds are normal there.
TEST_F(CanSpeedConflictTest, DifferentInterfacesAtDifferentSpeedsAreNotAConflict) {
  add(registry, CAN_NATIVE, &battery_receiver, "Akasol", CAN_Speed::CAN_SPEED_250KBPS);
  add(registry, CAN_ADDON_MCP2515, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_TRUE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  EXPECT_EQ(resolved, CAN_Speed::CAN_SPEED_250KBPS);
  EXPECT_TRUE(resolve_interface_speed(registry, CAN_ADDON_MCP2515, resolved));
  EXPECT_EQ(resolved, CAN_Speed::CAN_SPEED_500KBPS);
  EXPECT_FALSE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

/* 4. The reported shape, in setup order: the charger registers first and can
      only ask for 500, the 250 kbit/s battery second. Before this change the
      bus came up at 500 and the battery was deaf with nothing logged. */
TEST_F(CanSpeedConflictTest, ChargerFirstThenA250kbpsBatteryIsCaught) {
  add(registry, CAN_NATIVE, &charger_receiver, "Nissan Leaf Charger", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CAN_NATIVE, &battery_receiver, "Akasol", CAN_Speed::CAN_SPEED_250KBPS);

  EXPECT_FALSE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  ASSERT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));

  const std::string message = message_for(EVENT_CAN_SPEED_CONFLICT);
  EXPECT_LT(message.find("Nissan Leaf Charger"), message.find("Akasol"))
      << "the charger registered first, so it is the first party: " << message;
}

// 5. Every registration is compared, not just the first two.
TEST_F(CanSpeedConflictTest, AThirdRegistrationThatDisagreesIsCaught) {
  add(registry, CAN_NATIVE, &charger_receiver, "Chevy Volt Charger", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CAN_NATIVE, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CAN_NATIVE, &battery_receiver, "CellPower", CAN_Speed::CAN_SPEED_250KBPS);

  EXPECT_FALSE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  ASSERT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));
  EXPECT_NE(message_for(EVENT_CAN_SPEED_CONFLICT).find("CellPower"), std::string::npos);
}

// 6a. A runtime change on a shared bus deafens the peer, so it is refused.
TEST_F(CanSpeedConflictTest, RuntimeSpeedChangeIsRefusedWhenAPeerDisagrees) {
  add(registry, CAN_NATIVE, &battery_receiver, "BMW PHEV", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CAN_NATIVE, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_FALSE(
      can_speed_change_allowed(registry, CAN_NATIVE, CAN_Speed::CAN_SPEED_100KBPS, &battery_receiver, "BMW PHEV"));
  ASSERT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));

  const std::string message = message_for(EVENT_CAN_SPEED_CONFLICT);
  EXPECT_NE(message.find("BYD Can"), std::string::npos) << message;
  EXPECT_NE(message.find("100"), std::string::npos) << message;
}

/* 6b. Alone on its interface a driver owns the bitrate, and BMW PHEV's 100/500
       switch has to keep working. Its OWN registration holds the speed it asked
       for at boot, so it must not be counted against it. */
TEST_F(CanSpeedConflictTest, SoleRegistrantMayChangeItsOwnInterface) {
  add(registry, CAN_NATIVE, &battery_receiver, "BMW PHEV", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_TRUE(
      can_speed_change_allowed(registry, CAN_NATIVE, CAN_Speed::CAN_SPEED_100KBPS, &battery_receiver, "BMW PHEV"));
  EXPECT_FALSE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

// 6c. Peers that all want the speed being asked for are no obstacle.
TEST_F(CanSpeedConflictTest, RuntimeChangeToTheSpeedEveryonePeerWantsIsAllowed) {
  add(registry, CAN_NATIVE, &battery_receiver, "BMW PHEV", CAN_Speed::CAN_SPEED_100KBPS);
  add(registry, CAN_NATIVE, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_TRUE(
      can_speed_change_allowed(registry, CAN_NATIVE, CAN_Speed::CAN_SPEED_500KBPS, &battery_receiver, "BMW PHEV"));
  EXPECT_FALSE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

// 7. One bad interface does not take the others with it.
TEST_F(CanSpeedConflictTest, AConflictingInterfaceDoesNotStopACleanOne) {
  add(registry, CAN_NATIVE, &charger_receiver, "Chevy Volt Charger", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CAN_NATIVE, &battery_receiver, "Akasol", CAN_Speed::CAN_SPEED_250KBPS);
  add(registry, CAN_ADDON_MCP2515, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_FALSE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  EXPECT_TRUE(resolve_interface_speed(registry, CAN_ADDON_MCP2515, resolved));
  EXPECT_EQ(resolved, CAN_Speed::CAN_SPEED_500KBPS);
}

// An interface nobody registered for is not configured, which is not a fault.
TEST_F(CanSpeedConflictTest, AnEmptyInterfaceIsRefusedWithoutAnEvent) {
  EXPECT_FALSE(resolve_interface_speed(registry, CAN_NATIVE, resolved));
  EXPECT_FALSE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

/* The MCP2518FD block serves both CANFD_NATIVE and CANFD_ADDON_MCP2518. They are
   two keys for one controller and one wire, so they have to agree with each
   other too - "first key wins" is the same defect one level up. */
TEST_F(CanSpeedConflictTest, TwoKeysForOneFdControllerMustAgreeWithEachOther) {
  add(registry, CANFD_NATIVE, &battery_receiver, "MEB", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CANFD_ADDON_MCP2518, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_250KBPS);

  EXPECT_FALSE(resolve_shared_interface_speed(registry, CANFD_NATIVE, CANFD_ADDON_MCP2518, resolved));
  ASSERT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));

  const std::string message = message_for(EVENT_CAN_SPEED_CONFLICT);
  EXPECT_NE(message.find("MEB"), std::string::npos) << message;
  EXPECT_NE(message.find("BYD Can"), std::string::npos) << message;
}

TEST_F(CanSpeedConflictTest, OneFdKeyAloneResolvesToItsOwnSpeed) {
  add(registry, CANFD_ADDON_MCP2518, &battery_receiver, "MEB", CAN_Speed::CAN_SPEED_250KBPS);

  EXPECT_TRUE(resolve_shared_interface_speed(registry, CANFD_NATIVE, CANFD_ADDON_MCP2518, resolved));
  EXPECT_EQ(resolved, CAN_Speed::CAN_SPEED_250KBPS);
  EXPECT_FALSE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

// A key that is incoherent on its own is not rescued by the other key agreeing
// with what its first registration happened to say.
TEST_F(CanSpeedConflictTest, AnIncoherentFdKeyIsRefusedEvenWhenTheOtherKeyMatchesItsFirst) {
  add(registry, CANFD_NATIVE, &battery_receiver, "MEB", CAN_Speed::CAN_SPEED_500KBPS);
  add(registry, CANFD_NATIVE, &charger_receiver, "Chevy Volt Charger", CAN_Speed::CAN_SPEED_250KBPS);
  add(registry, CANFD_ADDON_MCP2518, &inverter_receiver, "BYD Can", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_FALSE(resolve_shared_interface_speed(registry, CANFD_NATIVE, CANFD_ADDON_MCP2518, resolved));
  EXPECT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));
}

/* The half that is not in this binary.
 *
 * comm_can.cpp reaches for the ESP32 CAN drivers and is not linked into these
 * tests, so nothing above can see whether init_CAN() actually ASKS. Reading it
 * is the only oracle available, and without it the policy could be perfect and
 * unused - which is precisely the state this change found the firmware in.
 */
namespace {

std::string strip_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size();) {
    if (src.compare(i, 2, "//") == 0) {
      while (i < src.size() && src[i] != '\n') {
        ++i;
      }
    } else if (src.compare(i, 2, "/*") == 0) {
      const size_t end = src.find("*/", i + 2);
      const size_t stop = end == std::string::npos ? src.size() : end + 2;
      for (; i < stop; ++i) {
        if (src[i] == '\n') {
          out += '\n';
        }
      }
    } else {
      out += src[i++];
    }
  }
  return out;
}

// Comments are stripped first: they name the very calls asserted on below, so a
// commented-out call would otherwise still satisfy the search.
std::string read_source(const std::string& relative_to_test_dir) {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  const std::string path = dir + "/" + relative_to_test_dir;
  std::ifstream src(path);
  EXPECT_TRUE(src.is_open()) << "this test reads " << path;
  return strip_comments(std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>()));
}

std::string function_body(const std::string& src, const std::string& signature) {
  const size_t at = src.find(signature);
  EXPECT_NE(at, std::string::npos) << "no `" << signature << "` in the source this test reads";
  if (at == std::string::npos) {
    return "";
  }
  const size_t open = src.find('{', at);
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

size_t count_of(const std::string& haystack, const std::string& needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

}  // namespace

TEST(CanSpeedConflictSourceTest, InitCanReadsNoSpeedOfItsOwn) {
  const std::string body =
      function_body(read_source("../Software/src/communication/can/comm_can.cpp"), "bool init_CAN()");

  EXPECT_EQ(body.find("->second.speed"), std::string::npos)
      << "init_CAN() must take every bitrate from the resolver, not from the first registration it finds";
  EXPECT_EQ(count_of(body, "resolve_interface_speed(") + count_of(body, "resolve_shared_interface_speed("), 4u)
      << "one resolution per controller: native, MCP2515, the FD pair, the second FD chip";
}

/* The runtime half has to be wired too: a speed change that never asks is the
   same defect a boot that never asks was. */
TEST(CanSpeedConflictSourceTest, ChangeCanSpeedAsksBeforeItSwitchesTheBus) {
  const std::string body = function_body(read_source("../Software/src/communication/can/comm_can.cpp"),
                                         "bool change_can_speed(CAN_Interface interface");

  const size_t guard = body.find("if (!can_speed_change_allowed(");
  ASSERT_NE(guard, std::string::npos) << "change_can_speed() must consult the policy";
  EXPECT_LT(guard, body.find("init_native_can(")) << "and before it touches the controller";
  EXPECT_LT(guard, body.find("changeSpeed(")) << "and before it touches the controller";
}

/* Each interface is brought up INSIDE its own resolver guard. A resolver that
   raises the event and then lets the interface start anyway would be worse than
   no check at all: the bus would still be wrong, now with a message saying it
   was handled. */
TEST(CanSpeedConflictSourceTest, EveryInterfaceStartsInsideItsResolverGuard) {
  const std::string body =
      function_body(read_source("../Software/src/communication/can/comm_can.cpp"), "bool init_CAN()");

  EXPECT_NE(body.find("if (resolve_interface_speed(can_receivers, CAN_NATIVE, native_speed))"), std::string::npos);
  EXPECT_NE(body.find("if (resolve_interface_speed(can_receivers, CAN_ADDON_MCP2515, addon_speed))"),
            std::string::npos);
  EXPECT_NE(body.find("if (fd_ok)"), std::string::npos);
  EXPECT_NE(body.find("if (fd2_ok)"), std::string::npos);
}

/* Why the bus-off recovery must not be routed through this policy at all.
 *
 * It names no requester, so every registration is a peer; and it asks for
 * whatever speed the controller is CURRENTLY running, which after a runtime
 * switch is not the speed anybody registered. The refusal below is correct for
 * a stranger asking to CHANGE the bus - it is the wrong question to ask about a
 * re-init that moves nothing.
 */
TEST_F(CanSpeedConflictTest, AStrangerIsRefusedAtASpeedTheSoleRegistrantDidNotAskFor) {
  add(registry, CAN_NATIVE, &battery_receiver, "BMW PHEV", CAN_Speed::CAN_SPEED_500KBPS);

  EXPECT_FALSE(
      can_speed_change_allowed(registry, CAN_NATIVE, CAN_Speed::CAN_SPEED_100KBPS, nullptr, "bus-off recovery"));
  ASSERT_TRUE(event_active(EVENT_CAN_SPEED_CONFLICT));
  EXPECT_NE(message_for(EVENT_CAN_SPEED_CONFLICT).find("bus-off recovery"), std::string::npos)
      << message_for(EVENT_CAN_SPEED_CONFLICT);
}

/* Bus-off recovery re-applies the speed already in force, so it is not a speed
   change and must not be ruled on as one. Routed through change_can_speed() it
   is refused exactly when a runtime switch has moved the bus off its registered
   speed - the state BMW PHEV's wake sequence creates for ~50 ms, on a sleeping
   bus that ACKs nothing, which is where bus-off is the ordinary outcome rather
   than the exceptional one. The refusal raises an ERROR event, and an ERROR
   event is FAULT, so the recovery would not happen and the board would stop. */
TEST(CanSpeedConflictSourceTest, BusOffRecoveryReInitsInsteadOfAskingThePolicy) {
  const std::string body =
      function_body(read_source("../Software/src/communication/can/comm_can.cpp"), "receive_frame_can_native() {");

  ASSERT_NE(body.find("TWAI_BUS_OFF_ST"), std::string::npos) << "the bus-off branch is what this test reads";
  EXPECT_EQ(body.find("change_can_speed("), std::string::npos)
      << "the recovery must not consult the change policy: it asks for the speed already in force, "
         "which after a runtime switch no registration holds";
  EXPECT_NE(body.find("init_native_can(native_can_speed"), std::string::npos)
      << "it re-inits the controller at the speed already in force";
}
