#ifndef _GEELY_GEOMETRY_C_HTML_H
#define _GEELY_GEOMETRY_C_HTML_H

#include <cstring>  //For unit test
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class GeelyGeometryCHtmlRenderer : public BatteryHtmlRenderer {
 public:
  GeelyGeometryCHtmlRenderer(DATALAYER_INFO_GEELY_GEOMETRY_C* data) : data(data) {}
  String get_status_html() {
    String content;
    char readableSerialNumber[29];  // One extra space for null terminator
    memcpy(readableSerialNumber, data->BatterySerialNumber, sizeof(data->BatterySerialNumber));
    readableSerialNumber[28] = '\0';   // Null terminate the string
    char readableSoftwareVersion[17];  // One extra space for null terminator
    memcpy(readableSoftwareVersion, data->BatterySoftwareVersion, sizeof(data->BatterySoftwareVersion));
    readableSoftwareVersion[16] = '\0';  // Null terminate the string
    char readableHardwareVersion[17];    // One extra space for null terminator
    memcpy(readableHardwareVersion, data->BatteryHardwareVersion, sizeof(data->BatteryHardwareVersion));
    readableHardwareVersion[16] = '\0';  // Null terminate the string
    content += "<h4>Serial number: " + String(readableSoftwareVersion) + "</h4>";
    content += "<h4>Software version: " + String(readableSerialNumber) + "</h4>";
    content += "<h4>Hardware version: " + String(readableHardwareVersion) + "</h4>";
    content += "<h4>SOC display: " + String(data->soc) + "ppt</h4>";
    content += "<h4>CC2 voltage: " + String(data->CC2voltage) + "mV</h4>";
    content += "<h4>Cell max voltage number: " + String(data->cellMaxVoltageNumber) + "</h4>";
    content += "<h4>Cell min voltage number: " + String(data->cellMinVoltageNumber) + "</h4>";
    content += "<h4>Cell total amount: " + String(data->cellTotalAmount) + "S</h4>";
    content += "<h4>Specificial Voltage: " + String(data->specificialVoltage) + "dV</h4>";
    content += "<h4>Unknown1: " + String(data->unknown1) + "</h4>";
    content += "<h4>Raw SOC max: " + String(data->rawSOCmax) + "</h4>";
    content += "<h4>Raw SOC min: " + String(data->rawSOCmin) + "</h4>";
    content += "<h4>Unknown4: " + String(data->unknown4) + "</h4>";
    content += "<h4>Capacity module max: " + String((data->capModMax / 10)) + "Ah</h4>";
    content += "<h4>Capacity module min: " + String((data->capModMin / 10)) + "Ah</h4>";
    content += "<h4>Unknown7: " + String(data->unknown7) + "</h4>";
    content += "<h4>Unknown8: " + String(data->unknown8) + "</h4>";
    content += "<h4>Module 1 temperature: " + String(data->ModuleTemperatures[0]) + " &deg;C</h4>";
    content += "<h4>Module 2 temperature: " + String(data->ModuleTemperatures[1]) + " &deg;C</h4>";
    content += "<h4>Module 3 temperature: " + String(data->ModuleTemperatures[2]) + " &deg;C</h4>";
    content += "<h4>Module 4 temperature: " + String(data->ModuleTemperatures[3]) + " &deg;C</h4>";
    content += "<h4>Module 5 temperature: " + String(data->ModuleTemperatures[4]) + " &deg;C</h4>";
    content += "<h4>Module 6 temperature: " + String(data->ModuleTemperatures[5]) + " &deg;C</h4>";
    return content;
  }

 private:
  DATALAYER_INFO_GEELY_GEOMETRY_C* data;
};

#endif
