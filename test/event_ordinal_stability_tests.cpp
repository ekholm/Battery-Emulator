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
 * THE ANCHORS ARE RELATIVE, NOT ABSOLUTE, and that is a correction. The first version of
 * this test pinned six absolute ordinals "spread across the enum on purpose" - including
 * EVENT_INVERTER_REBOOT_DECLINED at 167, "the last event that predates this lane". That
 * anchor reds on every LIFT rather than on any mistake of ours: upstream inserts events
 * mid-enum itself (between 2026-08-26 and 2026-08-31 it added EVENT_BYD_CHARGE_TERMINATED
 * at index 91 and EVENT_OTA_ROLLBACK at 118, moving that anchor to 169), so rebasing this
 * branch onto a newer upstream failed a test that was reporting upstream's renumbering,
 * not ours. A guard that cries on every rebase is a guard that gets deleted.
 *
 * So this pins the property this branch actually controls: the event it ADDS is the last
 * one before the sentinel. That is exactly "appended, not inserted", it is independent of
 * how upstream numbers its own events, and it survives any lift.
 */
namespace {

struct Anchor {
  EVENTS_ENUM_TYPE event;
  int ordinal;
  const char* why;
};

}  // namespace

TEST(EventOrdinalStability, TheEventsThisLaneAddsAreAppendedNotInserted) {
  /* The whole property, in one line each: the events this lane adds form a contiguous block
   * at the END, so nothing that predates them moved. Stated against EVENT_NOF_EVENTS and
   * against each other rather than against absolute numbers, because the numbers are
   * upstream's to change - it inserts mid-enum itself - and the POSITION is ours to keep. */
  const EVENTS_ENUM_TYPE added[] = {
      EVENT_CAN_NATIVE_INIT_FAILURE,
      EVENT_CAN_NATIVE_NOT_INITIALIZED,
  };
  const int count = static_cast<int>(sizeof(added) / sizeof(added[0]));

  for (int i = 1; i < count; i++) {
    EXPECT_EQ(static_cast<int>(added[i]), static_cast<int>(added[i - 1]) + 1)
        << get_event_enum_string(added[i]) << " is not adjacent to " << get_event_enum_string(added[i - 1])
        << ", so this lane's events are no longer one block at the end.";
  }

  EXPECT_EQ(static_cast<int>(added[count - 1]) + 1, static_cast<int>(EVENT_NOF_EVENTS))
      << get_event_enum_string(added[count - 1]) << " is not the last event before the\n"
      << "  sentinel, so an event was INSERTED rather than appended and every event after it has\n"
      << "  been renumbered. Event ordinals are published over ESP-NOW (ESPNOW_KEY_EVENT_ID), so a\n"
      << "  receiver on an older build now decodes the wrong event - silently, and precisely in\n"
      << "  the data a user reads to diagnose a fault.\n"
      << "  Add new events at the END of EVENTS_ENUM_TYPE, immediately before EVENT_NOF_EVENTS.";
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
