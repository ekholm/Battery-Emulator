#pragma once

// ---------------------------------------------------------------------------
// The settings declaration list: every setting in the system, one row each,
// in one place. The row constructor name doubles as the dispatch tag - a
// consumer defines BE_ROW_<Ctor> for the row kinds it wants to consume and
// includes settings_rows_null.h to turn every other row kind into nothing.
//
// Consumers today:
//   settings.cpp   - expands each row kind into the corresponding typed
//                    table (PERSISTED_UINTS ... VOLATILE_SCALEDS), exactly
//                    as they were previously written by hand.
//   the key check  - expands the persisted/instant rows into a constexpr
//                    key list and static_asserts NVS-key rules over it
//                    (settings.cpp, bottom).
//
// Row argument meanings per kind are documented at each table in
// settings.cpp (name/min/max/scale/storage/apply/read as before).
//
// Board-conditional rows use the BE_IF_* wrappers below instead of #ifdef -
// a preprocessor conditional cannot live inside a macro definition.
// ---------------------------------------------------------------------------

#ifdef HW_LILYGO2CAN
#define BE_IF_LILYGO2CAN(row) row
#else
#define BE_IF_LILYGO2CAN(row)
#endif
#ifdef HW_STARK
#define BE_IF_STARK(row) row
#else
#define BE_IF_STARK(row)
#endif
#ifdef HW_WAVESHARE
#define BE_IF_WAVESHARE(row) row
#else
#define BE_IF_WAVESHARE(row)
#endif
#ifdef SDCARD
#define BE_IF_SDCARD(row) row
#else
#define BE_IF_SDCARD(row)
#endif

// clang-format off
#define BE_SETTINGS_LIST \
  BE_ROW_UintSetting("INVTYPE", 0, (uint32_t)InverterProtocolType::Highest - 1, &user_selected_inverter_protocol) \
  BE_ROW_UintSetting("INVCOMM", 0, (uint32_t)comm_interface::Highest - 1, &user_selected_inv_comm) \
  BE_ROW_UintSetting("BATTTYPE", 0, (uint32_t)BatteryType::Highest - 1, &user_selected_battery_type) \
  BE_ROW_UintSetting("BATTCHEM", 0, (uint32_t)battery_chemistry_enum::Highest - 1, &user_selected_battery_chemistry) \
  BE_ROW_UintSetting("BATTCOMM", 0, (uint32_t)comm_interface::Highest - 1, &user_selected_batt_comm) \
  BE_ROW_UintSetting("BATTCVMAX", 0, 5000, &user_selected_max_cell_voltage_mV) \
  BE_ROW_UintSetting("BATTCVMIN", 0, 5000, &user_selected_min_cell_voltage_mV) \
  BE_ROW_UintSetting("CHGTYPE", 0, (uint32_t)ChargerType::Highest - 1, &user_selected_charger_type) \
  BE_ROW_UintSetting("CHGCOMM", 0, (uint32_t)comm_interface::Highest - 1, &user_selected_chg_comm) \
  BE_ROW_UintSetting("EQSTOP", 0, (uint32_t)STOP_BUTTON_BEHAVIOR::Highest - 1, &equipment_stop_behavior) \
  BE_ROW_UintSetting("BATT2COMM", 0, (uint32_t)comm_interface::Highest - 1, &user_selected_batt2_comm) \
  BE_ROW_UintSetting("BATT3COMM", 0, (uint32_t)comm_interface::Highest - 1, &user_selected_batt3_comm) \
  BE_ROW_UintSetting("SHUNTTYPE", 0, (uint32_t)ShuntType::Highest - 1, &user_selected_shunt_type) \
  BE_ROW_UintSetting("SHUNTCOMM", 0, (uint32_t)comm_interface::Highest - 1, &user_selected_shunt_comm) \
  BE_ROW_UintSetting("MAXPRETIME", 0, 120000, &precharge_max_precharge_time_before_fault) \
  BE_ROW_UintSetting("MAXPREFREQ", 0, 65535, &Precharge_max_PWM_Freq) \
  BE_ROW_UintSetting("WIFICHANNEL", 0, 14, &wifi_channel) \
  BE_ROW_UintSetting("DCHGPOWER", 0, 100000, &datalayer.battery.status.override_discharge_power_W) \
  BE_ROW_UintSetting("CHGPOWER", 0, 100000, &datalayer.battery.status.override_charge_power_W) \
  BE_ROW_UintSetting("MQTTPORT", 0, 65535, &mqtt_port) \
  BE_ROW_UintSetting("MQTTTIMEOUT", 0, 30000, &mqtt_timeout_ms) \
  BE_ROW_UintSetting("MQTTPUBLISHMS", 0, 3600000, &mqtt_publish_interval_ms) \
  BE_ROW_UintSetting("SOFAR_ID", 0, 255, &datalayer.battery.settings.sofar_user_specified_battery_id) \
  BE_ROW_UintSetting("INVCELLS", 0, 65535, &user_selected_inverter_cells) \
  BE_ROW_UintSetting("INVMODULES", 0, 65535, &user_selected_inverter_modules) \
  BE_ROW_UintSetting("INVCELLSPER", 0, 65535, &user_selected_inverter_cells_per_module) \
  BE_ROW_UintSetting("INVVLEVEL", 0, 65535, &user_selected_inverter_voltage_level) \
  BE_ROW_UintSetting("INVCAPACITY", 0, 65535, &user_selected_inverter_ah_capacity) \
  BE_ROW_UintSetting("INVBTYPE", 0, 255, &user_selected_inverter_battery_type) \
  BE_ROW_UintSetting("INVICNT", 0, 2, &user_selected_inverter_contactor_mode) \
  BE_ROW_UintSetting("PRECHGMS", 0, 120000, &precharge_time_ms) \
  BE_ROW_UintSetting("PWMFREQ", 0, 65535, &pwm_frequency) \
  BE_ROW_UintSetting("PWMHOLD", 0, 1023, &pwm_hold_duty) \
  BE_ROW_UintSetting("GTWCOUNTRY", 0, 65535, &user_selected_tesla_GTW_country) \
  BE_ROW_UintSetting("GTWMAPREG", 0, 9, &user_selected_tesla_GTW_mapRegion) \
  BE_ROW_UintSetting("GTWCHASSIS", 0, 9, &user_selected_tesla_GTW_chassisType) \
  BE_ROW_UintSetting("GTWPACK", 0, 9, &user_selected_tesla_GTW_packEnergy) \
  BE_ROW_UintSetting("LEDMODE", 0, 10, &datalayer.battery.status.led_mode) \
  BE_IF_LILYGO2CAN(BE_ROW_UintSetting("GPIOOPT1", 0, 255, &user_selected_gpioopt1)) \
  BE_ROW_UintSetting("GPIOOPT2", 0, 255, &user_selected_gpioopt2) \
  BE_ROW_UintSetting("GPIOOPT3", 0, 255, &user_selected_gpioopt3) \
  BE_ROW_UintSetting("GPIOOPT4", 0, 255, &user_selected_gpioopt4) \
  BE_IF_STARK(BE_ROW_UintSetting("GPIOOPT5", 0, 255, &user_selected_gpioopt5)) \
  BE_IF_WAVESHARE(BE_ROW_UintSetting("GPIOOPT6", 0, 255, &user_selected_gpioopt6)) \
  BE_ROW_UintSetting("INVSUNTYPE", 0, 255, &user_selected_inverter_sungrow_type) \
  BE_ROW_UintSetting("CTVNOM", 0, 65535, &ct_clamp_nominal_voltage_dV) \
  BE_ROW_UintSetting("CTANOM", 0, 65535, &ct_clamp_nominal_current_A) \
  BE_ROW_UintSetting("CTATTEN", 0, (uint32_t)adc_attenuation_enum::Highest - 1, &ct_clamp_pin_atten) \
  BE_ROW_UintSetting("PYLONBAUD", 0, 1000000, &user_selected_pylon_baudrate) \
  BE_ROW_UintSetting("PYLONBRAND", 0, 255, &user_selected_inverter_pylon_type) \
  BE_ROW_UintSetting("DALYPWRPCT", 0, 10000, &user_selected_daly_power_per_percent) \
  BE_ROW_UintSetting("DALYPWRDV", 0, 10000, &user_selected_daly_power_per_dV) \
  BE_ROW_UintSetting("DALYDVSTART", 0, 255, &user_selected_daly_power_per_dV_start) \
  BE_ROW_UintSetting("DALYPWRDEG", 0, 10000, &user_selected_daly_power_per_degree_C) \
  BE_ROW_UintSetting("DALYPWR0C", 0, 100000, &user_selected_daly_power_at_0_degree_C) \
  BE_ROW_UintSetting("PYLONSEND", 0, 1, &user_selected_pylon_send) \
  BE_ROW_UintSetting("CHGTAPERSTART", 0, 100, &charge_taper_start_soc) \
  BE_ROW_UintSetting("CHGTAPERFLOOR", 0, 2000, &charge_taper_floor_W) \
  BE_ROW_UintSetting("PERBMSRESETH", 24, 48, &periodic_bms_reset_interval_h) \
  BE_ROW_UintSetting("FOXESSTYPE", 0, 255, &user_selected_inverter_foxess_type) \
  BE_ROW_UintSetting("FOXESSSUBTYPE", 0, 255, &user_selected_inverter_foxess_subtype) \
  BE_ROW_UintSetting("FOXESSMODULES", 0, 255, &user_selected_inverter_foxess_modules) \
  BE_ROW_UintSetting("SYSLOGPORT", 0, 65535, &syslog_port) \
  BE_ROW_UintSetting("SYSLOGFAC", 0, 23, &syslog_facility) \
  BE_ROW_IntSetting("CPUTEMPOFFSET", -100, 100, &datalayer.system.info.CPU_temperature_calibration_offset) \
  BE_ROW_BoolSetting("DBLBTR", &user_selected_second_battery) \
  BE_ROW_BoolSetting("CNTCTRL", &contactor_control_enabled) \
  BE_ROW_BoolSetting("CNTCTRLDBL", &contactor_control_enabled_double_battery) \
  BE_ROW_BoolSetting("PWMCNTCTRL", &pwm_contactor_control) \
  BE_ROW_BoolSetting("PERBMSRESET", &periodic_bms_reset) \
  BE_ROW_BoolSetting("REMBMSRESET", &remote_bms_reset) \
  BE_ROW_BoolSetting("EXTPRECHARGE", &precharge_control_enabled) \
  BE_ROW_BoolSetting("NOINVDISC", &precharge_inverter_normally_open_contactor) \
  BE_ROW_BoolSetting("WIFIAPENABLED", &wifiap_enabled) \
  BE_ROW_BoolSetting("STATICIP", &wifi_static_IP_enabled) \
  BE_ROW_BoolSetting("PERFPROFILE", &datalayer.system.info.performance_measurement_active) \
  BE_ROW_BoolSetting("CANLOGUSB", &datalayer.system.info.CAN_usb_logging_active) \
  BE_ROW_BoolSetting("USBENABLED", &datalayer.system.info.usb_logging_active) \
  BE_ROW_BoolSetting("WEBENABLED", &datalayer.system.info.web_logging_active) \
  BE_IF_SDCARD(BE_ROW_BoolSetting("CANLOGSD", &datalayer.system.info.CAN_SD_logging_active)) \
  BE_IF_SDCARD(BE_ROW_BoolSetting("SDLOGENABLED", &datalayer.system.info.SD_logging_active)) \
  BE_ROW_BoolSetting("MQTTENABLED", &mqtt_enabled) \
  BE_ROW_BoolSetting("MQTTCELLV", &mqtt_transmit_all_cellvoltages) \
  BE_ROW_BoolSetting("HADISC", &ha_autodiscovery_enabled) \
  BE_ROW_BoolSetting("DEYEBYD", &user_selected_inverter_deye_workaround) \
  BE_ROW_BoolSetting("INTERLOCKREQ", &user_selected_LEAF_interlock_mandatory) \
  BE_ROW_BoolSetting("DIGITALHVIL", &user_selected_tesla_digital_HVIL) \
  BE_ROW_BoolSetting("GTWRHD", &user_selected_tesla_GTW_rightHandDrive) \
  BE_ROW_BoolSetting("SOCESTIMATED", &user_selected_use_estimated_SOC) \
  BE_ROW_BoolSetting("PYLONOFFSET", &user_selected_pylon_30koffset) \
  BE_ROW_BoolSetting("PYLONORDER", &user_selected_pylon_invert_byteorder) \
  BE_ROW_BoolSetting("NCCONTACTOR", &contactor_control_inverted_logic) \
  BE_ROW_BoolSetting("TRIBTR", &user_selected_triple_battery) \
  BE_ROW_BoolSetting("CNTCTRLTRI", &contactor_control_enabled_triple_battery) \
  BE_ROW_BoolSetting("ESPNOWENABLED", &espnow_enabled) \
  BE_ROW_BoolSetting("PRIMOGEN24", &user_selected_primo_gen24) \
  BE_ROW_BoolSetting("USEVOLTLIMITS", &datalayer.battery.settings.user_set_voltage_limits_active) \
  BE_ROW_BoolSetting("LOWPASSFILTER", &inverter_low_pass_filter) \
  BE_ROW_BoolSetting("CTINVERT", &ct_invert_current) \
  BE_ROW_BoolSetting("WEBAUTH", &webserver_auth) \
  BE_ROW_BoolSetting("CHGTAPERSOC", &charge_taper_soc) \
  BE_ROW_BoolSetting("SLOWCANINV", &user_selected_inverter_long_CAN_timeout) \
  BE_ROW_BoolSetting("INVOFFGRID", &user_selected_inverter_offgrid) \
  BE_ROW_BoolSetting("PERBMSDEFSOC", &periodic_bms_reset_defer_low_soc) \
  BE_ROW_BoolSetting("PERBMSSKIPBAL", &periodic_bms_reset_skip_balancing) \
  BE_ROW_BoolSetting("MEASURECPUTEMP", &datalayer.system.info.CPU_measurement_enabled) \
  BE_ROW_BoolSetting("SYSLOGEN", &datalayer.system.info.syslog_logging_active) \
  BE_ROW_StringSetting("SSID", 32, &ssid) \
  BE_ROW_StringSetting("PASSWORD", 64, &password, SETTING_SECRET) \
  BE_ROW_StringSetting("APNAME", 64, &ssidAP) \
  BE_ROW_StringSetting("APPASSWORD", 64, &passwordAP, SETTING_SECRET) \
  BE_ROW_StringSetting("HOSTNAME", 64, &custom_hostname) \
  BE_ROW_StringSetting("MQTTSERVER", 64, &mqtt_server) \
  BE_ROW_StringSetting("MQTTUSER", 64, &mqtt_user) \
  BE_ROW_StringSetting("MQTTPASSWORD", 64, &mqtt_password, SETTING_SECRET) \
  BE_ROW_StringSetting("HTTPUSER", 32, &http_username) \
  BE_ROW_StringSetting("HTTPPASS", 64, &http_password, SETTING_SECRET) \
  BE_ROW_StringSetting("LOCALIP", 15, &edited_static_local_IP) \
  BE_ROW_StringSetting("GATEWAY", 15, &edited_static_gateway) \
  BE_ROW_StringSetting("SUBNET", 15, &edited_static_subnet) \
  BE_ROW_StringSetting("DNS", 15, &edited_static_dns) \
  BE_ROW_StringSetting("CTOFFSET", 16, &ct_clamp_offset_text) \
  BE_ROW_StringSetting("HADISCTOPIC", 64, &ha_autodiscovery_topic) \
  BE_ROW_StringSetting("SYSLOGIP", 15, &syslog_ip) \
  BE_ROW_ScaledSetting("BATTPVMAX", 0.0f, 1000.0f, 10.0f, &user_selected_max_pack_voltage_dV) \
  BE_ROW_ScaledSetting("BATTPVMIN", 0.0f, 1000.0f, 10.0f, &user_selected_min_pack_voltage_dV) \
  BE_ROW_InstantUintSetting("BATTERY_WH_MAX", 1, 400000, &datalayer.battery.info.total_capacity_Wh) \
  BE_ROW_InstantUintSetting("BMSRESETDUR", 0, 60000, &datalayer.battery.settings.user_set_bms_reset_duration_ms) \
  BE_ROW_InstantUintSetting("BYDAUTOCALDRIFT", 1, 20, &datalayer_extended.bydAtto3.auto_calibrate_soc_drift_percent) \
  BE_ROW_InstantUintSetting("BYDAUTOCALDRFT2", 1, 20, &datalayer_extended.bydAtto3_2.auto_calibrate_soc_drift_percent) \
  BE_ROW_InstantIntSetting("MINPERCENTAGE", -10, 50, &min_percentage_tenths) \
  BE_ROW_InstantIntSetting("MAXPERCENTAGE", 0, 200, &max_percentage_tenths) \
  BE_ROW_InstantScaledSetting("MAXCHARGEAMP", 0.0f, 100.0f, 10.0f, &datalayer.battery.settings.max_user_set_charge_dA) \
  BE_ROW_InstantScaledSetting("MAXDISCHARGEAMP", 0.0f, 100.0f, 10.0f, &datalayer.battery.settings.max_user_set_discharge_dA) \
  BE_ROW_InstantScaledSetting("TARGETCHVOLT", 0.0f, 1000.0f, 10.0f, &datalayer.battery.settings.max_user_set_charge_voltage_dV) \
  BE_ROW_InstantScaledSetting("TARGETDISCHVOLT", 0.0f, 1000.0f, 10.0f, &datalayer.battery.settings.max_user_set_discharge_voltage_dV) \
  BE_ROW_InstantBoolSetting("USE_SCALED_SOC", &datalayer.battery.settings.soc_scaling_active) \
  BE_ROW_InstantBoolSetting("BYDAUTOCALEN", &datalayer_extended.bydAtto3.auto_calibrate_soc_enabled) \
  BE_ROW_InstantBoolSetting("BYDAUTOCALEN2", &datalayer_extended.bydAtto3_2.auto_calibrate_soc_enabled) \
  BE_ROW_VolatileUintSetting("TMP_CALTARGETSOC", 0, 100, [](uint32_t value) { datalayer_extended.bydAtto3.calibrationTargetSOC = (uint16_t)value; }, []() { return (uint32_t)datalayer_extended.bydAtto3.calibrationTargetSOC; }) \
  BE_ROW_VolatileUintSetting("TMP_CALTARGETAH", 0, 1000, [](uint32_t value) { datalayer_extended.bydAtto3.calibrationTargetAH = (uint16_t)value; }, []() { return (uint32_t)datalayer_extended.bydAtto3.calibrationTargetAH; }) \
  BE_ROW_VolatileUintSetting("TMP_CALTARGETSOC2", 0, 100, [](uint32_t value) { datalayer_extended.bydAtto3_2.calibrationTargetSOC = (uint16_t)value; }, []() { return (uint32_t)datalayer_extended.bydAtto3_2.calibrationTargetSOC; }) \
  BE_ROW_VolatileUintSetting("TMP_CALTARGETAH2", 0, 1000, [](uint32_t value) { datalayer_extended.bydAtto3_2.calibrationTargetAH = (uint16_t)value; }, []() { return (uint32_t)datalayer_extended.bydAtto3_2.calibrationTargetAH; }) \
  BE_ROW_VolatileUintSetting("TMP_FAKEBATTERYV", 0, 1000, [](uint32_t value) { if (battery != nullptr) battery->set_fake_voltage((float)value); }, []() { return battery ? (uint32_t)battery->get_voltage() : 0; }) \
  BE_ROW_VolatileUintSetting("TMP_BALFLOATPOWER", 0, UINT32_MAX, [](uint32_t value) { datalayer.battery.settings.balancing_float_power_W = (uint16_t)value; }, []() { return (uint32_t)datalayer.battery.settings.balancing_float_power_W; }) \
  BE_ROW_VolatileUintSetting("TMP_BALMAXPACKV", 0, UINT32_MAX, [](uint32_t value) { datalayer.battery.settings.balancing_max_pack_voltage_dV = (uint16_t)value; }, []() { return (uint32_t)datalayer.battery.settings.balancing_max_pack_voltage_dV; }) \
  BE_ROW_VolatileUintSetting("TMP_BALMAXCELLV", 0, UINT32_MAX, [](uint32_t value) { datalayer.battery.settings.balancing_max_cell_voltage_mV = (uint16_t)value; }, []() { return (uint32_t)datalayer.battery.settings.balancing_max_cell_voltage_mV; }) \
  BE_ROW_VolatileUintSetting("TMP_BALMAXDEVCELLV", 0, UINT32_MAX, [](uint32_t value) { datalayer.battery.settings.balancing_max_deviation_cell_voltage_mV = (uint16_t)value; }, []() { return (uint32_t)datalayer.battery.settings.balancing_max_deviation_cell_voltage_mV; }) \
  BE_ROW_VolatileBoolSetting("TMP_RECOVERYMODE", [](bool value) { datalayer.battery.settings.user_requests_forced_charging_recovery_mode = value; }, []() { return datalayer.battery.settings.user_requests_forced_charging_recovery_mode; }) \
  BE_ROW_VolatileBoolSetting("TMP_BALANCE", [](bool value) { datalayer.battery.settings.user_requests_balancing = value; }, []() { return datalayer.battery.settings.user_requests_balancing; }) \
  BE_ROW_VolatileBoolSetting("TMP_CHARGERHVENABLED", [](bool value) { datalayer.charger.charger_HV_enabled = value; }, []() { return datalayer.charger.charger_HV_enabled; }) \
  BE_ROW_VolatileBoolSetting("TMP_CHARGERAUX12VENABLED", [](bool value) { datalayer.charger.charger_aux12V_enabled = value; }, []() { return datalayer.charger.charger_aux12V_enabled; }) \
  BE_ROW_VolatileBoolSetting("BYDKEEPISOOFF", [](bool value) { datalayer_extended.bydAtto3.keep_iso_disabled = value; datalayer_extended.bydAtto3_2.keep_iso_disabled = value; }, []() { return datalayer_extended.bydAtto3.keep_iso_disabled; }) \
  BE_ROW_VolatileFloatSetting("TMP_CHARGERSETPOINTV", 0.0f, 1000.0f, [](float value) { if (value >= CHARGER_MIN_HV && value <= CHARGER_MAX_HV) datalayer.charger.charger_setpoint_HV_VDC = (float)value; }, []() { return (float)datalayer.charger.charger_setpoint_HV_VDC; }) \
  BE_ROW_VolatileFloatSetting("TMP_CHARGERSETPOINTA", 0.0f, 100.0f, [](float value) { if ((value <= CHARGER_MAX_A) && (value <= datalayer.battery.settings.max_user_set_charge_dA) && (value * datalayer.charger.charger_setpoint_HV_VDC <= CHARGER_MAX_POWER)) datalayer.charger.charger_setpoint_HV_IDC = (float)value; }, []() { return (float)datalayer.charger.charger_setpoint_HV_IDC; }) \
  BE_ROW_VolatileFloatSetting("TMP_CHARGERENDA", 0.0f, 100.0f, [](float value) { datalayer.charger.charger_setpoint_HV_IDC_END = (float)value; }, []() { return (float)datalayer.charger.charger_setpoint_HV_IDC_END; }) \
  BE_ROW_VolatileScaledSetting("TMP_BALTIME", 0.0f, (float)UINT32_MAX / 60000.0f, 60000.0f, [](float value) { datalayer.battery.settings.balancing_max_time_ms = (uint32_t)value; }, []() { return (float)datalayer.battery.settings.balancing_max_time_ms; }) \

// clang-format on
