#include <gtest/gtest.h>

#include <fstream>
#include <string>

// An empty submitted SSID must not overwrite the stored one.
//
// `/saveSettings` used to write SSID unguarded while PASSWORD right next to it
// was guarded, so any client that sent the field empty blanked the stored
// network - and since USB logging is off by default, the board then looks
// completely dead rather than merely unconfigured.
//
// webserver.cpp is not part of the host build (no FS.h, no async server), so
// this is a source guard rather than a behavioural test: it pins that the SSID
// branch tests the value before saving. If webserver.cpp is ever pulled into
// the host suite, replace this with a real POST through the handler.
TEST(SsidBlankGuard, AnEmptySsidIsNotWrittenOverTheStoredOne) {
  std::ifstream src(std::string(TEST_REPO_ROOT) + "/Software/src/devboard/webserver/webserver.cpp");
  ASSERT_TRUE(src.is_open()) << "cannot open webserver.cpp";
  const std::string text((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());

  const size_t branch = text.find("p->name() == \"SSID\"");
  ASSERT_NE(branch, std::string::npos) << "the SSID branch has moved - re-point this guard";

  const size_t save = text.find("settings.saveString(\"SSID\"", branch);
  ASSERT_NE(save, std::string::npos) << "no SSID save found after the branch";

  // Between entering the branch and saving, the value must be tested for empty.
  const std::string between = text.substr(branch, save - branch);
  // The NEGATION is the property, not the mention. `find("isEmpty()")` is
  // satisfied by `if (p->value().isEmpty()) { save }` - which saves ONLY when the
  // field is blank, so it both keeps the original defect and makes configuring
  // Wi-Fi impossible. That mutation passed this test before this line said `!`.
  EXPECT_NE(between.find("!p->value().isEmpty()"), std::string::npos)
      << "SSID is not saved under a NOT-empty test: an empty field will blank the stored "
         "network and strand the board. Between the branch and the save: "
      << between;
}
