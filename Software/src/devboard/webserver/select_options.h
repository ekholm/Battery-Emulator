#ifndef SELECT_OPTIONS_H
#define SELECT_OPTIONS_H

#include <WString.h>

#include <algorithm>
#include <utility>
#include <vector>

/* The <select> option builders for the settings page.
 *
 * In their own header, not for reuse but for the host suite: settings_html.cpp
 * is webserver territory and cannot link on the host, and the defect these
 * builders carried was invisible for exactly that reason.
 *
 * THE RULE EVERY BUILDER HERE ENFORCES: the stored value is ALWAYS represented
 * in the rendered options, marked selected. A select whose stored value matches
 * no option renders with nothing selected, the browser displays and submits the
 * FIRST option instead, and the next save silently rewrites the setting to
 * that first option - CAN 1 Native, for the comm selects. That is not
 * hypothetical: a settings-save replay against a board whose BATTCOMM held 0
 * (a bad write had zeroed it) would have "repaired" it to a guess without
 * anyone choosing anything.
 *
 * A stored value can fail to match for two reasons, and both are real: it is
 * outside the enum entirely (0, or a corrupt write), or it IS a valid member
 * whose name is blank on this build (an addon interface on hardware without
 * the addon - the name filter drops it). Either way the builder appends a
 * clearly-labelled option carrying the STORED number as its value, marked
 * selected: the page tells the truth, a save round-trips the stored value
 * unchanged, and changing it becomes something a person does on purpose.
 */

template <typename E>
constexpr auto to_underlying(E e) noexcept {
  return static_cast<std::underlying_type_t<E>>(e);
}

template <typename EnumType>
std::vector<EnumType> enum_values() {
  static_assert(std::is_enum_v<EnumType>, "Template argument must be an enum type.");

  constexpr auto count = to_underlying(EnumType::Highest);
  std::vector<EnumType> values;
  for (int i = 1; i < count; ++i) {
    values.push_back(static_cast<EnumType>(i));
  }
  return values;
}

template <typename EnumType, typename Func>
std::vector<std::pair<String, EnumType>> enum_values_and_names(Func name_for_type,
                                                               const EnumType* noneValue = nullptr) {
  auto values = enum_values<EnumType>();

  std::vector<std::pair<String, EnumType>> pairs;

  for (auto& type : values) {
    auto name = name_for_type(type);
    if (name != nullptr) {
      pairs.push_back(std::pair(String(name), type));
    }
  }

  std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  if (noneValue) {
    pairs.insert(pairs.begin(), std::pair(name_for_type(*noneValue), *noneValue));
  }

  return pairs;
}

/* The stored value matched no rendered option. Represent it rather than let
 * the browser guess: value = the stored number, so a save is a no-op until a
 * person picks something else. */
inline String unrepresented_option(int stored) {
  return "<option value=\"" + String(stored) + "\" selected>Stored value " + String(stored) +
         " (not selectable on this build)</option>";
}

template <typename TEnum, typename Func>
String options_for_enum_with_none(TEnum selected, Func name_for_type, TEnum noneValue) {
  String options;
  bool represented = false;
  TEnum none = noneValue;
  auto values = enum_values_and_names<TEnum>(name_for_type, &none);
  for (const auto& [name, type] : values) {
    options +=
        ("<option value=\"" + String(static_cast<int>(type)) + "\"" + (selected == type ? " selected" : "") + ">");
    options += name;
    options += "</option>";
    represented = represented || (selected == type);
  }
  if (!represented) {
    options = unrepresented_option(static_cast<int>(selected)) + options;
  }
  return options;
}

template <typename TEnum, typename Func>
String options_for_enum(TEnum selected, Func name_for_type) {
  String options;
  bool represented = false;
  auto values = enum_values_and_names<TEnum>(name_for_type, nullptr);
  for (const auto& [name, type] : values) {
    if (name[0] == '\0')
      continue;  // Don't show blank options
    options +=
        ("<option value=\"" + String(static_cast<int>(type)) + "\"" + (selected == type ? " selected" : "") + ">");
    options += name;
    options += "</option>";
    represented = represented || (selected == type);
  }
  if (!represented) {
    options = unrepresented_option(static_cast<int>(selected)) + options;
  }
  return options;
}

template <typename TMap>
String options_from_map(int selected, const TMap& value_name_map) {
  String options;
  bool represented = false;
  for (const auto& [value, name] : value_name_map) {
    options += "<option value=\"" + String(value) + "\"";
    if (selected == value) {
      options += " selected";
      represented = true;
    }
    options += ">";
    options += name;
    options += "</option>";
  }
  if (!represented) {
    options = unrepresented_option(selected) + options;
  }
  return options;
}

#endif  // SELECT_OPTIONS_H
