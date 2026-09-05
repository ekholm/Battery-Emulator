#include "ota_rollback.h"

#include <esp_app_format.h>
#include <esp_ota_ops.h>

#include "events.h"
#include "logging.h"
#include "ota_confirm_gate.h"

/* Take the confirmation away from initArduino() and do it ourselves.
 *
 * Rollback is already enabled in this build (CONFIG_APP_ROLLBACK_ENABLE comes
 * from the framework's own config, not from anything here), and the bootloader
 * already runs a freshly updated image as PENDING_VERIFY. What closes the
 * window is arduino-esp32: initArduino() calls
 * esp_ota_mark_app_valid_cancel_rollback() BEFORE setup() begins, so an update
 * is confirmed before a single line of this firmware has run. Every crash the
 * field reports - stored settings meeting new code, a driver that dies on
 * init - happens after that point, with the safety net already folded away.
 *
 * This weak hook is the framework's own way out: returning true defers the
 * decision to the application. It means the responsibility is now entirely
 * ours - if mark_ota_image_valid() is not reached, the next reset undoes the
 * update - which is why every steady state this firmware can be in reaches the
 * gate in ota_confirm_gate.h, and why failing to confirm is logged rather than
 * passed over.
 */
extern "C" bool verifyRollbackLater(void) {
  return true;
}

void report_ota_rollback(void) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  /* The image the bootloader gave up on is the OTHER slot, and its otadata
   * state is the record of what happened: ABORTED when it reset before
   * confirming itself, INVALID when it (or a previous boot of it) was rejected
   * outright. Both mean the same thing to a user - that image is not what is
   * running, and this older firmware is.
   *
   * WHY THE LINE NO LONGER SAYS "failed to start". ABORTED means the image was
   * PENDING_VERIFY when a reset went past it, and two different things produce
   * that. One is the case this feature exists for: the update crashed, or hung
   * until the watchdog fired, before it earned its 42 s. The other is a
   * deliberate in-window revert - /revertFirmware reboots, and the bootloader
   * rewrites every PENDING_VERIFY entry to ABORTED before it selects anything,
   * so an image the user chose to leave is failed by definition. otadata records
   * the state, not the reason, so at this point the two are indistinguishable
   * and a line that names one of them is wrong half the time.
   *
   * They could be told apart by having /revertFirmware leave a marker behind -
   * RTC_NOINIT memory survives the software reset it performs - and that was
   * considered and not done: it buys one adjective at the cost of a second
   * persistent state to keep in step, and the honest wording costs nothing. The
   * OTA path itself is no longer a source of false reports at all, because the
   * running image is now confirmed at onOTAStart() before the selection moves.
   *
   * Reported on EVERY boot, not once. The device really is running behind, and
   * the state stays until a later update succeeds, so a line that appeared once
   * and then vanished would be the less honest of the two. */
  const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
  if (other == NULL) {
    return;
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(other, &state) != ESP_OK) {
    return;
  }
  if (state != ESP_OTA_IMG_ABORTED && state != ESP_OTA_IMG_INVALID) {
    return;
  }

  /* Naming the version that was dropped is the difference between "something
   * happened once" and a user knowing which release to look at. It is read from
   * that image's own header, so it is right even when this happened several
   * boots ago. Kept inside the logger's line budget; see
   * EveryOtaLogLineFitsTheLoggersOwnBuffer. */
  esp_app_desc_t dropped;
  if (esp_ota_get_partition_description(other, &dropped) == ESP_OK) {
    LOG_SET_NEXT_SEVERITY(4);  // warning
    logging.printf("Firmware %s did not confirm itself; running %s instead\n", dropped.version,
                   esp_app_get_description()->version);
  } else {
    LOG_SET_NEXT_SEVERITY(4);  // warning
    logging.println("A firmware update did not confirm itself; this older version is running instead");
  }
  set_event(EVENT_OTA_ROLLBACK, 0);
#endif  // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
}

void ota_confirm_service(void) {
  /* Everything expensive is behind the flag, so this costs one read on the
     millions of passes where nothing is owed. */
  if (ota_confirm_take_pending()) {
    mark_ota_image_valid();
  }
}

void mark_ota_image_valid(void) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  /* Only one boot is ever pending, so this is a no-op on every ordinary start;
   * checking the state first keeps the log line meaningful instead of claiming
   * an update was confirmed on a board that has not been updated. */
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;
  }
  /* Where the write would LAND, read from otadata now rather than assumed: the
   * boot selection is what /revertFirmware and a finished OTA move, and it is
   * what esp_ota_mark_app_valid_cancel_rollback() writes to (the reasoning is
   * on ota_confirm_verdict). Compared by address - both pointers come from the
   * partition table's own list, but the address is the identity. */
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const bool still_selected = boot != NULL && boot->address == running->address;
  switch (ota_confirm_verdict(state == ESP_OTA_IMG_PENDING_VERIFY, still_selected)) {
    case OtaConfirmVerdict::NOT_PENDING:
      return;
    case OtaConfirmVerdict::BOOT_SELECTION_MOVED:
      /* Not an error and not a confirmation: the image about to boot earns its
       * own window, and this one is being left behind unconfirmed, which the
       * bootloader records as ABORTED on the way past. Said in the log because
       * the arrival will otherwise read as a rollback that nobody asked for. */
      LOG_SET_NEXT_SEVERITY(5);  // notice
      /* Kept short on purpose: Logging::printf renders into a MAX_LINE_LENGTH_PRINTF
       * buffer and overwrites the last character with a newline when the line
       * would be longer, so an over-long line loses its tail and says nothing
       * about it. Pinned by EveryOtaLogLineFitsTheLoggersOwnBuffer. */
      logging.printf("Firmware %s unconfirmed: boot selection moved, the next image runs its own %lu s window\n",
                     esp_app_get_description()->version, (unsigned long)(OTA_CONFIRM_UPTIME_MS / 1000));
      return;
    case OtaConfirmVerdict::CONFIRM:
      break;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    LOG_SET_NEXT_SEVERITY(5);  // notice
    logging.printf("Firmware %s started cleanly and is now confirmed; rollback window closed\n",
                   esp_app_get_description()->version);
  } else {
    /* Nothing to do about it here, but say so: the image stays pending, so the
     * next reset - however ordinary - will roll it back, and that is otherwise
     * a very confusing thing to have happen. */
    LOG_SET_NEXT_SEVERITY(4);  // warning
    logging.println("Could not confirm this firmware; the next restart will roll it back");
  }
#endif  // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
}
