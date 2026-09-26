#ifndef _TESLA_HTML_H
#define _TESLA_HTML_H

#include <cstring>
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class TeslaHtmlRenderer : public BatteryHtmlRenderer {
 public:
  String get_status_html();
};

#endif
