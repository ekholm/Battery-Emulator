#include "ota_revert.h"

#include <cctype>
#include <string>

// For OTA_CONFIRM_UPTIME_MS: the note below quotes the confirmation window, and a
// number that drifts from the real threshold is worse than no number.
#include "../utils/ota_confirm_gate.h"

OtaRevertDecision ota_revert_assessment(bool passive_has_valid_image, const std::string& passive_version,
                                        bool passive_marked_bad, bool running_pending_verify) {
  if (!passive_has_valid_image) {
    return {false,
            "No previous firmware to revert to: the passive OTA slot holds no valid image. A board "
            "flashed over USB writes only one slot; the other fills on the first OTA update."};
  }
  if (passive_marked_bad) {
    return {false,
            "The previous firmware is marked failed: an automatic rollback already rejected it, and "
            "the bootloader refuses to boot it again. Perform a fresh OTA update instead."};
  }
  // The offered text is interpolated verbatim into a JS confirm('...') and an
  // HTML title="..." on the promise it contains no quote characters. The
  // static sentences keep that promise by authorship; the version string comes
  // from the passive image's esp_app_desc and is NOT ours to promise for, so
  // anything outside a version-shaped alphabet is dropped here.
  std::string safe_version;
  for (char c : passive_version) {
    if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '+' || c == ' ') {
      safe_version += c;
    }
  }
  std::string text = "Revert to the previous firmware (" + safe_version +
                     ")? The board will reboot into the older image. WARNING: settings saved by the "
                     "current firmware may not be readable by the older one - check the settings "
                     "page after the revert and reconfigure anything that looks wrong.";
  if (running_pending_verify) {
    /* Re-decided once confirm-after-run reached main: this window is no longer a
       composition curiosity, it follows EVERY update, so the note has to be worth
       reading rather than merely true.
    
       The mechanism is not what the first version said. Reverting does not mark
       the running image failed - esp_ota_set_boot_partition() only writes a new
       otadata entry for the target slot. It is the REBOOT that does it: with
       rollback enabled the bootloader rewrites every PENDING_VERIFY otadata entry
       to ABORTED before it selects anything, so an image that reboots before
       confirming is failed by definition, whether or not a revert was involved.
       The offer is then withdrawn on the way back, because passive_marked_bad
       above covers ABORTED.
    
       Which is why the useful sentence is about WAITING, not about reverting: the
       one-way-ness is temporary, and a user who might want this image back only
       has to let it confirm first.

       And the other side of the flip, measured by the OTA-then-revert acceptance
       run and not something this text needs to say: the revert's own graceful_restart() arms a confirmation,
       and before the write side became target-aware that write landed on the
       slot being reverted INTO, so the
       older image came up VALID without a window of its own. The write side now
       declines once the boot selection has moved (ota_confirm_verdict), so the
       image reverted into arrives PENDING_VERIFY and earns its own window the same as
       it would on an outside-the-window revert. Nothing here changes: the
       running image is still failed by the reboot, and (b) still withdraws the
       offer on the way back. */
    text += " NOTE: the CURRENT firmware has not finished its " + std::to_string(OTA_CONFIRM_UPTIME_MS / 1000) +
            " second confirmation yet. Rebooting before it does marks it as failed, so this revert is "
            "one-way - you could not come back to this version. Wait for it to confirm first if you "
            "might want it back.";
  }
  return {true, text};
}

std::string ota_revert_refusal_text(const char* esp_err_name) {
  const std::string name = esp_err_name ? esp_err_name : "";
  if (name == "ESP_ERR_OTA_VALIDATE_FAILED") {
    return "The previous firmware image is incomplete or corrupted - most likely an interrupted "
           "OTA update wrote only part of it. It cannot be booted; perform a fresh OTA update "
           "instead. (" +
           name + ")";
  }
  if (name == "ESP_ERR_INVALID_ARG") {
    return "The passive OTA slot could not be selected as the boot partition. (" + name + ")";
  }
  return "Revert refused: " + name;
}
