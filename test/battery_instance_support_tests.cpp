#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/datalayer/datalayer.h"

/* `battery_supports_double()` / `battery_supports_triple()` decide two things: whether the UI
 * offers a second or third battery at all, and whether setup_battery() is allowed to try. The
 * switches in setup_battery() decide whether it can actually build one. Those are two lists of
 * battery types written in different places, and the file asks for them to agree in a comment -
 * "Must match the switch in setup_battery() below" - which is not a thing a comment can enforce.
 *
 * They had drifted: CmpSmartCar had a working battery3 case and was absent from the triple
 * predicate, so the guard refused a configuration the code underneath could have built.
 *
 * The invariant has TWO directions and they need different instruments, which is worth stating
 * because it is not obvious and it is why both kinds of test are here.
 *
 *   predicate true, switch cannot build  -> the guard passes and setup_battery() silently does
 *                                           nothing. Catchable by RUNNING setup_battery(), which
 *                                           is what the first two tests do.
 *   switch can build, predicate false    -> the case is DEAD CODE and the configuration is
 *                                           refused. NOT catchable by running anything: the
 *                                           switch sits behind the predicate's guard, so the
 *                                           unreachable case is unreachable from a test too.
 *                                           That is this file's third test, and it has to read
 *                                           the source.
 *
 * The second direction is the one that had drifted - CmpSmartCar had a working battery3 case and
 * was absent from the triple predicate - so a suite with only the runnable half would have stayed
 * green through it.
 */
namespace {

// setup_battery() only builds into an empty slot, and the slots are globals shared by the whole
// binary, so each case starts from nothing.
void clear_batteries() {
  delete battery;
  delete battery2;
  delete battery3;
  battery = nullptr;
  battery2 = nullptr;
  battery3 = nullptr;
}

// Every value the enum can take, so a type added without a decision shows up here rather than
// being silently absent from one of the two lists.
std::vector<BatteryType> all_battery_types() {
  std::vector<BatteryType> types;
  for (int i = 0; i < static_cast<int>(BatteryType::Highest); i++) {
    types.push_back(static_cast<BatteryType>(i));
  }
  return types;
}

std::string name_of(BatteryType t) {
  const char* n = name_for_battery_type(t);
  return n ? n : ("BatteryType(" + std::to_string(static_cast<int>(t)) + ")");
}

}  // namespace

TEST(BatteryInstanceSupport, TheDoublePredicateIsTrueExactlyWhenASecondBatteryCanBeBuilt) {
  for (BatteryType type : all_battery_types()) {
    clear_batteries();
    user_selected_battery_type = type;
    user_selected_second_battery = true;
    user_selected_triple_battery = false;
    setup_battery();

    const bool built = battery2 != nullptr;
    EXPECT_EQ(battery_supports_double(type), built)
        << name_of(type) << ": battery_supports_double() says " << battery_supports_double(type)
        << " but setup_battery() " << (built ? "built" : "did not build") << " a second battery.\n"
        << "  These are two lists of types in one file and they have to agree: the predicate gates "
           "the UI and the guard, the switch does the work.";
  }
  clear_batteries();
}

TEST(BatteryInstanceSupport, TheTriplePredicateIsTrueExactlyWhenAThirdBatteryCanBeBuilt) {
  for (BatteryType type : all_battery_types()) {
    clear_batteries();
    user_selected_battery_type = type;
    user_selected_second_battery = true;
    user_selected_triple_battery = true;
    setup_battery();

    const bool built = battery3 != nullptr;
    EXPECT_EQ(battery_supports_triple(type), built)
        << name_of(type) << ": battery_supports_triple() says " << battery_supports_triple(type)
        << " but setup_battery() " << (built ? "built" : "did not build") << " a third battery.\n"
        << "  A type in the switch but not the predicate is refused a configuration the code can "
           "build; one in the predicate but not the switch passes the guard and then silently does "
           "nothing.";
  }
  clear_batteries();
}

/* The direction the running tests cannot see: a type the switch can build and the predicate
 * refuses. The case is behind the guard, so no amount of driving setup_battery() reaches it -
 * the source is the only place the two lists are both visible.
 *
 * Reading the source is a weaker instrument than running the code and it is used here because
 * the code cannot be run. The shape that would retire it is extracting each construction switch
 * into a function returning Battery*, which a test could then call without the guard.
 */
namespace {

std::string batteries_source() {
  const std::string self = __FILE__;
  const std::string dir = self.substr(0, self.find_last_of('/'));
  std::ifstream src(dir + "/../Software/src/battery/BATTERIES.cpp");
  EXPECT_TRUE(src.is_open()) << "BATTERIES.cpp is not where this test looks";
  return std::string((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
}

// The `case BatteryType::X:` labels between `from` and the `default:` that closes that switch.
std::set<std::string> case_labels(const std::string& src, const std::string& from) {
  const size_t at = src.find(from);
  EXPECT_NE(at, std::string::npos) << from << " is not where this test looks";
  if (at == std::string::npos) {
    return {};
  }
  const std::string block = src.substr(at, src.find("default:", at) - at);
  std::set<std::string> labels;
  const std::string needle = "case BatteryType::";
  for (size_t i = block.find(needle); i != std::string::npos; i = block.find(needle, i + 1)) {
    const size_t start = i + needle.size();
    labels.insert(block.substr(start, block.find(':', start) - start));
  }
  return labels;
}

}  // namespace

TEST(BatteryInstanceSupport, NoConstructionCaseIsUnreachableBehindItsOwnPredicate) {
  const std::string src = batteries_source();

  const std::pair<std::string, std::string> pairs[] = {
      {"bool battery_supports_double(BatteryType type) {", "battery_supports_double(user_selected_battery_type)"},
      {"bool battery_supports_triple(BatteryType type) {", "battery_supports_triple(user_selected_battery_type)"},
  };
  for (const auto& [predicate, guard] : pairs) {
    const std::set<std::string> declared = case_labels(src, predicate);
    const std::set<std::string> buildable = case_labels(src, guard);
    ASSERT_FALSE(declared.empty());
    ASSERT_FALSE(buildable.empty());

    for (const std::string& type : buildable) {
      EXPECT_TRUE(declared.count(type) != 0)
          << type << " has a construction case that " << predicate
          << " does not declare, so the case is dead code and the configuration is refused - "
             "the UI never offers it and the guard prints the not-supported diagnostic.";
    }
  }
}
