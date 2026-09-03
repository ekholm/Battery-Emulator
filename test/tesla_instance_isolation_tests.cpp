#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "../Software/src/battery/TESLA-BATTERY.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/datalayer/datalayer_extended.h"

#include "Arduino.h"

/* There is exactly one datalayer_extended.tesla, and a double-Tesla setup runs
 * two TeslaBattery instances against it. Before the per-instance pointer both
 * constructors published into that one struct, so the second pack's values
 * landed on the first pack's advanced page - two packs interleaved into one
 * set of numbers, with nothing on the page to say so.
 *
 * These tests pin the split: the main instance still publishes, the second
 * instance publishes nothing at all. The positive control matters more than
 * usual here - "instance 2 wrote nothing" also passes if update_values() has
 * stopped writing for everyone, so the first test exists to fail in that case.
 */
namespace {

// A pattern no field would arrive at on its own, so any write is visible.
const uint8_t kPoison = 0xAB;

void poison_extended() {
  memset(&datalayer_extended.tesla, kPoison, sizeof(datalayer_extended.tesla));
}

bool extended_is_untouched() {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&datalayer_extended.tesla);
  for (size_t i = 0; i < sizeof(datalayer_extended.tesla); i++) {
    if (bytes[i] != kPoison) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST(TeslaInstanceIsolation, TheMainInstanceStillPublishesIntoTheExtendedStruct) {
  poison_extended();

  TeslaBattery main_battery;
  main_battery.update_values();

  EXPECT_FALSE(extended_is_untouched())
      << "the main battery published nothing into datalayer_extended.tesla - the advanced page is now "
         "empty for everyone, and the isolation test below would pass for the wrong reason";
}

TEST(TeslaInstanceIsolation, ASecondInstanceWritesNothingTheFirstCanSee) {
  DATALAYER_BATTERY_TYPE second_pack = {};
  poison_extended();

  TeslaBattery second_battery(&second_pack, CAN_NATIVE);
  second_battery.update_values();

  EXPECT_TRUE(extended_is_untouched())
      << "the second Tesla pack wrote into the shared extended struct - those values reach the FIRST "
         "battery's advanced page, where nothing marks them as another pack's";
}

TEST(TeslaInstanceIsolation, ASecondInstanceStillPublishesIntoItsOwnBatteryDatalayer) {
  /* The guard covers the extended struct only. Everything addressed through
     datalayer_battery-> is per-instance already and must keep flowing, or the
     second pack goes dark on the main page too. */
  DATALAYER_BATTERY_TYPE second_pack = {};
  second_pack.status.total_discharged_battery_Wh = 12345;

  TeslaBattery second_battery(&second_pack, CAN_NATIVE);
  second_battery.update_values();

  EXPECT_NE(second_pack.status.total_discharged_battery_Wh, 12345u)
      << "update_values() no longer publishes the second pack's own energy counters - the extended "
         "guard swallowed a write that was never about the extended struct";
}

TEST(TeslaInstanceIsolation, ASecondInstancesPageShowsNoDataRatherThanTheFirstPacks) {
  DATALAYER_BATTERY_TYPE second_pack = {};
  memset(&datalayer_extended.tesla, 0, sizeof(datalayer_extended.tesla));
  // A value the first pack's page would render, to catch it leaking into the second's.
  datalayer_extended.tesla.battery_beginning_of_life = 137;

  TeslaBattery second_battery(&second_pack, CAN_NATIVE);
  const std::string html = second_battery.get_status_renderer().get_status_html().c_str();

  EXPECT_EQ(html.find("137"), std::string::npos) << "the second pack's page rendered the FIRST pack's data";
  EXPECT_NE(html.find("No extended data"), std::string::npos) << "the second pack's page should say it has no data";
}

TEST(TeslaInstanceIsolation, TheMainInstancesPageStillRendersItsData) {
  memset(&datalayer_extended.tesla, 0, sizeof(datalayer_extended.tesla));
  datalayer_extended.tesla.battery_beginning_of_life = 137;

  TeslaBattery main_battery;
  const std::string html = main_battery.get_status_renderer().get_status_html().c_str();

  EXPECT_NE(html.find("Battery Beginning of Life: 137"), std::string::npos)
      << "the main battery's page lost its data - the nullptr branch is being taken for the instance "
         "that does own the extended struct";
}
