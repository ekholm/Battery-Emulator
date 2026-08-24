#include "settings.h"
#include "settings_rows.h"
#include "settings_handlers.h"
#include "webserver_new.h"

#include <cmath>
#include <cstdlib>

#include "../../battery/BATTERIES.h"
#include "../../battery/Battery.h"
#include "../../charger/CHARGERS.h"
#include "../../charger/CanCharger.h"
#include "../../communication/can/comm_can.h"
#include "../../communication/contactorcontrol/comm_contactorcontrol.h"
#include "../../communication/equipmentstopbutton/comm_equipmentstopbutton.h"
#include "../../communication/nvm/comm_nvm.h"
#include "../../communication/precharge_control/precharge_control.h"
#include "../../datalayer/datalayer.h"
#include "../../datalayer/datalayer_extended.h"
#include "../../devboard/mqtt/mqtt.h"
#include "../../devboard/network/hostname.h"  // custom_hostname
#include "../../devboard/utils/logging.h"
#include "../../devboard/utils/types.h"
#include "../../devboard/wifi/wifi.h"
#include "../../inverter/INVERTERS.h"
#include "../../inverter/InverterProtocol.h"
#include "../../lib/bblanchon-ArduinoJson/ArduinoJson.h"
#include "../../shunt/Shunt.h"

// Static IP settings are stored and edited as dotted-quad strings, but loaded
// into an IPAddress on boot, and can't be edited at runtime. We don't actually
// need to store these, but StringSetting can't handle a nullptr storage
// argument.
static std::string edited_static_local_IP, edited_static_gateway, edited_static_subnet, edited_static_dns;

// Stored as tenths of a percent, rather than hundredths like in datalayer.
static int32_t min_percentage_tenths, max_percentage_tenths;

// ---------------------------------------------------------------------------
// Settings tables. Each setting appears exactly once, in the table whose
// persistence/type semantics it needs. These are loaded in comm_nvm.cpp and
// saved in the GET/POST handlers and store_settings().
// ---------------------------------------------------------------------------

// Settings are distinguished into three classes:
//
// PERSISTED settings are persisted to NVM and require a reboot to take effect.
// They are loaded at boot by init_stored_settings().
//
// INSTANT settings are persisted to NVM but also applied to the live state
// immediately. They are loaded at boot by init_stored_settings().
//
// VOLATILE settings are not persisted to NVM, they are only applied to the live
// state.

// clang-format off

// Unsigned ints and enums, which are persisted to flash, and require a reboot
// to take effect.
#define BE_ROW_UintSetting(...) UintSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const PersistedUint PERSISTED_UINTS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Signed numeric settings. These are persisted to flash, and require a reboot to take effect.
#define BE_ROW_IntSetting(...) IntSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const PersistedInt PERSISTED_INTS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Boolean settings. These are persisted to flash, and require a reboot to take effect.
#define BE_ROW_BoolSetting(...) BoolSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const PersistedBool PERSISTED_BOOLS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// String settings. These are persisted to flash, and require a reboot to take effect.
#define BE_ROW_StringSetting(...) StringSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const PersistedString PERSISTED_STRINGS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Scaled fixed-point settings. These are persisted to flash, and require a reboot to take effect.
// Edited/validated as float, stored scaled (uint16_t).
#define BE_ROW_ScaledSetting(...) ScaledSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const PersistedScaled PERSISTED_SCALEDS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Instant unsigned integer settings. These are persisted to flash, but take
// effect immediately without a reboot.
#define BE_ROW_InstantUintSetting(...) InstantUintSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const InstantUint INSTANT_UINTS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

#define BE_ROW_InstantIntSetting(...) InstantIntSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const InstantInt INSTANT_INTS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Instant fixed-point settings. These are persisted to flash, but take effect
// immediately without a reboot. 
#define BE_ROW_InstantScaledSetting(...) InstantScaledSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const InstantScaled INSTANT_SCALEDS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Instant boolean settings. These are persisted to flash, but take effect
// immediately without a reboot.
#define BE_ROW_InstantBoolSetting(...) InstantBoolSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const InstantBool INSTANT_BOOLS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Volatile unsigned integer settings. These are not persisted to flash, and
// take effect immediately without a reboot.
#define BE_ROW_VolatileUintSetting(...) VolatileUintSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const VolatileUint VOLATILE_UINTS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Volatile boolean settings. These are not persisted to flash, and take effect
// immediately without a reboot.
#define BE_ROW_VolatileBoolSetting(...) VolatileBoolSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const VolatileBool VOLATILE_BOOLS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Volatile float settings. These are not persisted to flash, and take effect
// immediately without a reboot.
#define BE_ROW_VolatileFloatSetting(...) VolatileFloatSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const VolatileFloat VOLATILE_FLOATS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// Volatile scaled fixed-point settings. These are not persisted to flash, and
// take effect immediately without a reboot.
#define BE_ROW_VolatileScaledSetting(...) VolatileScaledSetting(__VA_ARGS__),
#include "settings_rows_null.h"
const VolatileScaled VOLATILE_SCALEDS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

// clang-format on

// ---------------------------------------------------------------------------

void populate_editable_settings() {
  // Some instant variables are stored differently in the NVM than they are in
  // the working variables. We can't directly use the settings framework for
  // these, since they need processing before editing (to convert them to their
  // editable representation) and after editing (to convert them back to their
  // working representation).

  // We use parallel 'editable' variables for these settings, which we populate
  // from the live variables just before editing, and then apply back to the
  // live variables afterwards.

  max_percentage_tenths = (int32_t)datalayer.battery.settings.max_percentage / 10;
  min_percentage_tenths = (int32_t)datalayer.battery.settings.min_percentage / 10;
}

void apply_editable_settings() {
  datalayer.battery.settings.max_percentage = (uint16_t)max_percentage_tenths * 10;
  datalayer.battery.settings.min_percentage = (int16_t)min_percentage_tenths * 10;
}

// ---------------------------------------------------------------------------
// Loads stored settings from NVM at boot.
// ---------------------------------------------------------------------------

void load_stored_settings(BatteryEmulatorSettingsStore& settings) {
  // Ensure the default values are populated
  populate_editable_settings();

  // For each persisted setting, load the value from NVM and apply it to the
  // live value. The current live value is passed as the default, so if there is
  // no stored value, the live value is unchanged.
  load_all_settings(PERSISTED_UINTS, settings);
  load_all_settings(PERSISTED_INTS, settings);
  load_all_settings(PERSISTED_BOOLS, settings);
  load_all_settings(PERSISTED_STRINGS, settings);
  load_all_settings(PERSISTED_SCALEDS, settings);
  load_all_settings(INSTANT_UINTS, settings);
  load_all_settings(INSTANT_INTS, settings);
  load_all_settings(INSTANT_SCALEDS, settings);
  load_all_settings(INSTANT_BOOLS, settings);

  apply_editable_settings();
}

void store_settings_from_live(BatteryEmulatorSettingsStore& settings) {
  populate_editable_settings();

  store_all_settings(INSTANT_UINTS, settings);
  store_all_settings(INSTANT_INTS, settings);
  store_all_settings(INSTANT_SCALEDS, settings);
  store_all_settings(INSTANT_BOOLS, settings);
}

// ---------------------------------------------------------------------------
// Settings (GET/POST to /api/internal/settings)
// ---------------------------------------------------------------------------

// Validate or apply the given settings:
// - `doc` contains the settings to validate or apply.
// - `errors` is a JSON object to which any validation errors will be added.
// - `settings` is the settings store to which persisted settings will be saved.
// - `save` is `false` for validation only, `true` to save the settings.
// - `reboot_required_saved` is set to true if any persisted setting was changed.
void apply_setting_updates(const JsonDocument& doc, JsonDocument& errors, BatteryEmulatorSettingsStore& settings,
                           bool save, bool& reboot_required_saved) {
  populate_editable_settings();

  process_all_settings(PERSISTED_UINTS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(PERSISTED_INTS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(PERSISTED_BOOLS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(PERSISTED_STRINGS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(PERSISTED_SCALEDS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(INSTANT_UINTS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(INSTANT_INTS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(INSTANT_SCALEDS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(INSTANT_BOOLS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(VOLATILE_UINTS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(VOLATILE_BOOLS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(VOLATILE_FLOATS, doc, errors, settings, save, reboot_required_saved);
  process_all_settings(VOLATILE_SCALEDS, doc, errors, settings, save, reboot_required_saved);

  apply_editable_settings();
}

void build_settings_json(JsonDocument& doc, BatteryEmulatorSettingsStore& settings) {
  JsonArray bats = doc["batteries"].to<JsonArray>();
  for (int i = 0; i < (int)BatteryType::Highest; i++) {
    bats[i] = name_for_battery_type((BatteryType)i);
  }
  JsonArray invs = doc["inverters"].to<JsonArray>();
  for (int i = 0; i < (int)InverterProtocolType::Highest; i++) {
    invs[i] = name_for_inverter_type((InverterProtocolType)i);
  }

  JsonObject sets = doc["settings"].to<JsonObject>();

  populate_editable_settings();

  // PERSISTED_* and INSTANT_* show the stored value when present (so a change
  // that still awaits its reboot is visible), else the live storage; VOLATILE_*
  // use read hooks over live state.
  emit_all_settings(PERSISTED_UINTS, sets, settings);
  emit_all_settings(PERSISTED_INTS, sets, settings);
  emit_all_settings(PERSISTED_BOOLS, sets, settings);
  emit_all_settings(PERSISTED_STRINGS, sets, settings);
  emit_all_settings(PERSISTED_SCALEDS, sets, settings);
  emit_all_settings(INSTANT_UINTS, sets, settings);
  emit_all_settings(INSTANT_INTS, sets, settings);
  emit_all_settings(INSTANT_SCALEDS, sets, settings);
  emit_all_settings(INSTANT_BOOLS, sets, settings);
  emit_all_settings(VOLATILE_UINTS, sets, settings);
  emit_all_settings(VOLATILE_BOOLS, sets, settings);
  emit_all_settings(VOLATILE_FLOATS, sets, settings);
  emit_all_settings(VOLATILE_SCALEDS, sets, settings);

  doc["reboot_required"] = settingsUpdated;
}

// ---------------------------------------------------------------------------
// Second consumer of BE_SETTINGS_LIST: the NVS-key rules, checked at compile
// time. Persisted/instant rows become NVS keys, so each must be unique and at
// most 15 characters (the NVS key-length limit the tables' comments cite).
// Volatile rows never reach NVS and are exempt (their TMP_* names may be
// longer). A violation fails the build here, before it can strand a value.
// ---------------------------------------------------------------------------

namespace {

#define BE_ROW_UintSetting(name, ...) name,
#define BE_ROW_IntSetting(name, ...) name,
#define BE_ROW_BoolSetting(name, ...) name,
#define BE_ROW_StringSetting(name, ...) name,
#define BE_ROW_ScaledSetting(name, ...) name,
#define BE_ROW_InstantUintSetting(name, ...) name,
#define BE_ROW_InstantIntSetting(name, ...) name,
#define BE_ROW_InstantScaledSetting(name, ...) name,
#define BE_ROW_InstantBoolSetting(name, ...) name,
#include "settings_rows_null.h"
constexpr const char* BE_PERSISTED_KEYS[] = {
  BE_SETTINGS_LIST
};
#include "settings_rows_undef.h"

constexpr size_t BE_PERSISTED_KEY_COUNT = sizeof(BE_PERSISTED_KEYS) / sizeof(BE_PERSISTED_KEYS[0]);

constexpr size_t be_key_len(const char* s) {
  size_t n = 0;
  while (s[n] != 0) {
    n++;
  }
  return n;
}

constexpr bool be_keys_equal(const char* a, const char* b) {
  size_t i = 0;
  while (a[i] != 0 && a[i] == b[i]) {
    i++;
  }
  return a[i] == b[i];
}

constexpr bool be_keys_within_nvs_limit() {
  for (size_t i = 0; i < BE_PERSISTED_KEY_COUNT; i++) {
    if (be_key_len(BE_PERSISTED_KEYS[i]) > 15) {
      return false;
    }
  }
  return true;
}

constexpr bool be_keys_unique() {
  for (size_t i = 0; i < BE_PERSISTED_KEY_COUNT; i++) {
    for (size_t j = i + 1; j < BE_PERSISTED_KEY_COUNT; j++) {
      if (be_keys_equal(BE_PERSISTED_KEYS[i], BE_PERSISTED_KEYS[j])) {
        return false;
      }
    }
  }
  return true;
}

static_assert(be_keys_within_nvs_limit(), "a persisted settings key exceeds the 15-char NVS limit");
static_assert(be_keys_unique(), "two settings rows declare the same NVS key");

}  // namespace
