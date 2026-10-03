#ifndef _TESLA_HTML_H
#define _TESLA_HTML_H

#include <cstdio>
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

// Names a code out of one of the renderer's lookup tables, without reading past its end.
//
// Every table below is selected by a datalayer_extended field the CAN parser fills from a raw bit
// slice of a frame, and several of those slices are wider than the table they select from: a 5-bit
// PCS sub-state picks from 18 entries, a 4-bit BMS state from 10, a 2-bit contactor request status
// from 3. A pack reporting a code the table has no entry for used to hand String() whatever
// pointer-shaped bytes followed the table - a wild dereference on the ESP32, and a segfault on the
// host. Out of range now renders as UNKNOWN(n), which is both safe and more useful than a name:
// it shows the code the pack actually sent. UNKNOWN(n) is this file's own spelling for a code with
// no name - the tables above pad their tails with exactly those strings - and the same battery's
// serial-logging twins (getContactorText() and friends in TESLA-BATTERY.cpp) have always had a
// switch default for the same reason. Their default returns a bare "UNKNOWN" rather than the code;
// carrying the number is the one deliberate difference, because a page that says which value the
// pack sent is diagnosable and one that says "UNKNOWN" is not.
//
// Taking the table by reference is what makes the bound automatic - the length comes from the
// array's own type, so a table that gains or loses an entry cannot leave a hardcoded limit behind.
template <size_t N>
static String lookupName(const char* const (&table)[N], uint8_t index) {
  if (index < N) {
    return String(table[index]);
  }
  char unknown[sizeof("UNKNOWN(255)")];  // The widest a uint8_t code can render
  snprintf(unknown, sizeof(unknown), "UNKNOWN(%u)", static_cast<unsigned>(index));
  return String(unknown);
}

class TeslaHtmlRenderer : public BatteryHtmlRenderer {
 public:
  TeslaHtmlRenderer(DATALAYER_INFO_TESLA* extended, DATALAYER_BATTERY_TYPE* core)
      : tesla_info(extended), core_data(core) {}

  bool renders_own_battery_data() { return true; }

  String get_status_html();

 private:
  DATALAYER_INFO_TESLA* tesla_info;
  DATALAYER_BATTERY_TYPE* core_data;
};

#endif
