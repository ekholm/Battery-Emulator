#ifndef _BYD_ATTO_3_HTML_H
#define _BYD_ATTO_3_HTML_H

#include <Arduino.h>
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class BydAtto3HtmlRenderer : public BatteryHtmlRenderer {
 public:
  BydAtto3HtmlRenderer(DATALAYER_INFO_BYDATTO3* dl, const String& sfx = "") : byd_datalayer(dl), s(sfx) {}

  bool renders_own_battery_data() { return true; }

  bool html_render_failed() const override { return render_failed; }

  String get_dtc_html() override;

  String get_status_html();

 private:
  bool render_failed = false;

  void append_balance_time_html(CheckedHtml& out) const;

  DATALAYER_INFO_BYDATTO3* byd_datalayer;
  String s;
};

#endif
