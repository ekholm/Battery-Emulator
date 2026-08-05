#ifndef _RIVIAN_HTML_H
#define _RIVIAN_HTML_H

#include <cstring>
#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

class RivianHtmlRenderer : public BatteryHtmlRenderer {
 public:
  RivianHtmlRenderer(DATALAYER_INFO_RIVIAN* d) : data(d) {}

  String get_status_html() {
    String content;

    content += "<h4>Voltage, pre contactors: " + String(data->pre_contactor_voltage) + " dV</h4>";
    content += "<h4>Voltage, main contactors: " + String(data->main_contactor_voltage) + " dV</h4>";
    content += "<h4>Voltage, reference: " + String(data->voltage_reference) + " dV</h4>";
    content += "<h4>Voltage, DCFC contactors: " + String(data->DCFC_contactor_voltage) + " dV</h4>";

    content += "<h4>SOC, max: " + String(data->battery_SOC_max) + " pptt</h4>";
    content += "<h4>SOC, min: " + String(data->battery_SOC_min) + " pptt</h4>";

    if (data->NACS_charger_detected) {
      content += "<h4>NACS charger detected!</h4>";
    }

    content += "<h4>Isolation measurement ongoing: ";
    if (data->IsolationMeasurementOngoing) {
      content += "Yes</h4>";
    } else {
      content += "No</h4>";
    }

    content += "<h4>Isolation Status: ";
    if (data->isolation_fault_status == 0) {
      content += "Undefined";
    } else if (data->isolation_fault_status == 1) {
      content += "Stable";
    } else if (data->isolation_fault_status == 2) {
      content += "No Fault";
    } else if (data->isolation_fault_status == 3) {
      content += "High Side Fault";
    } else if (data->isolation_fault_status == 4) {
      content += "Low Side Fault";
    } else if (data->isolation_fault_status == 5) {
      content += "Dual Side Fault";
    } else if (data->isolation_fault_status == 6) {
      content += "Iso Circuit Failure";
    } else if (data->isolation_fault_status == 7) {
      content += "Iso Circuit Check timeout";
    }
    content += "</h4>";

    content += "<h4>Interlock status: ";
    if (data->HVIL == 0) {
      content += "NOT OK";
    } else if (data->HVIL == 1) {
      content += "NOT OK";
    } else if (data->HVIL == 2) {
      content += "NOT OK";
    } else if (data->HVIL == 3) {
      content += "OK";
    }
    content += "</h4>";

    content += "<h4>BMS State: ";
    if (data->BMS_state == 0) {
      content += "Sleep";
    } else if (data->BMS_state == 1) {
      content += "Standby";
    } else if (data->BMS_state == 2) {
      content += "Ready";
    } else if (data->BMS_state == 3) {
      content += "Go";
    }
    content += "</h4>";

    content += "<h4>Contactor State: ";
    if (data->contactor_state == 0) {
      content += "Open";
    } else if (data->contactor_state == 1) {
      content += "Closed";
    } else if (data->contactor_state == 2) {
      content += "Precharge";
    } else if (data->contactor_state == 3) {
      content += "Turning off";
    } else if (data->contactor_state == 4) {
      content += "Initialization";
    } else if (data->contactor_state == 5) {
      content += "FAILURE";
    }
    content += "</h4>";

    content += "<h4>Active errors and faults:</h4>";

    if (data->error_relay_open) {
      content += "<h4>Error Relay Open</h4>";
    }
    if (data->error_flags_from_BMS & 0x01) {
      content += "<h4>Error Isolation Single</h4>";
    }
    if ((data->error_flags_from_BMS & 0x02) >> 1) {
      content += "<h4>Error Isolation Double</h4>";
    }
    if ((data->error_flags_from_BMS & 0x04) >> 2) {
      content += "<h4>Error Emergency Off CRASH</h4>";
    }
    if ((data->error_flags_from_BMS & 0x08) >> 3) {
      content += "<h4>Error Emergency Off Pilot</h4>";
    }
    if ((data->error_flags_from_BMS & 0x10) >> 4) {
      content += "<h4>Error Emergency Off Request</h4>";
    }
    if ((data->error_flags_from_BMS & 0x20) >> 5) {
      content += "<h4>Error Emergency Off</h4>";
    }
    if ((data->error_flags_from_BMS & 0x40) >> 6) {
      content += "<h4>Error Contactors Welded</h4>";
    }
    if ((data->error_flags_from_BMS & 0x80) >> 7) {
      content += "<h4>Error Limited Power</h4>";
    }

    //HMI errors / status codes, bundle them also under errors

    if ((data->HMI_part1 & 0x10) >> 4) {
      content += "<h4>Vehicle Battery Issue</h4>";
    }
    if ((data->HMI_part1 & 0x20) >> 5) {
      content += "<h4>Critical Battery Issue</h4>";
    }
    if ((data->HMI_part1 & 0x40) >> 6) {
      content += "<h4>AC performance limited</h4>";
    }
    if ((data->HMI_part1 & 0x80) >> 7) {
      content += "<h4>DC performance limited</h4>";
    }
    if (data->HMI_part2 & 0x01) {
      content += "<h4>DC charging disabled</h4>";
    }
    if ((data->HMI_part2 & 0x02) >> 1) {
      content += "<h4>Electric hazard</h4>";
    }
    if ((data->HMI_part2 & 0x04) >> 2) {
      content += "<h4>Fire risk</h4>";
    }
    if ((data->HMI_part2 & 0x08) >> 3) {
      content += "<h4>Vehicle system fault</h4>";
    }
    if ((data->HMI_part2 & 0x10) >> 4) {
      content += "<h4>Battery electric malfunction</h4>";
    }

    //Misc
    if (data->system_safe_state > 1) {
      content += "<h4>System safe state A active</h4>";
    }
    if (data->puncture_fault) {
      content += "<h4>Puncture fault detected</h4>";
    }
    if (data->liquid_fault) {
      content += "<h4>Liquid fault detected</h4>";
    }
    if (data->contactor_DCFC_welded) {
      content += "<h4>DCFC contactor welded</h4>";
    }

    if (data->slewrate_potential_violation) {
      content += "<h4>Slewrate potential violation</h4>";
    }
    if (data->minimum_power_potential_violation) {
      content += "<h4>Min power potential violation</h4>";
    }
    if (data->operation_limit_violation_warning) {
      content += "<h4>Operation limit violation warning</h4>";
    }

    return content;
  }

 private:
  DATALAYER_INFO_RIVIAN* data;
};

#endif
