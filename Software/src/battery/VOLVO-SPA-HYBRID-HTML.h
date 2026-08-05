#ifndef _VOLVO_SPA_HYBRID_HTML_H
#define _VOLVO_SPA_HYBRID_HTML_H

#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class VolvoSpaHybridHtmlRenderer : public BatteryHtmlRenderer {
 public:
  explicit VolvoSpaHybridHtmlRenderer(DATALAYER_INFO_VOLVO_HYBRID* d) : data(d) {}

  String get_status_html() {
    String content;

    content += "<h4>BECM reported SOC: " + String(data->soc_bms) + "</h4>";
    content += "<h4>Calculated SOC: " + String(data->soc_calc) + "</h4>";
    content += "<h4>Rescaled SOC: " + String(data->soc_rescaled / 10) + "</h4>";
    content += "<h4>BECM reported SOH: " + String(data->soh_bms) + "</h4>";
    content += "<h4>BECM supply voltage: " + String(data->BECMsupplyVoltage) + " mV</h4>";

    content += "<h4>HV voltage: " + String(data->BECMBatteryVoltage) + " V</h4>";
    content += "<h4>HV current: " + String(data->BECMBatteryCurrent) + " A</h4>";
    content += "<h4>Dynamic max voltage: " + String(data->BECMUDynMaxLim) + " V</h4>";
    content += "<h4>Dynamic min voltage: " + String(data->BECMUDynMinLim) + " V</h4>";

    content += "<h4>Discharge power limit 1: " + String(data->HvBattPwrLimDcha1) + " kW</h4>";
    content += "<h4>Discharge soft power limit: " + String(data->HvBattPwrLimDchaSoft) + " kW</h4>";

    content += "<h4>HV system relay status: ";
    switch (data->HVSysRlySts) {
      case 0:
        content += String("Open");
        break;
      case 1:
        content += String("Closed");
        break;
      case 2:
        content += String("KeepStatus");
        break;
      case 3:
        content += String("OpenAndRequestActiveDischarge");
        break;
      default:
        content += String("Not valid");
    }
    content += "</h4><h4>HV system relay status 1: ";
    switch (data->HVSysDCRlySts1) {
      case 0:
        content += String("Open");
        break;
      case 1:
        content += String("Closed");
        break;
      case 2:
        content += String("KeepStatus");
        break;
      case 3:
        content += String("Fault");
        break;
      default:
        content += String("Not valid");
    }
    content += "</h4><h4>HV system relay status 2: ";
    switch (data->HVSysDCRlySts2) {
      case 0:
        content += String("Open");
        break;
      case 1:
        content += String("Closed");
        break;
      case 2:
        content += String("KeepStatus");
        break;
      case 3:
        content += String("Fault");
        break;
      default:
        content += String("Not valid");
    }
    content += "</h4><h4>HV system isolation resistance monitoring status: ";
    switch (data->HVSysIsoRMonrSts) {
      case 0:
        content += String("Not valid 1");
        break;
      case 1:
        content += String("False");
        break;
      case 2:
        content += String("True");
        break;
      case 3:
        content += String("Not valid 2");
        break;
      default:
        content += String("Not valid");
    }

    return content;
  }

 private:
  DATALAYER_INFO_VOLVO_HYBRID* data;
};

#endif
