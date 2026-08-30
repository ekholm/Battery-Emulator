#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/utils/events.h"
#include "alloc_probe.h"

/* get_event_message() writes the message as bytes into a caller-owned buffer, on the snprintf
 * contract, because most callers want bytes: the MQTT publisher, the ESP-NOW frame and
 * set_event()'s own debug line all took `.c_str()` off the String form on the next token.
 *
 * The property that matters is that no message a user reads changed. The whole table was
 * diffed against the previous implementation when this landed - all 170 messages byte for
 * byte, including the two composed GPIO ones and every pack-suffixed variant - and what is
 * pinned here is the part of that a future edit could break silently: the composition, the
 * suffix, the truncation contract, and the exact text of the structurally interesting cases.
 */
namespace {

class EventMessageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    init_hal();
    init_events();
    reset_all_events();
    // Pack 1 only names itself when there is another pack to tell it from, so the cases below
    // run as a three-pack install unless they say otherwise.
    saved_pack_count = datalayer.system.info.configured_batteries;
    datalayer.system.info.configured_batteries = 3;
  }

  void TearDown() override { datalayer.system.info.configured_batteries = saved_pack_count; }

  uint8_t saved_pack_count = 1;

  // Drives the HAL into the state the two GPIO messages report on, so their text is
  // deterministic rather than whatever a previous test left behind.
  static void provoke_pin_conflict() {
    esp32hal->alloc_pins("Equipment stop button", GPIO_NUM_32);
    esp32hal->alloc_pins("Precharge control", GPIO_NUM_32);
  }
};

std::string message_of(EVENTS_ENUM_TYPE event) {
  char buf[EVENT_MESSAGE_BUF_SIZE];
  const size_t len = get_event_message(event, buf, sizeof(buf));
  EXPECT_LT(len, sizeof(buf)) << "message " << get_event_enum_string(event) << " no longer fits "
                              << "EVENT_MESSAGE_BUF_SIZE - the constant is measured, so re-measure it";
  return std::string(buf);
}

}  // namespace

/* Every event, not a sample: a message that comes out empty means an id fell through the table,
 * which is invisible until someone hits that fault in the field.
 */
TEST_F(EventMessageTest, EveryEventHasAMessageAndReportsItsOwnLength) {
  provoke_pin_conflict();
  for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
    const EVENTS_ENUM_TYPE event = (EVENTS_ENUM_TYPE)i;
    char buf[EVENT_MESSAGE_BUF_SIZE];
    const size_t len = get_event_message(event, buf, sizeof(buf));
    EXPECT_GT(len, 0u) << get_event_enum_string(event) << " has no message";
    EXPECT_LT(len, sizeof(buf)) << get_event_enum_string(event) << " does not fit the documented buffer";
    EXPECT_EQ(len, strlen(buf)) << get_event_enum_string(event)
                                << ": the return must be the length actually written when it fits";
  }
}

/* The pack suffix is the one piece of composition every battery event depends on, and the
 * three variants resolve by arithmetic on the enum - so this walks the whole enum rather than
 * trusting the three it is easy to think of.
 */
TEST_F(EventMessageTest, EveryPackSpecificEventNamesItsPack) {
  for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
    const EVENTS_ENUM_TYPE event = (EVENTS_ENUM_TYPE)i;
    const std::string name = get_event_enum_string(event);
    const std::string message = message_of(event);
    if (name.rfind("BATTERY2_", 0) == 0) {
      EXPECT_NE(message.find(" (Battery 2)"), std::string::npos) << name << ": " << message;
    } else if (name.rfind("BATTERY3_", 0) == 0) {
      EXPECT_NE(message.find(" (Battery 3)"), std::string::npos) << name << ": " << message;
    }
  }
}

/* Exact text, captured from the implementation this replaced. A plain message, all three
 * variants of one pack-specific event, and both composed GPIO messages - the cases where the
 * composition does something rather than copying a literal. If one of these fails, either the
 * composition broke or somebody edited user-visible text, and both want saying out loud.
 */
TEST_F(EventMessageTest, TheComposedMessagesReadExactlyAsTheyDidBefore) {
  EXPECT_EQ(message_of(EVENT_CAN_INVERTER_MISSING),
            "Inverter not sending messages via CAN for the last 60 seconds. Check wiring!");
  EXPECT_EQ(message_of(EVENT_BATTERY_OVERHEAT),
            "Battery overheated. Shutting down to prevent thermal runaway! (Battery 1)");
  EXPECT_EQ(message_of(EVENT_BATTERY2_OVERHEAT),
            "Battery overheated. Shutting down to prevent thermal runaway! (Battery 2)");
  EXPECT_EQ(message_of(EVENT_BATTERY3_OVERHEAT),
            "Battery overheated. Shutting down to prevent thermal runaway! (Battery 3)");

  // A single-pack install drops the pack 1 suffix, and the byte form must follow the same rule
  // as the String form or the two builders disagree on the commonest install there is.
  datalayer.system.info.configured_batteries = 1;
  EXPECT_EQ(message_of(EVENT_BATTERY_OVERHEAT), "Battery overheated. Shutting down to prevent thermal runaway!");
  EXPECT_EQ(std::string(get_event_message_string(EVENT_BATTERY_OVERHEAT).c_str()), message_of(EVENT_BATTERY_OVERHEAT));
  // Only pack 1's suffix is conditional. A pack 2 or 3 event names its pack whatever the count
  // says, so a stale one on an install that dropped to one pack still says which pack it was.
  EXPECT_EQ(message_of(EVENT_BATTERY2_OVERHEAT),
            "Battery overheated. Shutting down to prevent thermal runaway! (Battery 2)");
  datalayer.system.info.configured_batteries = 3;

  provoke_pin_conflict();
  EXPECT_EQ(message_of(EVENT_GPIO_CONFLICT),
            "GPIO Pin Conflict: The pin used by 'Precharge control' is already allocated by 'Equipment stop "
            "button'. Please check your configuration and assign different pins.");
  EXPECT_EQ(message_of(EVENT_GPIO_NOT_DEFINED),
            "Missing GPIO Assignment: The component 'Precharge control' requires a GPIO pin that isn't "
            "configured. Please define a valid pin number in your settings.");
}

// The two GPIO messages read HAL state at call time; caching either would report the wrong pin.
TEST_F(EventMessageTest, TheGpioMessagesFollowTheHalRatherThanCachingIt) {
  esp32hal->alloc_pins("SMA inverter", GPIO_NUM_32);
  esp32hal->alloc_pins("CAN", GPIO_NUM_32);
  EXPECT_NE(message_of(EVENT_GPIO_CONFLICT).find("'CAN' is already allocated by 'SMA inverter'"), std::string::npos);

  esp32hal->alloc_pins("Precharge control", GPIO_NUM_32);
  EXPECT_NE(message_of(EVENT_GPIO_CONFLICT).find("'Precharge control' is already allocated by"), std::string::npos)
      << "the message must reflect the allocation that just failed, not the first one";
}

/* The snprintf contract, which is the whole reason a caller can trust a fixed buffer: always
 * NUL-terminated, and the return is what the message WOULD have taken so truncation is
 * detectable. A form that returned the bytes written instead would be indistinguishable from
 * success at every call site.
 */
TEST_F(EventMessageTest, TruncationIsReportedRatherThanHidden) {
  const std::string full = message_of(EVENT_BATTERY_OVERHEAT);
  ASSERT_GT(full.size(), 20u);

  char small[8];
  EXPECT_EQ(get_event_message(EVENT_BATTERY_OVERHEAT, small, sizeof(small)), full.size())
      << "the return is the full length, snprintf-style";
  EXPECT_EQ(std::string(small), full.substr(0, sizeof(small) - 1)) << "and what landed is a NUL-terminated prefix";

  // Exactly one byte short: the boundary an off-by-one lands on.
  std::vector<char> tight(full.size());
  EXPECT_EQ(get_event_message(EVENT_BATTERY_OVERHEAT, tight.data(), tight.size()), full.size());
  EXPECT_EQ(std::string(tight.data()), full.substr(0, full.size() - 1));

  // Exactly enough: the first size that must NOT truncate.
  std::vector<char> exact(full.size() + 1);
  EXPECT_EQ(get_event_message(EVENT_BATTERY_OVERHEAT, exact.data(), exact.size()), full.size());
  EXPECT_EQ(std::string(exact.data()), full);
}

/* Truncation has to report the full length for a pack-suffixed message too. The suffix is
 * appended after the body, so a buffer that ends inside the body must still account for it -
 * otherwise a caller sizing from the return would come back and still be short.
 */
TEST_F(EventMessageTest, ATruncatedPackSuffixIsStillCounted) {
  const std::string full = message_of(EVENT_BATTERY3_OVERHEAT);
  for (size_t len = 1; len <= full.size() + 1; len++) {
    std::vector<char> buf(len);
    EXPECT_EQ(get_event_message(EVENT_BATTERY3_OVERHEAT, buf.data(), buf.size()), full.size())
        << "with a buffer of " << len;
    EXPECT_EQ(std::string(buf.data()), full.substr(0, len - 1)) << "with a buffer of " << len;
  }
}

/* Split from the null case below, and the split is the point rather than tidiness.
 *
 * Both halves are one guard, but only this half can be ASSERTED: with a real buffer and a
 * zero length, dropping the guard makes snprintf write nothing and return the length the
 * message would have taken, so the function reports a length for a copy the caller never got.
 * That is a clean, named failure. The null half can only ever crash the process, which is a
 * mutation caught by the suite dying rather than by a test saying what broke - so the guard
 * needs a case that fails out loud, and this is it. It must stay ahead of the null case,
 * because a segfault there ends the process before anything after it runs.
 */
TEST_F(EventMessageTest, AZeroLengthDestinationIsRefusedAndReportsNoLength) {
  char buf[8];
  memset(buf, 'x', sizeof(buf));

  EXPECT_EQ(get_event_message(EVENT_BATTERY_OVERHEAT, buf, 0), 0u)
      << "a zero-length buffer must report 0, not the length the message would have taken - "
         "the return is what a caller sizes its next buffer from";
  for (size_t i = 0; i < sizeof(buf); i++) {
    EXPECT_EQ(buf[i], 'x') << "nothing may be written when there is no room, not even a NUL";
  }
}

TEST_F(EventMessageTest, ANullDestinationIsRefused) {
  EXPECT_EQ(get_event_message(EVENT_BATTERY_OVERHEAT, nullptr, EVENT_MESSAGE_BUF_SIZE), 0u);
}

/* The point of the item, asserted rather than estimated: the byte form does not allocate, and
 * the String form does. If the byte form ever starts allocating, the three callers moved onto
 * it are paying for nothing.
 */
TEST_F(EventMessageTest, TheByteFormAllocatesNothingAndTheStringFormAllocates) {
#ifdef __SANITIZE_ADDRESS__
  GTEST_SKIP() << "the probe replaces global operator new, which ASAN owns";
#endif
  provoke_pin_conflict();
  char buf[EVENT_MESSAGE_BUF_SIZE];

  alloc_probe_reset();
  for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
    get_event_message((EVENTS_ENUM_TYPE)i, buf, sizeof(buf));
  }
  EXPECT_EQ(alloc_probe_count(), 0u) << "the byte form must not allocate for any event, including the two "
                                        "composed GPIO messages";

  alloc_probe_reset();
  for (int i = 0; i < EVENT_NOF_EVENTS; i++) {
    String s = get_event_message_string((EVENTS_ENUM_TYPE)i);
    (void)s.length();
  }
  EXPECT_GT(alloc_probe_count(), 0u) << "the String form is expected to allocate - if it stops, this test is "
                                        "measuring nothing and the callers above gained nothing";
}
