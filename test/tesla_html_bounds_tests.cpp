#include <gtest/gtest.h>

#include <functional>

#include "../Software/src/battery/TESLA-BATTERY.h"
#include "../Software/src/battery/TESLA-HTML.h"
#include "../Software/src/datalayer/datalayer_extended.h"

#include "Arduino.h"

namespace {

// The Tesla renderer indexes fourteen static tables with fields the CAN parser fills straight
// from bit slices of a frame. Every field below is wider than the table it selects from, so a
// pack that reports a code the table has no entry for reads past the end: on the host that is a
// wild pointer handed to String(), on the ESP32 the same. These tests pin the bound at the only
// place it is observable - the rendered text.
//
// The datalayer is a global shared by every test, so each case starts from the power-on state.
String render_with_tesla_state(const std::function<void(DATALAYER_INFO_TESLA&)>& set_fields) {
  datalayer_extended.tesla = DATALAYER_INFO_TESLA{};
  set_fields(datalayer_extended.tesla);
  TeslaHtmlRenderer renderer;
  return renderer.get_status_html();
}

bool contains(const String& haystack, const char* needle) {
  return haystack.str().find(needle) != std::string::npos;
}

// A freshly set-up battery has published nothing yet: every field is zero and battery_manufactureDate
// is still the null pointer it powers on as. Rendering that must produce a page, not a crash.
TEST(TeslaHtmlLookupBounds, AFreshlySetUpTeslaRenders) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {});

  EXPECT_TRUE(contains(content, "<h4>BMS State: STANDBY</h4>"));
}

// contactorText[13], selected by a 4-bit field (0x20A byte 1, low nibble).
TEST(TeslaHtmlLookupBounds, AnOutOfRangeContactorSetStateIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.packContactorSetState = 15; });

  EXPECT_TRUE(contains(content, "<h4>HVP Contactor State: UNKNOWN(15)</h4>"));
}

// BMS_contactorState[7], selected by a 3-bit field (0x212 byte 1).
TEST(TeslaHtmlLookupBounds, AnOutOfRangeBmsContactorStateIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.BMS_contactorState = 7; });

  EXPECT_TRUE(contains(content, "<h4>BMS Contactor State: UNKNOWN(7)</h4>"));
}

// BMS_state[10], selected by a 4-bit signal. The DBC even documents a code 10 ("BMS_DIAG") the
// table has no entry for.
TEST(TeslaHtmlLookupBounds, AnOutOfRangeBmsStateIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.BMS_state = 10; });

  EXPECT_TRUE(contains(content, "<h4>BMS State: UNKNOWN(10)</h4>"));
}

// BMS_hvState[7], selected by a 3-bit field (0x212 byte 2).
TEST(TeslaHtmlLookupBounds, AnOutOfRangeBmsHvStateIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.BMS_hvState = 7; });

  EXPECT_TRUE(contains(content, "<h4>BMS HV State: UNKNOWN(7)</h4>"));
}

// BMS_uiChargeStatus[6], selected by a 3-bit signal.
TEST(TeslaHtmlLookupBounds, AnOutOfRangeUiChargeStatusIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.BMS_uiChargeStatus = 7; });

  EXPECT_TRUE(contains(content, "<h4>BMS UI Charge Status: UNKNOWN(7)</h4>"));
}

// BMS_powerLimitState[2], selected by a 1-bit field - exactly covered, so the bound must not fire.
TEST(TeslaHtmlLookupBounds, AnInRangePowerLimitStateStillRendersItsLabel) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.BMS_powerLimitState = 1; });

  EXPECT_TRUE(contains(content, "<h4>Power Limit State: CALCULATED_FOR_DRIVE</h4>"));
}

// HVP_contactor[3], selected by a 2-bit field (0x20A byte 3, top two bits).
TEST(TeslaHtmlLookupBounds, AnOutOfRangeContactorRequestStatusIsNamedNotDereferenced) {
  String content =
      render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.battery_packCtrsRequestStatus = 3; });

  EXPECT_TRUE(contains(content, "<h4>Contactors Request Status: UNKNOWN(3)</h4>"));
}

// PCS_dcdcStatus[3], selected by three separate 2-bit fields (0x224 byte 0).
TEST(TeslaHtmlLookupBounds, AnOutOfRangeDcdcStatusIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {
    tesla.PCS_dcdcPrechargeStatus = 3;
    tesla.PCS_dcdc12VSupportStatus = 3;
    tesla.PCS_dcdcHvBusDischargeStatus = 3;
  });

  EXPECT_TRUE(contains(content, "<h4>Precharge Status: UNKNOWN(3)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>12V Support Status: UNKNOWN(3)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>HV Bus Discharge Status: UNKNOWN(3)</h4>"));
}

// PCS_dcdcMainState[7], selected by a 4-bit field assembled from two bytes of 0x224.
TEST(TeslaHtmlLookupBounds, AnOutOfRangeDcdcMainStateIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) { tesla.PCS_dcdcMainState = 15; });

  EXPECT_TRUE(contains(content, "<h4>Main State: UNKNOWN(15)</h4>"));
}

// PCS_dcdcSubState[18], selected by two separate 5-bit fields (0x224 bytes 1 and 7).
TEST(TeslaHtmlLookupBounds, AnOutOfRangeDcdcSubStateIsNamedNotDereferenced) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {
    tesla.PCS_dcdcSubState = 31;
    tesla.PCS_dcdcInitialPrechargeSubState = 31;
  });

  EXPECT_TRUE(contains(content, "<h4>Sub State: UNKNOWN(31)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Initial Precharge Substate: UNKNOWN(31)</h4>"));
}

/* The four PCS retry COUNTERS render as numbers, not as a two-entry table.
 *
 * They are counts the parser lifts straight out of 0x224 - 3-bit and 4-bit fields - and they
 * used to index falseTrue[], so ONE retry read "True" and two read past the end of a two-entry
 * table. Bounding that made it safe (UNKNOWN(2) and up) without making it right: a count is not
 * a boolean, and the labels already say "Rty Cnt".
 *
 * Rendering the number changes what 0 and 1 display too - from "False"/"True" to "0"/"1" - which
 * is the deliberate part. The page now says how many retries there were, which is the only
 * reading of these fields that was ever true.
 */
TEST(TeslaHtmlLookupBounds, TheRetryCountersRenderTheCountRatherThanABoolean) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {
    tesla.PCS_dcdcPrechargeRtyCnt = 7;
    tesla.PCS_dcdc12VSupportRtyCnt = 15;
    tesla.PCS_dcdcDischargeRtyCnt = 15;
    tesla.PCS_dcdcPrechargeRestartCnt = 7;
  });

  EXPECT_TRUE(contains(content, "<h4>Precharge Rty Cnt: 7</h4>"));
  EXPECT_TRUE(contains(content, "<h4>12V Support Rty Cnt: 15</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Discharge Rty Cnt: 15</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Precharge Restart Cnt: 7</h4>"));

  EXPECT_FALSE(contains(content, "Rty Cnt: True")) << "a count must never render as a boolean word";
  EXPECT_FALSE(contains(content, "Rty Cnt: UNKNOWN")) << "and it is a number, so no value is out of range";
}

/* The values the old two-entry table could represent are exactly where the change is visible to
 * a user, so pin them: 0 and 1 stop saying False/True.
 */
TEST(TeslaHtmlLookupBounds, TheCountsZeroAndOneRenderAsNumbersToo) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {
    tesla.PCS_dcdcPrechargeRtyCnt = 0;
    tesla.PCS_dcdc12VSupportRtyCnt = 1;
  });

  EXPECT_TRUE(contains(content, "<h4>Precharge Rty Cnt: 0</h4>"));
  EXPECT_TRUE(contains(content, "<h4>12V Support Rty Cnt: 1</h4>"));
  EXPECT_FALSE(contains(content, "Rty Cnt: False"));
}

/* hvilStatusState[16] and contactorState[12] are the two tables the wire cannot overrun (4-bit
 * into 16, 3-bit into 12). The bound must leave their real labels alone.
 *
 * Assert the last DISTINCTLY NAMED entry of each, not the last entry: both tables pad their tail
 * with literal "UNKNOWN(n)" strings, which are the same text the bound produces out of range. A
 * case asserting hvil_status = 15 renders "UNKNOWN(15)" therefore passes whether the bound fired
 * or the table answered, and cannot tell a correct bound from one that fires early. The boundary
 * itself is pinned directly below, where a purpose-built table has a distinctive last entry.
 */
TEST(TeslaHtmlLookupBounds, TheExactlyCoveredTablesStillRenderTheirNamedLabels) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {
    tesla.hvil_status = 9;
    tesla.packContNegativeState = 7;
    tesla.packContPositiveState = 7;
  });

  EXPECT_TRUE(contains(content, "<h4>HVIL Status: VEHICLE_OR_PENTHOUSE_LID_OPENFAULT</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Negative Contactor: WELDED</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Positive Contactor: WELDED</h4>"));
}

/* The bound is a property of lookupName(), and lookupName() is directly callable, so the boundary
 * can be pinned without going through a page of rendered HTML.
 *
 * It needs to be. Every other case here asserts on a substring of the whole page, and the widest
 * code the wire can carry is nowhere near the end of most tables - so a bound that fires ONE ENTRY
 * EARLY changes nothing any of them look at. Measured, not assumed: mutating the bound to
 * `index + 1 < N` fails exactly ONE test in the entire suite, and it is not one of the cases named
 * for covering this. A table with a distinctive last entry is what makes the off-by-one visible.
 */
TEST(TeslaHtmlLookupBounds, TheBoundAdmitsTheLastEntryAndNamesTheFirstPastIt) {
  static const char* const table[] = {"ZERO", "ONE", "LAST"};

  EXPECT_EQ(lookupName(table, 0), String("ZERO"));
  EXPECT_EQ(lookupName(table, 2), String("LAST")) << "the last entry is in range";
  EXPECT_EQ(lookupName(table, 3), String("UNKNOWN(3)")) << "one past the end is named, not read";
  EXPECT_EQ(lookupName(table, 255), String("UNKNOWN(255)")) << "the widest code a uint8_t can carry";

  static const char* const single[] = {"ONLY"};

  EXPECT_EQ(lookupName(single, 0), String("ONLY")) << "a one-entry table still has an entry 0";
  EXPECT_EQ(lookupName(single, 1), String("UNKNOWN(1)"));
}

// The wire can only reach the widths above, but nothing in the renderer depends on that: a
// corrupted or future-widened field must still name its value rather than dereference whatever
// sits past the table. 255 is the widest a uint8_t field can carry.
TEST(TeslaHtmlLookupBounds, EverySaturatedFieldNamesItsValue) {
  String content = render_with_tesla_state([](DATALAYER_INFO_TESLA& tesla) {
    tesla.hvil_status = 255;
    tesla.packContactorSetState = 255;
    tesla.BMS_contactorState = 255;
    tesla.packContNegativeState = 255;
    tesla.packContPositiveState = 255;
    tesla.battery_packCtrsRequestStatus = 255;
    tesla.BMS_state = 255;
    tesla.BMS_hvState = 255;
    tesla.BMS_uiChargeStatus = 255;
    tesla.BMS_powerLimitState = 255;
    tesla.PCS_dcdcPrechargeStatus = 255;
    tesla.PCS_dcdc12VSupportStatus = 255;
    tesla.PCS_dcdcHvBusDischargeStatus = 255;
    tesla.PCS_dcdcMainState = 255;
    tesla.PCS_dcdcSubState = 255;
    tesla.PCS_dcdcInitialPrechargeSubState = 255;
    tesla.PCS_dcdcPrechargeRtyCnt = 255;
    tesla.PCS_dcdc12VSupportRtyCnt = 255;
    tesla.PCS_dcdcDischargeRtyCnt = 255;
    tesla.PCS_dcdcPrechargeRestartCnt = 255;
  });

  EXPECT_TRUE(contains(content, "<h4>HVIL Status: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>HVP Contactor State: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>BMS Contactor State: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Negative Contactor: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Positive Contactor: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Contactors Request Status: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>BMS State: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>BMS HV State: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>BMS UI Charge Status: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Power Limit State: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Main State: UNKNOWN(255)</h4>"));
  EXPECT_TRUE(contains(content, "<h4>Sub State: UNKNOWN(255)</h4>"));
}

}  // namespace

/* The parser half: HVP_currentSenseMia masked TWO bits where its own DBC comment says one.
 *
 * Bit 58 is currentSenseMia; bit 59 is shuntRefVoltageMismatch, parsed one line below. With a
 * two-bit mask the page reported current-sense MIA whenever the ref-voltage bit was set on its
 * own - a wrong diagnosis rather than a crash, which is why nothing caught it. Every sibling MIA
 * field in the same block masks a single bit; this one was alone in being wrong.
 *
 * Driven through the real frame handler on 0x7AA mux 1, so it exercises the parser rather than a
 * restatement of the arithmetic.
 */
TEST(TeslaParserBitWidths, CurrentSenseMiaReadsOnlyItsOwnBit) {
  TeslaBattery battery;

  auto hvp_mux0 = [](uint8_t byte7) {
    CAN_frame f{};
    f.ID = 0x7AA;
    f.DLC = 8;
    f.data.u8[0] = 0;  // HVP_debugMessageMultiplexer = 0, the block carrying the MIA bits
    f.data.u8[7] = byte7;
    return f;
  };

  // Only the neighbour's bit (59) set: current-sense MIA must stay false.
  battery.handle_incoming_can_frame(hvp_mux0(1u << 3));
  battery.update_values();  // the datalayer copy lives here, not in the frame handler
  EXPECT_FALSE(datalayer_extended.tesla.HVP_currentSenseMia)
      << "the ref-voltage-mismatch bit alone must not report current-sense MIA";
  EXPECT_TRUE(datalayer_extended.tesla.HVP_shuntRefVoltageMismatch) << "and its own field must be set";

  // Only its own bit (58) set.
  battery.handle_incoming_can_frame(hvp_mux0(1u << 2));
  battery.update_values();
  EXPECT_TRUE(datalayer_extended.tesla.HVP_currentSenseMia);
  EXPECT_FALSE(datalayer_extended.tesla.HVP_shuntRefVoltageMismatch);

  // Neither.
  battery.handle_incoming_can_frame(hvp_mux0(0));
  battery.update_values();
  EXPECT_FALSE(datalayer_extended.tesla.HVP_currentSenseMia);
  EXPECT_FALSE(datalayer_extended.tesla.HVP_shuntRefVoltageMismatch);
}
