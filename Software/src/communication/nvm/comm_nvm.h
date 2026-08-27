#ifndef _COMM_NVM_H_
#define _COMM_NVM_H_

#include <Preferences.h>
#include <WString.h>
#include <limits>
#include "../../datalayer/datalayer.h"
#include "../../devboard/utils/events.h"
#include "../../devboard/utils/flash_write_broker.h"
#include "../../devboard/utils/logging.h"
#include "../../devboard/wifi/wifi.h"

/**
 * @brief Initialization of setting storage
 *
 * @param[in] void
 *
 * @return void
 */
void init_stored_settings();

/**
 * @brief Store settings of equipment stop button
 *
 * @param[in] void
 *
 * @return void
 */
void store_settings_equipment_stop();

/**
 * @brief Persist a WatchDogTimeout the inverter changed, if one is pending
 *
 * @param[in] void
 *
 * @return void
 */
void store_settings_inverter_watchdog();

void erase_phy_cal_data();

/**
 * @brief Store settings
 *
 * @param[in] void
 *
 * @return void
 */
void store_settings();

void clear_wifi_sta_settings();

// Wraps the Preferences object begin/end calls, so that the scope of this object
// runs them automatically (via constructor/destructor).
//
// Every write goes through flash_write_broker(): one NVS key is the
// shortest flash operation this API can issue, so a save of N changed keys
// becomes N short cache-off windows with a CAN drain between them, instead of
// one stall as long as the whole save. The reads are not brokered - they run
// with the cache on and stall nothing.
//
// Batching the keys into one commit would NOT help: Preferences::putX calls
// nvs_commit() per key, and nvs_commit() is a no-op in IDF ("no-op for now, to
// be used when intermediate cache is added", nvs_api.cpp) - the entry is
// already in flash when nvs_set_* returns. The write count is the key count,
// and the interleave is what shortens the windows.
class BatteryEmulatorSettingsStore {
 public:
  BatteryEmulatorSettingsStore(bool readOnly = false) {
    if (!settings.begin("batterySettings", readOnly)) {
      set_event(EVENT_PERSISTENT_SAVE_INFO, 0);
    }
  }

  ~BatteryEmulatorSettingsStore() { settings.end(); }

  void clearAll() {
    // The one call here that erases whole pages rather than programming an
    // entry, so the longest window a settings operation can produce.
    flash_write_broker().run([&]() { settings.clear(); });
    settingsUpdated = true;
  }

  int32_t getInt(const char* name, int32_t defaultValue) {
    return settings.isKey(name) ? settings.getInt(name, defaultValue) : defaultValue;
  }

  void saveInt(const char* name, int32_t value) {
    // isKey() check instead of a sentinel default: saving a value equal to the
    // sentinel into a missing key must not be skipped.
    if (!settings.isKey(name) || getInt(name, 0) != value) {
      flash_write_broker().run([&]() { settings.putInt(name, value); });
      settingsUpdated = true;
    }
  }

  uint32_t getUInt(const char* name, uint32_t defaultValue) {
    return settings.isKey(name) ? settings.getUInt(name, defaultValue) : defaultValue;
  }

  void saveUInt(const char* name, uint32_t value) {
    // isKey() check instead of a sentinel default: saving a value equal to the
    // sentinel into a missing key must not be skipped.
    if (!settings.isKey(name) || getUInt(name, 0) != value) {
      flash_write_broker().run([&]() { settings.putUInt(name, value); });
      settingsUpdated = true;
    }
  }

  bool settingExists(const char* name) { return settings.isKey(name); }

  void removeKey(const char* name) {
    if (settings.isKey(name)) {
      flash_write_broker().run([&]() { settings.remove(name); });
      settingsUpdated = true;
    }
  }

  bool getBool(const char* name, bool defaultValue = false) {
    return settings.isKey(name) ? settings.getBool(name, defaultValue) : defaultValue;
  }

  void saveBool(const char* name, bool value) {
    // isKey() check: a stored 'false' must not be mistaken for a missing key,
    // or the first save of a false value would be skipped and never persisted.
    if (!settings.isKey(name) || getBool(name, false) != value) {
      flash_write_broker().run([&]() { settings.putBool(name, value); });
      settingsUpdated = true;
    }
  }

  String getString(const char* name) { return getString(name, ""); }

  String getString(const char* name, const char* defaultValue) {
    return settings.isKey(name) ? settings.getString(name, defaultValue) : String(defaultValue);
  }

  void saveString(const char* name, const char* value) {
    // isKey() check: a stored empty string must not be mistaken for a missing
    // key, or the first save of an empty value would be skipped.
    if (!settings.isKey(name) || getString(name, "") != String(value)) {
      flash_write_broker().run([&]() { settings.putString(name, value); });
      settingsUpdated = true;
    }
  }

  // Parses an IP string; returns 0.0.0.0 when missing/malformed (fromString leaves partial bytes on failure).
  IPAddress getIP(const char* name) {
    IPAddress ip;
    if (!ip.fromString(getString(name).c_str())) {
      ip = IPAddress();
    }
    return ip;
  }

  bool were_settings_updated() const { return settingsUpdated; }

 private:
  Preferences settings;

  // To track if settings were updated
  bool settingsUpdated = false;
};

#endif
