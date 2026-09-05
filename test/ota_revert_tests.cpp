#include <gtest/gtest.h>

#include "../Software/src/devboard/utils/ota_confirm_gate.h"
#include "../Software/src/devboard/webserver/ota_revert.h"

// The revert control's whole value is refusing correctly: a fresh USB-flashed
// board has no image to revert to, a rolled-back image must not be offered
// again, and an offer must carry the settings-schema warning - a silent
// confirm dialog costs a user their configuration. Pure decision function, so
// the matrix runs on the host; the otadata mechanics stay bench-only as they
// did for the OTA lineage before this.

TEST(OtaRevertTest, EmptyPassiveSlotDisablesWithTheUsbFlashExplanation) {
  auto d = ota_revert_assessment(false, "", false, false);
  EXPECT_FALSE(d.offered);
  EXPECT_NE(d.text.find("no valid image"), std::string::npos);
  EXPECT_NE(d.text.find("USB"), std::string::npos) << "the reason must explain HOW a slot ends up empty";
}

TEST(OtaRevertTest, RolledBackImageDisablesWithTheRollbackExplanation) {
  auto d = ota_revert_assessment(true, "v1.2.3", true, false);
  EXPECT_FALSE(d.offered);
  EXPECT_NE(d.text.find("automatic rollback"), std::string::npos);
  EXPECT_NE(d.text.find("bootloader refuses"), std::string::npos);
}

// An unreadable slot must win over its state flags: a slot with no image can
// carry stale otadata, and offering "revert to ()" would be nonsense.
TEST(OtaRevertTest, MissingImageWinsOverStateFlags) {
  auto d = ota_revert_assessment(false, "", true, true);
  EXPECT_FALSE(d.offered);
  EXPECT_NE(d.text.find("no valid image"), std::string::npos);
  EXPECT_EQ(d.text.find("rollback"), std::string::npos);
}

TEST(OtaRevertTest, ValidPassiveImageIsOfferedWithVersionAndSettingsWarning) {
  auto d = ota_revert_assessment(true, "v12.4.0-7-gabc1234", false, false);
  EXPECT_TRUE(d.offered);
  EXPECT_NE(d.text.find("v12.4.0-7-gabc1234"), std::string::npos)
      << "the person deciding must see WHICH firmware they get";
  EXPECT_NE(d.text.find("settings"), std::string::npos);
  EXPECT_NE(d.text.find("may not be readable"), std::string::npos)
      << "the settings-schema warning is the user-visible risk and must not be silent";
  EXPECT_EQ(d.text.find("has not finished its"), std::string::npos);
}

// The offered text lands verbatim inside confirm('...') and title="..." - the
// static sentences are authored quote-free, but the version string comes from
// the passive image's esp_app_desc, which is not this code's to promise for.
TEST(OtaRevertTest, AQuoteBearingVersionStringCannotBreakOutOfTheDialog) {
  auto d = ota_revert_assessment(true, "v1.0')); alert(1); //\"", false, false);
  ASSERT_TRUE(d.offered);
  EXPECT_EQ(d.text.find('\''), std::string::npos);
  EXPECT_EQ(d.text.find('"'), std::string::npos);
  EXPECT_EQ(d.text.find(';'), std::string::npos) << "only version-shaped characters may survive";
  EXPECT_NE(d.text.find("v1.0"), std::string::npos) << "the sanitizer must keep the version-shaped part";
}

// A revert issued inside the pending-verify window is a distinct, deliberate
// path: still offered, and the extra consequence - the running image gets
// marked failed on the way out - is said in the same dialog.
TEST(OtaRevertTest, PendingVerifyWindowIsOfferedWithTheExtraConsequence) {
  auto d = ota_revert_assessment(true, "v1.0.0", false, true);
  EXPECT_TRUE(d.offered);
  EXPECT_NE(d.text.find("has not finished its"), std::string::npos);
  EXPECT_NE(d.text.find("one-way"), std::string::npos);
  EXPECT_NE(d.text.find("Wait for it to confirm first"), std::string::npos)
      << "the window is temporary, and saying so is what makes the note actionable rather than "
         "just alarming";
  EXPECT_NE(d.text.find("may not be readable"), std::string::npos) << "the settings warning must survive the note";
}

// A slot half-written by an interrupted OTA update carries a readable header
// (so the control is offered) and still fails set_boot_partition's full-image
// validation - a bench board with a deliberately truncated app1 answered the
// click with the raw enum. The refusal must reach the user as a sentence.
TEST(OtaRevertTest, ValidateFailedRefusalIsAUserSentenceNotAnEnum) {
  std::string text = ota_revert_refusal_text("ESP_ERR_OTA_VALIDATE_FAILED");
  EXPECT_NE(text.find("incomplete or corrupted"), std::string::npos);
  EXPECT_NE(text.find("interrupted"), std::string::npos) << "the likely cause must be named";
  EXPECT_NE(text.find("ESP_ERR_OTA_VALIDATE_FAILED"), std::string::npos) << "keep the enum for bug reports";
  EXPECT_NE(ota_revert_refusal_text("ESP_ERR_UNKNOWN_THING").find("ESP_ERR_UNKNOWN_THING"), std::string::npos);
}

/* The note quotes the confirmation window at the user. That number is only worth
 * printing while it is the number the gate actually enforces, and the two live in
 * different files - this is the one composition of these two features that can be
 * checked off-target, so it is checked here rather than left to the bench.
 */
TEST(OtaRevertTest, TheNoteQuotesTheConfirmationWindowTheGateActuallyEnforces) {
  const OtaRevertDecision d = ota_revert_assessment(true, "v1.0", false, true);
  ASSERT_TRUE(d.offered);

  const std::string seconds = std::to_string(OTA_CONFIRM_UPTIME_MS / 1000);
  EXPECT_NE(d.text.find("its " + seconds + " second confirmation"), std::string::npos)
      << "the note must quote the gate's own threshold, or it tells the user to wait for a window "
         "that is not the one being enforced: "
      << d.text;
}
