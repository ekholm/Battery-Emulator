#ifndef _KIA_64FD_HTML_H
#define _KIA_64FD_HTML_H

#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class Kia64FDHtmlRenderer : public BatteryHtmlRenderer {
 public:
  Kia64FDHtmlRenderer(DATALAYER_INFO_KIA64FD* dl) : kia_datalayer(dl) {}

  bool renders_own_battery_data() { return true; }

  String get_status_html();

 private:
  DATALAYER_INFO_KIA64FD* kia_datalayer;
};

#endif
