#ifndef _COMM_NVM_H_
#define _COMM_NVM_H_

#include <Preferences.h>
#include <WString.h>
#include <limits>
#include "../../datalayer/datalayer.h"
#include "../../devboard/utils/events.h"
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
class BatteryEmulatorSettingsStore {
 public:
  BatteryEmulatorSettingsStore(bool readOnly = false) : readOnly(readOnly) {
    if (!settings.begin("batterySettings", readOnly)) {
      set_event(EVENT_PERSISTENT_SAVE_INFO, 0);
    }
  }

  ~BatteryEmulatorSettingsStore() { settings.end(); }

  void clearAll() {
    if (readOnly) {
      return;
    }
    noteWrite(settings.clear());
  }

  int32_t getInt(const char* name, int32_t defaultValue) {
    return settings.isKey(name) ? settings.getInt(name, defaultValue) : defaultValue;
  }

  void saveInt(const char* name, int32_t value) {
    if (readOnly) {
      return;
    }
    // isKey() check instead of a sentinel default: saving a value equal to the
    // sentinel into a missing key must not be skipped.
    if (!settings.isKey(name) || getInt(name, 0) != value) {
      noteWrite(settings.putInt(name, value) != 0);
    }
  }

  uint32_t getUInt(const char* name, uint32_t defaultValue) {
    return settings.isKey(name) ? settings.getUInt(name, defaultValue) : defaultValue;
  }

  void saveUInt(const char* name, uint32_t value) {
    if (readOnly) {
      return;
    }
    // isKey() check instead of a sentinel default: saving a value equal to the
    // sentinel into a missing key must not be skipped.
    if (!settings.isKey(name) || getUInt(name, 0) != value) {
      noteWrite(settings.putUInt(name, value) != 0);
    }
  }

  bool settingExists(const char* name) { return settings.isKey(name); }

  void removeKey(const char* name) {
    if (readOnly) {
      return;
    }
    if (settings.isKey(name)) {
      noteWrite(settings.remove(name));
    }
  }

  bool getBool(const char* name, bool defaultValue = false) {
    return settings.isKey(name) ? settings.getBool(name, defaultValue) : defaultValue;
  }

  void saveBool(const char* name, bool value) {
    if (readOnly) {
      return;
    }
    // isKey() check: a stored 'false' must not be mistaken for a missing key,
    // or the first save of a false value would be skipped and never persisted.
    if (!settings.isKey(name) || getBool(name, false) != value) {
      noteWrite(settings.putBool(name, value) != 0);
    }
  }

  String getString(const char* name) { return getString(name, ""); }

  String getString(const char* name, const char* defaultValue) {
    return settings.isKey(name) ? settings.getString(name, defaultValue) : String(defaultValue);
  }

  void saveString(const char* name, const char* value) {
    if (readOnly) {
      return;
    }
    // isKey() check: a stored empty string must not be mistaken for a missing
    // key, or the first save of an empty value would be skipped.
    if (!settings.isKey(name) || getString(name, "") != String(value)) {
      // putString() returns the number of bytes written, so the empty string reports 0 on
      // success - the same value every put reports on failure. That one case is settled by
      // reading the key back; every other length is conclusive on its own. The read-back is
      // gated on what is being WRITTEN, not on what the key holds: consulting it for a
      // non-empty value would report success for a failed write whenever the key already
      // held an empty string, which is the same silent loss this whole class is here to end.
      const bool writing_empty_string = String(value).length() == 0;
      noteWrite(settings.putString(name, value) != 0 || (writing_empty_string && storesEmptyString(name)));
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
  /* Records the outcome of one write to the store.
   *
   * Preferences::putX() returns 0 and Preferences::remove()/clear() return false when the
   * underlying NVS call fails, and a full partition is the ordinary way that happens: NVS is
   * log-structured, so an update appends a new entry and marks the old one erased, and
   * reclaiming the erased entries needs a free page to compact into. Once there is none, every
   * save fails. That return used to be discarded and settingsUpdated set regardless, so the
   * firmware told the user the setting was stored and to reboot to apply it - and the reboot
   * brought back the old value with nothing reported anywhere.
   *
   * EVENT_PERSISTENT_SAVE_INFO does not cover this: it is raised where settings.begin() fails,
   * and a full store opens perfectly well.
   */
  void noteWrite(bool succeeded) {
    if (succeeded) {
      settingsUpdated = true;
    } else {
      set_event(EVENT_PERSISTENT_SAVE_FAILURE, 0);
    }
  }

  // True when the key is present AND holds an empty string, which distinguishes a stored ""
  // from a key that was never written - the two cases putString() reports identically.
  bool storesEmptyString(const char* name) {
    return settings.isKey(name) && settings.getString(name, "").length() == 0;
  }

  Preferences settings;
  const bool readOnly;

  // To track if settings were updated
  bool settingsUpdated = false;
};

#endif
