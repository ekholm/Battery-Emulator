#include <gtest/gtest.h>

#include "../Software/src/devboard/utils/events.h"

/* Event ordinals are ON THE WIRE, so inserting an event mid-enum is a protocol break.
 *
 * `espnow.cpp` publishes the raw enum value - `put_u16_field(ESPNOW_KEY_EVENT_ID,
 * static_cast<uint16_t>(handle))` - and `espnow.h` documents that field as
 * "UINT16 EVENTS_ENUM_TYPE ordinal". A receiver decodes it against ITS OWN copy of this
 * enum, so a new event added anywhere but the end renumbers every event after it and two
 * nodes at different versions disagree about what happened. The failure is silent and it
 * mislabels exactly the thing a user reads to diagnose a fault.
 *
 * This was not caught for a whole lane of branches because nothing pinned it: four
 * separate fixes each inserted next to the events they were about, which is the natural
 * place to put them and the wrong one. NEW EVENTS GO AT THE END, immediately before the
 * EVENT_NOF_EVENTS sentinel - which is a count, not a wire value, and may move.
 *
 * The anchors below are spread across the enum on purpose: an insertion anywhere before
 * the last one moves at least one of them. They are not a complete list and are not
 * meant to be - a complete list would have to be regenerated on every legitimate append,
 * which is the maintenance cost that makes such tests get deleted.
 */
namespace {

struct Anchor {
  EVENTS_ENUM_TYPE event;
  int ordinal;
  const char* why;
};

}  // namespace

TEST(EventOrdinalStability, NoEventIsInsertedAheadOfAnExistingOne) {
  const Anchor anchors[] = {
      {EVENT_CANMCP2518FD_INIT_FAILURE, 0, "the first entry - moves only if something is inserted at the very top"},
      {EVENT_CAN_BATTERY3_DETECTED, 8, "just past the init-failure block, where three of these fixes wanted to insert"},
      {EVENT_CAN_INVERTER_MISSING, 15, "the bus-error neighbourhood, where the transmit guard wanted to insert"},
      {EVENT_BALANCING_END, 32, "just past the detection block, where the replay fix wanted to insert"},
      {EVENT_BATTERY_OVERVOLTAGE, 60, "mid-enum: catches an insertion anywhere in the first third"},
      {EVENT_INVERTER_REBOOT_DECLINED, 167, "the last event that predates this lane"},
  };

  for (const Anchor& a : anchors) {
    EXPECT_EQ(static_cast<int>(a.event), a.ordinal)
        << get_event_enum_string(a.event) << " has moved from ordinal " << a.ordinal << " to "
        << static_cast<int>(a.event) << ".\n"
        << "  Why this matters: " << a.why << ".\n"
        << "  Event ordinals are published over ESP-NOW (ESPNOW_KEY_EVENT_ID), so renumbering one\n"
        << "  makes every node on an older build report the wrong event. Add new events at the END\n"
        << "  of EVENTS_ENUM_TYPE, immediately before EVENT_NOF_EVENTS.";
  }
}

/* The sentinel is a count and is allowed to move - but it has to stay LAST, or the
 * ordinals of anything after it would be counted as events and the array sized off it
 * would be short. */
TEST(EventOrdinalStability, TheCountSentinelIsStillTheLastEntry) {
  // Every real event must be below the count, and the count is what sizes the events array.
  for (int i = 0; i < static_cast<int>(EVENT_NOF_EVENTS); i++) {
    EXPECT_NE(get_event_enum_string(static_cast<EVENTS_ENUM_TYPE>(i)), nullptr)
        << "ordinal " << i << " is below EVENT_NOF_EVENTS but names no event";
  }
}
