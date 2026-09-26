#ifndef _NISSAN_LEAF_HTML_H
#define _NISSAN_LEAF_HTML_H

#include <cstring>
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class NissanLeafHtmlRenderer : public BatteryHtmlRenderer {
 public:
  NissanLeafHtmlRenderer(DATALAYER_BATTERY_TYPE* battery_dl, DATALAYER_INFO_NISSAN_LEAF* dl)
      : battery_dl(battery_dl), nissan_dl(dl) {}

  bool renders_own_battery_data() { return true; }

  String get_status_html();

  //The trouble codes, in a panel of their own after the status ones. The Read and Erase DTC
  //buttons the page adds next land in the same panel.
  String get_dtc_html() { return battery_dl ? render_dtc_section(battery_dl->dtc) : String(); }

#ifndef SMALL_FLASH_DEVICE
  //The degradation reset gets a panel of its own: the challenge values its sequence fills in, above
  //the button that starts it.
  String get_command_prefix_html(const char* identifier);
#endif

 private:
  //One status row, Unknown until the broadcast carrying it has arrived. A true/false flag shows as
  //a tick or a cross. A wider field is given the name of its state instead, shown with the raw
  //value in brackets unless that is 0, so a reading can still be matched against a CAN log.
  static void status_row(String& content, const char* label, uint8_t value, bool known, const char* name = nullptr);

  //The page streams all of this inside a battery-panel div of its own and closes the last one after
  //the command buttons. Closing the current panel and opening the next is all it takes to split the
  //Leaf's information into several, each under a title of its own.
  static void new_panel(String& content, const char* title);

  // The LBC reports standard 3-byte DTCs, but Nissan service data, LeafSpy and nissan_leaf_dtc.json
  // all use the 5-character short form (P33D7, U1000) built from the first two bytes only. That is
  // therefore what goes into data-dtc-code for the JSON loader to match on. The third byte is the
  // failure type: it is appended for display when set ("P33D7-2F") so nothing is silently dropped,
  // but it stays out of the lookup key.
  static String render_dtc_section(DATALAYER_BATTERY_DTC_TYPE& dtc);

  DATALAYER_BATTERY_TYPE* battery_dl;
  DATALAYER_INFO_NISSAN_LEAF* nissan_dl;
};

#endif
