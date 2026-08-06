#ifndef _MEB_HTML_H
#define _MEB_HTML_H

#include "../datalayer/datalayer.h"
#include "../datalayer/datalayer_extended.h"
#include "../devboard/webserver/BatteryHtmlRenderer.h"

// Mirrors the scaling applied in MEB-BATTERY.cpp when BMS_voltage_intermediate is decoded
static constexpr int32_t meb_interm_raw_to_dV(int32_t raw) {
  return (raw - 2000) * 10 / 2;
}

class MebHtmlRenderer : public BatteryHtmlRenderer {
 public:
  explicit MebHtmlRenderer(DATALAYER_INFO_MEB* meb) : meb_(meb) {}

  // platform's battery setup() overrides
  const char* dtc_json_filename = "vag_meb_dtc.json";

  String get_status_html() {
    String content;
    content.reserve(4096);

    static const char* const kl30c_enum[] = {"Init", "Closed", "Open!", "Fault"};  // also used for HVIL
    static const char* const bms_mode_enum[] = {"HV inactive", "HV active",     "Balancing",   "Extern charging",
                                                "AC charging", "Battery error", "DC charging", "Init"};
    static const char* const balancing_enum[] = {"Init", "Active", "Inactive"};
    static const char* const diagnostic_enum[] = {"Init", "Battery display",       "?",    "?", "Battery display OK",
                                                  "?",    "Battery display check", "Fault"};
    static const char* const ptc_line_enum[] = {"Init", "Heater no open HV line", "Heater open HV line", "Fault"};
    static const char* const welded_enum[] = {"Init", "No contactor welded", "At least 1 contactor welded",
                                              "Protection status detection error"};
    static const char* const warning_support_enum[] = {"OK", "Not OK", "?", "?", "?", "?", "Init", "Fault"};
    static const char* const voltage_free_enum[] = {"Init", "BMS interm circuit voltage free (U<20V)",
                                                    "BMS interm circuit not voltage free (U >= 25V)", "Error"};
    static const char* const bms_error_enum[] = {
        "Component IO", "Iso Error 1",     "Iso Error 2",           "Interlock",
        "SD",           "Performance red", "No component function", "Init"};

    add_h4(content, "Service disconnect switch", meb_->SDSW ? "Missing!" : "OK");
    add_h4(content, "Pilotline", meb_->pilotline ? "Open!" : "OK");
    add_h4(content, "Transportmode", meb_->transportmode ? "Locked!" : "OK");
    add_h4(content, "Shutdown", meb_->shutdown_active ? "Active!" : "No");
    add_h4(content, "Component protection", meb_->componentprotection ? "Active!" : "No");
    add_h4(content, "HVIL status", enum_str(kl30c_enum, meb_->HVIL));
    add_h4(content, "KL30C status", enum_str(kl30c_enum, meb_->BMS_Kl30c_Status));
    add_h4(content, "BMS mode", enum_str(bms_mode_enum, meb_->BMS_mode));
    add_h4(content, "Charging", meb_->charging_active ? "Active" : "Not active");
    add_h4(content, "Balancing", enum_str(balancing_enum, meb_->balancing_active));
    add_h4(content, "Slow charging", meb_->balancing_request ? "Requested" : "Not requested");
    add_h4(content, "Diagnostic", enum_str(diagnostic_enum, meb_->battery_diagnostic));

    content += "<h4>Heater HV line status: ";
    if (meb_->status_HV_PTC_line < 4) {
      content += ptc_line_enum[meb_->status_HV_PTC_line];
    } else {
      content += "? ";
      content += String(meb_->status_HV_PTC_line);
    }
    content += "</h4>";

    add_h4(content, "BMS fault performance", meb_->BMS_fault_performance ? "Active!" : "Off");
    add_h4(content, "BMS fault emergency shutdown crash", meb_->BMS_fault_emergency_shutdown_crash ? "Active!" : "Off");
    add_h4(content, "BMS error shutdown request", meb_->BMS_error_shutdown_request ? "Active!" : "Inactive");
    add_h4(content, "BMS error shutdown", meb_->BMS_error_shutdown ? "Active!" : "Off");
    add_h4(content, "Welded contactors", enum_str(welded_enum, meb_->BMS_welded_contactors_status));
    add_h4(content, "Warning support", enum_str(warning_support_enum, meb_->warning_support));

    // Raw 0xFFD-0xFFF are status codes; showing them as ~1047V would look like a real reading.
    constexpr int32_t not_meas_code_dV = meb_interm_raw_to_dV(0xFFD);
    constexpr int32_t init_code_dV = meb_interm_raw_to_dV(0xFFE);
    constexpr int32_t fault_code_dV = meb_interm_raw_to_dV(0xFFF);
    const int32_t interm_dV = meb_->BMS_voltage_intermediate_dV;
    content += "<h4>Interm. Voltage (";
    if (interm_dV == not_meas_code_dV) {
      content += "NOT measurable";
    } else if (interm_dV == init_code_dV) {
      content += "Init";
    } else if (interm_dV == fault_code_dV) {
      content += "Fault";
    } else {
      content += String(interm_dV / 10.0f, 1);
      content += "V";
    }
    content += ") status: ";
    content += enum_str(voltage_free_enum, meb_->BMS_status_voltage_free);
    content += "</h4>";

    add_h4(content, "BMS error status", enum_str(bms_error_enum, meb_->BMS_error_status));
    add_h4(content, "BMS voltage", String(meb_->BMS_voltage_dV / 10.0f, 1));
    add_h4(content, "OBD MIL", meb_->BMS_OBD_MIL ? "ON!" : "Off");
    add_h4(content, "Red error lamp", meb_->BMS_error_lamp_req ? "ON!" : "Off");
    add_h4(content, "Yellow warning lamp", meb_->BMS_warning_lamp_req ? "ON!" : "Off");
    add_h4(content, "Isolation resistance", String(meb_->isolation_resistance) + " kOhm");
    add_h4(content, "Battery heating", meb_->battery_heating ? "Active!" : "Off");

    // The values are copied out by name on purpose: indexing straight off &rt_overcurrent works
    // today but would silently mislabel everything if the struct is ever reordered.
    static const char* const rt_enum[] = {"No", "Error level 1", "Error level 2", "Error level 3"};
    static const char* const rt_labels[] = {
        "Overcurrent",          "CAN fault",        "Overcharged",       "SOC too high",   "SOC too low",
        "SOC jumping",          "Temp difference",  "Cell overtemp",     "Cell undertemp", "Battery overvoltage",
        "Battery undervoltage", "Cell overvoltage", "Cell undervoltage", "Cell imbalance", "Battery unathorized"};
    const uint8_t rt_values[] = {meb_->rt_overcurrent,      meb_->rt_CAN_fault,        meb_->rt_overcharge,
                                 meb_->rt_SOC_high,         meb_->rt_SOC_low,          meb_->rt_SOC_jumping,
                                 meb_->rt_temp_difference,  meb_->rt_cell_overtemp,    meb_->rt_cell_undertemp,
                                 meb_->rt_battery_overvolt, meb_->rt_battery_undervol, meb_->rt_cell_overvolt,
                                 meb_->rt_cell_undervol,    meb_->rt_cell_imbalance,   meb_->rt_battery_unathorized};
    static_assert(sizeof(rt_values) == sizeof(rt_labels) / sizeof(rt_labels[0]), "rt label/value table mismatch");
    for (size_t i = 0; i < sizeof(rt_values); i++) {
      add_h4(content, rt_labels[i], rt_enum[rt_values[i] & 0x03]);
    }

    content += "<h4>Battery temperature: ";
    if (meb_->battery_temperature_dC == 875) {  //Raw value 255
      content += "Error</h4>";
    } else if (meb_->battery_temperature_dC == 870) {  //Raw value 254
      content += "Init</h4>";
    } else {
      content += String(meb_->battery_temperature_dC / 10.f, 1) + " &deg;C</h4>";
    }

    for (int i = 0; i < 3; i++) {
      content += "<h4>Temperature points " + String(i * 6 + 1) + "-" + String(i * 6 + 6) + " :";
      for (int j = 0; j < 6; j++)
        content += " &nbsp;" + String(meb_->temp_points[i * 6 + j], 1);
      content += " &deg;C</h4>";
    }
    bool temps_done = false;
    for (int i = 0; i < 7 && !temps_done; i++) {
      content += "<h4>Cell temperatures " + String(i * 8 + 1) + "-" + String(i * 8 + 8) + " :";
      for (int j = 0; j < 8; j++) {
        if (meb_->celltemperature_dC[i * 8 + j] == 865) {
          temps_done = true;
          break;
        } else {
          content += " &nbsp;" + String(meb_->celltemperature_dC[i * 8 + j] / 10.f, 1);
        }
      }
      content += " &deg;C</h4>";
    }
    add_h4(content, "Total charged",
           String(datalayer.batteries[0].status.total_charged_battery_Wh / 1000.0, 1) + " kWh");
    add_h4(content, "Total discharged",
           String(datalayer.batteries[0].status.total_discharged_battery_Wh / 1000.0, 1) + " kWh");

    content += BatteryHtmlRenderer::render_dtc_section_html(datalayer.batteries[0].dtc, dtc_json_filename, false);

    return content;
  }

 private:
  DATALAYER_INFO_MEB* meb_;

  // Emits "<h4>label: value</h4>" from separately stored fragments, so no label prefix is
  // duplicated in flash and the repeated values pool into a single copy each.
  static void add_h4(String& out, const char* label, const char* value) {
    out += "<h4>";
    out += label;
    out += ": ";
    out += value;
    out += "</h4>";
  }

  static void add_h4(String& out, const char* label, const String& value) {
    out += "<h4>";
    out += label;
    out += ": ";
    out += value;
    out += "</h4>";
  }

  // Bounds checked table lookup, reproducing the `default: "?"` of the switches this replaces.
  // Taking the array by reference deduces N, so a table and its length can never drift apart.
  template <size_t N>
  static const char* enum_str(const char* const (&table)[N], uint8_t value) {
    return value < N ? table[value] : "?";
  }
};

#endif
