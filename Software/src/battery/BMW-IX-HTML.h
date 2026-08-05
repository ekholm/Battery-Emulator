#ifndef _BMW_IX_HTML_H
#define _BMW_IX_HTML_H

#include <Arduino.h>
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class BmwIXBattery;

class BmwIXHtmlRenderer : public BatteryHtmlRenderer {
 private:
  BmwIXBattery& batt;
  DATALAYER_INFO_BMWIX* data;

 public:
  BmwIXHtmlRenderer(BmwIXBattery& b, DATALAYER_INFO_BMWIX* d) : batt(b), data(d) {}

  String get_status_html();
};

#endif
