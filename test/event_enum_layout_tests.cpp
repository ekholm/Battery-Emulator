#include <gtest/gtest.h>

#include <string>

#include "../Software/src/devboard/utils/events.h"

/* Inserting an event into the middle of the XX() list shifts every ordinal after it, and one
 * region of that list is ORDER-SENSITIVE in a way nothing was checking.
 *
 * resolve_battery_event() and event_battery_number() (events.cpp) do index arithmetic over the
 * enum: a per-pack event is set as set_event(EVENT_BATTERY_X, data, pack) and resolved to the
 * concrete event by `base + (pack - 1)`, valid only while the block from EVENT_BATTERY_EMPTY to
 * EVENT_BATTERY3_TEMP_DEVIATION_HIGH is exactly consecutive triples and every base sits at a
 * multiple of three from the first one. Get that wrong and battery 2's event lands on an
 * unrelated entry, at runtime, silently.
 *
 * EVENT_CAN_NATIVE_NOT_INITIALIZED was added at the CAN group, well above the block, so the
 * whole block shifted uniformly and the arithmetic still holds. That is the right place, and
 * nothing in the tree would have said so if it had not been.
 */
namespace {

// The three names of one triple differ only by the pack infix: EVENT_BATTERY_X,
// EVENT_BATTERY2_X, EVENT_BATTERY3_X. get_event_enum_string() drops the leading "EVENT_".
std::string suffix_after_pack_infix(const std::string& name, int pack) {
  const std::string prefix = (pack == 1) ? "BATTERY_" : "BATTERY" + std::to_string(pack) + "_";
  if (name.rfind(prefix, 0) != 0) {
    return "";
  }
  return name.substr(prefix.size());
}

}  // namespace

// Every triple must be (battery 1, battery 2, battery 3) of the SAME event, in that order.
// This is what makes `base + (pack - 1)` mean what its callers think.
//
// events.cpp already static_asserts the block's LENGTH and three sampled triplets, and those
// are the stronger check where they reach - they fail the firmware build, not just this one.
// What they cannot reach is stated in their own comment: "Checking the two ends plus one
// interior triplet is enough, because the block is generated as whole triplets." That is an
// assumption about how the list is maintained, and two edits satisfy every assert while
// breaking the block - swapping the pack 2 and pack 3 entries of any unsampled triple, and
// inserting three non-per-pack events mid-block, where the length stays a multiple of three
// and every shifted base keeps its alignment. Both compile clean and are caught here.
TEST(EventEnumLayout, EveryPerPackTripleIsTheSameEventForPacksOneTwoThree) {
  for (int e = static_cast<int>(EVENT_BATTERY_EMPTY); e <= static_cast<int>(EVENT_BATTERY3_TEMP_DEVIATION_HIGH);
       e += 3) {
    const std::string first = get_event_enum_string(static_cast<EVENTS_ENUM_TYPE>(e));
    const std::string second = get_event_enum_string(static_cast<EVENTS_ENUM_TYPE>(e + 1));
    const std::string third = get_event_enum_string(static_cast<EVENTS_ENUM_TYPE>(e + 2));

    const std::string s1 = suffix_after_pack_infix(first, 1);
    const std::string s2 = suffix_after_pack_infix(second, 2);
    const std::string s3 = suffix_after_pack_infix(third, 3);

    ASSERT_FALSE(s1.empty()) << first
                             << " sits at a triple boundary but is not an EVENT_BATTERY_* base - an event "
                                "inserted inside the per-pack block misaligns every triple after it";
    EXPECT_EQ(s1, s2) << first << " is followed by " << second << ", not by its battery 2 variant";
    EXPECT_EQ(s1, s3) << first << " is followed by " << third << ", not by its battery 3 variant";
  }
}

// The block must also be entered at a base, not one past it: EVENT_BATTERY_EMPTY itself is the
// origin all the arithmetic is relative to.
TEST(EventEnumLayout, TheBlockStartsAtABatteryOneBase) {
  EXPECT_EQ(suffix_after_pack_infix(get_event_enum_string(EVENT_BATTERY_EMPTY), 1), "EMPTY");
  EXPECT_EQ(suffix_after_pack_infix(get_event_enum_string(EVENT_BATTERY3_TEMP_DEVIATION_HIGH), 3),
            "TEMP_DEVIATION_HIGH");
}
