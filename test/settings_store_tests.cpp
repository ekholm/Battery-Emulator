#include <gtest/gtest.h>

#include "../Software/src/communication/nvm/comm_nvm.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/utils/events.h"

// Covers BatteryEmulatorSettingsStore, the wrapper every setting is read and
// written through.
//
// Its save methods all share a shape that is easy to get wrong: skip the write
// when the stored value already matches, but only after checking the key
// exists. Without the existence check, saving a value equal to the type's zero
// - false, 0, "" - into a key that has never been written would look like a
// no-op and never reach flash. Each of those cases is pinned below.
//
// The settingsUpdated flag drives whether a reboot is offered after a settings
// change, so a save that silently fails to set it is a change the user is never
// told to apply.

namespace {

constexpr const char* kNamespace = "batterySettings";

class SettingsStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    emul_nvs_reset();
    reset_all_events();
  }
};

}  // namespace

// --- Round-trip ------------------------------------------------------------

TEST_F(SettingsStoreTest, StoresAndReadsBackEachType) {
  BatteryEmulatorSettingsStore settings;

  settings.saveInt("INTKEY", -12345);
  settings.saveUInt("UINTKEY", 4000000000u);
  settings.saveBool("BOOLKEY", true);
  settings.saveString("STRKEY", "hello");

  EXPECT_EQ(settings.getInt("INTKEY", 0), -12345);
  EXPECT_EQ(settings.getUInt("UINTKEY", 0), 4000000000u);
  EXPECT_TRUE(settings.getBool("BOOLKEY", false));
  EXPECT_EQ(settings.getString("STRKEY", ""), String("hello"));
}

// Settings have to survive a power cycle, which is a fresh store object over
// the same storage.
TEST_F(SettingsStoreTest, ValuesSurviveReopeningTheStore) {
  {
    BatteryEmulatorSettingsStore settings;
    settings.saveInt("PERSIST", 99);
  }

  BatteryEmulatorSettingsStore reopened;
  EXPECT_EQ(reopened.getInt("PERSIST", 0), 99);
}

// --- Defaults --------------------------------------------------------------

TEST_F(SettingsStoreTest, MissingKeysReturnTheCallersDefault) {
  BatteryEmulatorSettingsStore settings;

  EXPECT_EQ(settings.getInt("ABSENT", 42), 42);
  EXPECT_EQ(settings.getUInt("ABSENT", 43u), 43u);
  EXPECT_TRUE(settings.getBool("ABSENT", true));
  EXPECT_EQ(settings.getString("ABSENT", "fallback"), String("fallback"));
  EXPECT_FALSE(settings.settingExists("ABSENT"));
}

// A stored value must win over the default even when it equals the type's zero,
// which is the case a naive "is it zero?" check would get wrong.
TEST_F(SettingsStoreTest, StoredZeroValuesWinOverNonZeroDefaults) {
  BatteryEmulatorSettingsStore settings;
  settings.saveInt("ZEROINT", 0);
  settings.saveBool("FALSEBOOL", false);
  settings.saveString("EMPTYSTR", "");

  EXPECT_EQ(settings.getInt("ZEROINT", 42), 0);
  EXPECT_FALSE(settings.getBool("FALSEBOOL", true));
  EXPECT_EQ(settings.getString("EMPTYSTR", "fallback"), String(""));
}

// --- The zero-value save cases ---------------------------------------------

// The regression these isKey() checks exist for: saving false into a key that
// has never been written must actually write it.
TEST_F(SettingsStoreTest, SavingFalseIntoAMissingKeyPersistsIt) {
  BatteryEmulatorSettingsStore settings;

  settings.saveBool("NEWFALSE", false);

  EXPECT_TRUE(settings.settingExists("NEWFALSE")) << "a first save of false must reach storage";
  EXPECT_TRUE(settings.were_settings_updated());
}

TEST_F(SettingsStoreTest, SavingZeroIntoAMissingKeyPersistsIt) {
  BatteryEmulatorSettingsStore settings;

  settings.saveInt("NEWZERO", 0);
  settings.saveUInt("NEWUZERO", 0u);

  EXPECT_TRUE(settings.settingExists("NEWZERO"));
  EXPECT_TRUE(settings.settingExists("NEWUZERO"));
  EXPECT_TRUE(settings.were_settings_updated()) << "a first save of zero is a change";
}

TEST_F(SettingsStoreTest, SavingAnEmptyStringIntoAMissingKeyPersistsIt) {
  BatteryEmulatorSettingsStore settings;

  settings.saveString("NEWEMPTY", "");

  EXPECT_TRUE(settings.settingExists("NEWEMPTY"));
  EXPECT_TRUE(settings.were_settings_updated()) << "a first save of an empty string is a change";
}

// --- The updated flag ------------------------------------------------------

TEST_F(SettingsStoreTest, UnchangedSavesDoNotMarkSettingsUpdated) {
  {
    BatteryEmulatorSettingsStore first;
    first.saveInt("SAME", 7);
    first.saveUInt("SAMEU", 9u);
    first.saveBool("SAMEB", true);
    first.saveString("SAMES", "x");
  }

  BatteryEmulatorSettingsStore settings;
  settings.saveInt("SAME", 7);
  settings.saveUInt("SAMEU", 9u);
  settings.saveBool("SAMEB", true);
  settings.saveString("SAMES", "x");

  EXPECT_FALSE(settings.were_settings_updated())
      << "rewriting identical values must not ask the user to reboot for nothing";
}

TEST_F(SettingsStoreTest, ChangedSavesMarkSettingsUpdated) {
  {
    BatteryEmulatorSettingsStore first;
    first.saveInt("CHANGES", 7);
  }

  BatteryEmulatorSettingsStore settings;
  EXPECT_FALSE(settings.were_settings_updated());
  settings.saveInt("CHANGES", 8);
  EXPECT_TRUE(settings.were_settings_updated());
}

TEST_F(SettingsStoreTest, AFreshStoreStartsNotUpdated) {
  BatteryEmulatorSettingsStore settings;

  EXPECT_FALSE(settings.were_settings_updated());
}

// --- Removal and clearing --------------------------------------------------

TEST_F(SettingsStoreTest, RemovingAKeyMakesItMissingAgain) {
  BatteryEmulatorSettingsStore settings;
  settings.saveInt("GOING", 5);
  ASSERT_TRUE(settings.settingExists("GOING"));

  settings.removeKey("GOING");

  EXPECT_FALSE(settings.settingExists("GOING"));
  EXPECT_EQ(settings.getInt("GOING", 77), 77) << "a removed key must fall back to the default";
  EXPECT_TRUE(settings.were_settings_updated());
}

// Removing something that was never there is not a change, so it must not ask
// the user to reboot.
TEST_F(SettingsStoreTest, RemovingAMissingKeyIsNotAnUpdate) {
  BatteryEmulatorSettingsStore settings;

  settings.removeKey("NEVEREXISTED");

  EXPECT_FALSE(settings.were_settings_updated());
}

TEST_F(SettingsStoreTest, ClearAllEmptiesTheNamespace) {
  {
    BatteryEmulatorSettingsStore settings;
    settings.saveInt("A", 1);
    settings.saveInt("B", 2);
    ASSERT_EQ(emul_nvs_key_count(kNamespace), 2u);
    settings.clearAll();
  }

  EXPECT_EQ(emul_nvs_key_count(kNamespace), 0u);
  BatteryEmulatorSettingsStore reopened;
  EXPECT_FALSE(reopened.settingExists("A"));
}

// --- Read-only stores ------------------------------------------------------

// The settings page opens a read-only store to render current values. It must
// not be able to write through it.
TEST_F(SettingsStoreTest, ReadOnlyStoreDoesNotPersistWrites) {
  {
    BatteryEmulatorSettingsStore writable;
    writable.saveInt("RO", 1);
  }

  {
    BatteryEmulatorSettingsStore read_only(true);
    read_only.saveInt("RO", 2);
  }

  BatteryEmulatorSettingsStore check;
  EXPECT_EQ(check.getInt("RO", 0), 1) << "a read-only store must not change stored settings";
}

// A store that cannot write must not claim anything changed either: the flag
// drives whether the user is told to reboot to apply a setting.
TEST_F(SettingsStoreTest, ReadOnlyStoreDoesNotReportSettingsUpdated) {
  {
    BatteryEmulatorSettingsStore writable;
    writable.saveInt("ROFLAG", 1);
  }

  BatteryEmulatorSettingsStore read_only(true);
  read_only.saveInt("ROFLAG", 2);
  read_only.saveBool("ROFLAGB", true);
  read_only.removeKey("ROFLAG");

  EXPECT_FALSE(read_only.were_settings_updated());
}

// --- The NVS key-length limit ----------------------------------------------

// NVS rejects keys longer than 15 characters. Every key in the firmware is
// currently within that (the longest, TARGETDISCHVOLT, is exactly 15), and a
// new one that exceeded it would fail only on hardware - the setting would
// simply never persist. The emulation models the limit so that shows up here.
TEST_F(SettingsStoreTest, KeysLongerThanTheNvsLimitDoNotPersist) {
  BatteryEmulatorSettingsStore settings;

  settings.saveInt("EXACTLY15CHARS_", 1);
  settings.saveInt("SIXTEEN_CHARS_XY", 2);

  EXPECT_TRUE(settings.settingExists("EXACTLY15CHARS_")) << "15 characters is the limit, not over it";
  EXPECT_FALSE(settings.settingExists("SIXTEEN_CHARS_XY")) << "a 16-character key is rejected by NVS";
}

// --- A full store ----------------------------------------------------------

// NVS is log-structured: an update appends a new entry and marks the old one erased, and
// reclaiming those erased entries needs a free page to compact into. Once none is free every
// save fails, and Preferences::putX() reports that as a 0 return. That return used to be
// discarded and settingsUpdated set anyway, so the firmware told the user the setting was
// stored and to reboot to apply it - and the reboot brought the old value back with nothing
// reported anywhere. The cases below pin each half of the fix: the flag stays false, and the
// user gets an event.

namespace {

bool event_is_active(EVENTS_ENUM_TYPE event) {
  const EVENTS_STRUCT_TYPE* entry = get_event_pointer(event);
  return entry->state == EVENT_STATE_ACTIVE || entry->state == EVENT_STATE_ACTIVE_LATCHED;
}

}  // namespace

TEST_F(SettingsStoreTest, AFailedSaveDoesNotReportSettingsUpdated) {
  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveInt("FULLI", 1);
  settings.saveUInt("FULLU", 1u);
  settings.saveBool("FULLB", true);
  settings.saveString("FULLS", "value");

  EXPECT_FALSE(settings.were_settings_updated())
      << "a save that never reached flash must not offer the user a reboot to apply it";
}

TEST_F(SettingsStoreTest, OverwritingAStoredEmptyStringOnAFullStoreIsReportedAsFailure) {
  {
    BatteryEmulatorSettingsStore first;
    first.saveString("WASEMPTY", "");
  }

  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveString("WASEMPTY", "a real value");

  EXPECT_EQ(settings.getString("WASEMPTY", "x"), String("")) << "the write did not reach flash";
  EXPECT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
  EXPECT_FALSE(settings.were_settings_updated())
      << "the read-back carve-out is for saving an empty string, not for a key that already holds one";
}

TEST_F(SettingsStoreTest, AFailedSaveRaisesTheSaveFailureEvent) {
  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveInt("FULLI", 1);

  EXPECT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
}

// The two failures are distinct and must stay distinct: SAVE_INFO means the store would not
// open at all, and a full store opens perfectly well. Overloading it would make an unreadable
// device and a device that cannot be written indistinguishable on the events page.
TEST_F(SettingsStoreTest, AFailedSaveDoesNotRaiseTheStoreWouldNotOpenEvent) {
  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveInt("FULLI", 1);

  EXPECT_FALSE(event_is_active(EVENT_PERSISTENT_SAVE_INFO));
}

// A failed save is a warning, not an error: the emulator keeps running the battery correctly,
// only the persistence failed. It still has to colour the status, which is what the silent
// version was missing.
/* The level is not only how loudly the user is told. EVENT_LEVEL_ERROR drives
 * datalayer.system.status.system_status to FAULT, and the contactor control
 * counts faulted ticks and requests SHUTDOWN after ten seconds of it - so
 * raising this event at error level would open the contactors and disconnect
 * the battery because a setting could not be written. Warning is the correct
 * level for that reason, not merely a judgement about volume, and this pins the
 * consequence rather than the label.
 */
TEST_F(SettingsStoreTest, AFailedSaveDoesNotFaultTheSystem) {
  init_events();
  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveInt("FULLI", 1);

  ASSERT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
  EXPECT_NE(datalayer.system.status.system_status, FAULT) << "a persistence failure must not shut the battery down";
}

TEST_F(SettingsStoreTest, TheSaveFailureEventIsAWarning) {
  init_events();
  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveInt("FULLI", 1);

  EXPECT_EQ(get_event_level(), EVENT_LEVEL_WARNING);
  EXPECT_EQ(get_emulator_status(), EMULATOR_STATUS::STATUS_WARNING);
}

// What the user actually experiences: the setting reverts. Pinned so the event above is
// understood to report a real loss and not a cosmetic warning.
TEST_F(SettingsStoreTest, AFailedSaveLeavesThePreviousValueInPlace) {
  {
    BatteryEmulatorSettingsStore settings;
    settings.saveInt("BATTTYPE", 7);
  }

  emul_nvs_set_full(true);
  {
    BatteryEmulatorSettingsStore settings;
    settings.saveInt("BATTTYPE", 9);
  }

  emul_nvs_set_full(false);
  BatteryEmulatorSettingsStore after_reboot;
  EXPECT_EQ(after_reboot.getInt("BATTTYPE", 0), 7) << "the change was lost, which is the point of the event";
}

// The negative control for every case above: on a healthy store nothing must be reported.
TEST_F(SettingsStoreTest, SuccessfulSavesRaiseNoFailureEvent) {
  BatteryEmulatorSettingsStore settings;

  settings.saveInt("OKI", 1);
  settings.saveUInt("OKU", 1u);
  settings.saveBool("OKB", true);
  settings.saveString("OKS", "value");

  EXPECT_TRUE(settings.were_settings_updated());
  EXPECT_FALSE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
}

// --- The empty string, which putString() reports as 0 bytes ----------------

// putString() returns the number of bytes written, so a successful save of "" returns the same
// 0 that every put returns on failure. Clearing a WiFi password or an MQTT topic is exactly
// that write, so a bare `!= 0` test would report every such save as lost.
TEST_F(SettingsStoreTest, SavingAnEmptyStringIsNotMistakenForAFailure) {
  BatteryEmulatorSettingsStore settings;

  settings.saveString("WIFIPASS", "");

  EXPECT_TRUE(settings.were_settings_updated());
  EXPECT_FALSE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
  EXPECT_TRUE(settings.settingExists("WIFIPASS")) << "the empty string is stored, not absent";
}

TEST_F(SettingsStoreTest, AnEmptyStringThatCannotBeWrittenIsStillReportedAsFailed) {
  BatteryEmulatorSettingsStore settings;
  emul_nvs_set_full(true);

  settings.saveString("WIFIPASS", "");

  EXPECT_FALSE(settings.were_settings_updated());
  EXPECT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
}

// The read-back has to distinguish "" from the old value too, not just from an absent key.
TEST_F(SettingsStoreTest, AnEmptyStringOverwriteThatFailsLeavesTheOldValueAndReportsFailure) {
  BatteryEmulatorSettingsStore settings;
  settings.saveString("WIFIPASS", "secret");

  emul_nvs_set_full(true);
  settings.saveString("WIFIPASS", "");

  EXPECT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
  EXPECT_EQ(settings.getString("WIFIPASS", ""), String("secret"));
}

// --- Erases -----------------------------------------------------------------

// remove() and clear() report failure the same way and were discarded the same way. Nothing
// makes them fail on a healthy partition, which is precisely why the path needs a test.
TEST_F(SettingsStoreTest, AFailedRemoveIsReportedAndDoesNotClaimAnUpdate) {
  BatteryEmulatorSettingsStore settings;
  settings.saveInt("GONE", 1);

  emul_nvs_set_erase_fails(true);
  BatteryEmulatorSettingsStore remover;
  remover.removeKey("GONE");

  EXPECT_FALSE(remover.were_settings_updated());
  EXPECT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
}

TEST_F(SettingsStoreTest, AFailedClearAllIsReportedAndDoesNotClaimAnUpdate) {
  BatteryEmulatorSettingsStore settings;
  settings.saveInt("KEPT", 1);

  emul_nvs_set_erase_fails(true);
  BatteryEmulatorSettingsStore clearer;
  clearer.clearAll();

  EXPECT_FALSE(clearer.were_settings_updated());
  EXPECT_TRUE(event_is_active(EVENT_PERSISTENT_SAVE_FAILURE));
  EXPECT_EQ(emul_nvs_key_count(kNamespace), 1u) << "nothing was erased, so the factory reset did not happen";
}
