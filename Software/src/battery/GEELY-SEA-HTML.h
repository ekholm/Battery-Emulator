#ifndef _GEELY_SEA_HTML_H
#define _GEELY_SEA_HTML_H

#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class GeelySeaHtmlRenderer : public BatteryHtmlRenderer {
 public:
  explicit GeelySeaHtmlRenderer(DATALAYER_INFO_GEELY_SEA* data) : data(data) {}

  String get_status_html() {
    String content;
    content += "</h4><h4>BECM reported number of DTCs: " + String(data->DTCcount) + "</h4>";
    content += "</h4><h4>Inhibition status (crash): " + String(data->CrashStatus) + "</h4>";
    content += "<h4>BECM reported SOC: " + String(data->soc_bms / 100.0) + " %</h4>";
    content += "<h4>BECM reported SOH: " + String(data->soh_bms / 100.0) + " %</h4>";
    content += "<h4>HV voltage: " + String(data->BECMBatteryVoltage / 100.0) + " V</h4>";
    //content += "<h4>Battery current: " + String((data->BatteryCurrent / 10.0) - 1638) + " A</h4>";
    content += "<h4>Highest cell voltage: " + String(data->CellVoltHighest / 1000.00) + " V</h4>";
    content += "<h4>Lowest cell voltage: " + String(data->CellVoltLowest / 1000.00) + " V</h4>";
    content += "<h4>BECM supply voltage: " + String(data->BECMsupplyVoltage / 1000.0) + " V</h4>";
    content += "<h4>Cell count: " + String(datalayer.batteries[0].info.number_of_cells) + "</h4>";
    content += "<h4>Highest cell temp: " + String((data->CellTempHighest / 100.0) - 50.0) + " ºC</h4>";
    content += "<h4>Average cell temp: " + String((data->CellTempAverage / 100.0) - 50.0) + " ºC</h4>";
    content += "<h4>Lowest cell temp: " + String((data->CellTempLowest / 100.0) - 50.0) + " ºC</h4>";
    content += "<h4>HVIL Circuit 1 (M1+M2+FC connectors) status : ";
    switch (data->Interlock & 0x80) {
      case 0x80:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>HVIL Circuit 2 (LV connector pin 9-10) status: ";
    switch (data->Interlock & 0x40) {
      case 0x40:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>HVIL Circuit 3 (LV connector pin 8-12) status: ";
    switch (data->Interlock & 0x04) {
      case 0x04:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>Unknow Contactor Status 1 (Negative FC?): ";
    switch (data->Interlock & 0x01) {
      case 0x01:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>Unknown Contactor Status 2 (Positive FC?): ";
    switch (data->Interlock & 0x02) {
      case 0x02:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>Negative Contactor Status: ";
    switch (data->Interlock & 0x08) {
      case 0x08:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>Precharge Contactor Status: ";
    switch (data->Interlock & 0x10) {
      case 0x10:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>Positive Contactor Status: ";
    switch (data->Interlock & 0x20) {
      case 0x20:
        content += String("Open");
        break;
      default:
        content += String("Closed");
    }
    content += "<h4>";
    return content;
  }

 private:
  DATALAYER_INFO_GEELY_SEA* data;
};

#endif
