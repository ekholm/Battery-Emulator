#ifndef OTA_REVERT_H
#define OTA_REVERT_H

#include <string>

/* Decides whether the "revert to previous firmware" control is offered, and
   with what text. Every partition scheme this project ships is dual-slot, so
   the previous image is physically retained after an OTA update - but the
   feature is the caveats, not the esp_ota_set_boot_partition() call:

   - A board flashed over USB has NO valid image in the passive slot (esptool
     writes one slot), so the control must render disabled with a reason and
     never fail on click.
   - A passive slot marked INVALID/ABORTED was already rejected by an
     automatic rollback; the bootloader refuses to boot it and
     esp_ota_set_boot_partition() refuses to select it. The UI says WHY the
     control is unavailable exactly when a rollback already fired.
   - Reverting across a settings-schema change can leave stored settings
     unreadable by the older image - the confirm text warns, it does not stay
     silent.
   - A revert issued while the RUNNING image is still unconfirmed (the
     pending-verify window of the confirm-after-run mechanism) is a one-way
     trip: the reboot it performs is what fails the running image, because the
     bootloader rewrites every PENDING_VERIFY otadata entry to ABORTED before
     it selects anything, and the ABORTED case above then withdraws the offer
     in the other direction. The note says so, and says that waiting for the
     confirmation is what makes the trip reversible.

   Pure function of four already-extracted facts, so the whole matrix runs on
   the host; the caller wires esp_ota_get_partition_description() /
   esp_ota_get_state_partition() on the target. */
struct OtaRevertDecision {
  /** True: render the enabled control with `text` as its confirm dialog.
      False: render it disabled with `text` as the reason. */
  bool offered;
  std::string text;
};

OtaRevertDecision ota_revert_assessment(bool passive_has_valid_image, const std::string& passive_version,
                                        bool passive_marked_bad, bool running_pending_verify);

/* Maps esp_ota_set_boot_partition()'s refusal to a user-shaped sentence.
   The offer is decided from the passive slot's DESCRIPTION, which proves only
   that the image's header is present - a slot half-written by an interrupted
   OTA update carries a readable header and still fails the full-image
   validation set_boot_partition performs (a bench board reproduced exactly
   that). Unknown errors pass through by name. */
std::string ota_revert_refusal_text(const char* esp_err_name);

#endif  // OTA_REVERT_H
