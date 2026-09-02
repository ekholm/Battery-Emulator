#include <gtest/gtest.h>

#include <map>
#include <string>

#include "../Software/src/devboard/webserver/select_options.h"

/* A <select> whose stored value matches no option renders with nothing
 * selected, and HTML fills that silence with a guess: the browser displays and
 * submits the FIRST option, so the next save rewrites the setting to something
 * nobody chose. For the comm selects that guess is CAN 1 Native. The builders
 * now guarantee the stored value is always represented; these tests pin both
 * halves - the guarantee, and that a represented value renders byte-identically
 * to what the page always produced.
 */
namespace {

// The shape of comm_interface on a build without the FD addon: 1..7 declared,
// 5 and 6 present in the enum but blank-named, so the page offers {1,2,3,4,7}.
enum class FakeComm { A = 1, B = 2, C = 3, D = 4, HiddenE = 5, HiddenF = 6, G = 7, Highest };

const char* fake_comm_name(FakeComm v) {
  switch (v) {
    case FakeComm::A:
      return "CAN 1 Native";
    case FakeComm::B:
      return "CAN 2 Native";
    case FakeComm::C:
      return "Modbus";
    case FakeComm::D:
      return "RS485";
    case FakeComm::G:
      return "CAN FD Addon 2";
    default:
      return "";  // valid member, not offered on this hardware
  }
}

enum class FakeType { None = 0, X = 1, Y = 2, Highest };
const char* fake_type_name(FakeType v) {
  switch (v) {
    case FakeType::None:
      return "None";
    case FakeType::X:
      return "X-battery";
    case FakeType::Y:
      return "Y-battery";
    default:
      return "";
  }
}

std::string str(const String& s) {
  return std::string(s.c_str());
}

int count(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
    n++;
  }
  return n;
}

}  // namespace

TEST(SelectOptionsTest, ARepresentedValueRendersExactlyAsBefore) {
  // The fix must cost nothing on the path every healthy board takes. This is
  // the pre-fix output, verbatim.
  const std::string got = str(options_for_enum(FakeComm::C, fake_comm_name));
  EXPECT_EQ(got,
            "<option value=\"1\">CAN 1 Native</option>"
            "<option value=\"2\">CAN 2 Native</option>"
            "<option value=\"7\">CAN FD Addon 2</option>"
            "<option value=\"3\" selected>Modbus</option>"
            "<option value=\"4\">RS485</option>");
}

TEST(SelectOptionsTest, EveryRenderedSelectHasExactlyOneSelectedOption) {
  // The invariant the browser depends on, swept across the interesting values:
  // in-enum, hidden-in-enum, zero, and garbage.
  for (int v : {1, 2, 3, 4, 5, 6, 7, 0, 99, -1}) {
    const std::string got = str(options_for_enum(static_cast<FakeComm>(v), fake_comm_name));
    EXPECT_EQ(count(got, " selected"), 1) << "stored value " << v << " rendered: " << got;
  }
}

TEST(SelectOptionsTest, AnOutOfEnumValueIsRepresentedNotGuessedAway) {
  // The stark case: a bad write left the comm setting at 0. Nothing in the
  // enum matches, and before the fix the browser would submit option 1.
  const std::string got = str(options_for_enum(static_cast<FakeComm>(0), fake_comm_name));
  EXPECT_NE(got.find("<option value=\"0\" selected>"), std::string::npos)
      << "the stored 0 must be an option carrying 0, so a save round-trips it: " << got;
  EXPECT_NE(got.find("not selectable on this build"), std::string::npos);
}

TEST(SelectOptionsTest, AValidButHiddenEnumMemberIsAlsoRepresented) {
  // The other road to the same cliff: the value IS a comm_interface member,
  // but this build blank-names it (an addon the hardware does not have), so
  // the name filter drops its option.
  const std::string got = str(options_for_enum(FakeComm::HiddenE, fake_comm_name));
  EXPECT_NE(got.find("<option value=\"5\" selected>"), std::string::npos) << got;
  EXPECT_EQ(count(got, " selected"), 1);
}

TEST(SelectOptionsTest, TheSentinelRoundTripsTheStoredNumberExactly) {
  // value= carries the STORED number - that is the whole mechanism by which a
  // save becomes a no-op instead of a rewrite.
  for (int v : {0, 5, 42, 255}) {
    const std::string got = str(options_for_enum(static_cast<FakeComm>(v), fake_comm_name));
    if (fake_comm_name(static_cast<FakeComm>(v))[0] != '\0' && v >= 1 && v < 8) {
      continue;  // represented normally; covered above
    }
    EXPECT_NE(got.find("<option value=\"" + std::to_string(v) + "\" selected>"), std::string::npos)
        << "stored " << v << ": " << got;
  }
}

TEST(SelectOptionsTest, WithNoneBuildersGetTheSameGuarantee) {
  const std::string ok = str(options_for_enum_with_none(FakeType::None, fake_type_name, FakeType::None));
  EXPECT_EQ(count(ok, " selected"), 1);
  EXPECT_EQ(ok.find("not selectable"), std::string::npos) << "None is a real option; no sentinel owed";

  const std::string bad = str(options_for_enum_with_none(static_cast<FakeType>(9), fake_type_name, FakeType::None));
  EXPECT_EQ(count(bad, " selected"), 1);
  EXPECT_NE(bad.find("<option value=\"9\" selected>"), std::string::npos) << bad;
}

TEST(SelectOptionsTest, MapBuilderGetsTheSameGuarantee) {
  const std::map<int, String> modes = {{0, "Classic"}, {1, "Flow"}};
  EXPECT_EQ(count(str(options_from_map(1, modes)), " selected"), 1);
  const std::string bad = str(options_from_map(7, modes));
  EXPECT_EQ(count(bad, " selected"), 1);
  EXPECT_NE(bad.find("<option value=\"7\" selected>"), std::string::npos) << bad;
}

TEST(SelectOptionsTest, TheSentinelTextSurvivesAnHtmlParserAsOneOption) {
  // The sentinel is markup we invented; it must not smuggle structure. Cheap
  // structural check: one '<option', one '</option>', no quotes beyond the
  // value attribute's own pair.
  const std::string s = str(unrepresented_option(42));
  EXPECT_EQ(count(s, "<option"), 1);
  EXPECT_EQ(count(s, "</option>"), 1);
  EXPECT_EQ(count(s, "\""), 2);
}
