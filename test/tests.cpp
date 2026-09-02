#include <gtest/gtest.h>
#include <stdio.h>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/charger/CHARGERS.h"
#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/datalayer/datalayer_extended.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/safety/safety.h"
#include "../Software/src/devboard/utils/events.h"
#include "../Software/src/inverter/INVERTERS.h"

void RegisterCanLogTests(void);
void RegisterStillAliveTests(void);

class DataLayerResetListener : public ::testing::EmptyTestEventListener {
 public:
  void OnTestStart(const ::testing::TestInfo& /*test_info*/) override {
    datalayer = DataLayer();

    /* The extended datalayer is a UNION across battery types, and it was the
       omission here. Only one battery ever runs on a board, so two drivers
       sharing the storage costs nothing there - but this binary runs all of
       them, one after another, and a test that renders battery B's page after
       battery A's test has run is reading A's bytes through B's struct.
       UBSAN sees it first as "load of value 255, which is not a valid value
       for type 'bool'" in NissanLeafHtmlRenderer after FordMachEBattery has
       written the same union; the same pollution in a wider field would be a
       plausible wrong NUMBER that an assertion accepts instead. Order
       dependent, so it appears and disappears with the shuffle seed. */
    datalayer_extended = DataLayerExtended();

    reset_all_events();

    // Every instance holds pointers into the datalayer we just replaced, so
    // destroy them all.
    delete battery;
    battery = nullptr;
    delete battery2;
    battery2 = nullptr;
    delete battery3;
    battery3 = nullptr;
    delete charger;
    charger = nullptr;
    /* The inverter is the same kind of instance and was the one omission here.
       It also decides behaviour by TYPE - needs_can_startup_grace() is true for
       the SMA family and false for the rest - so a test inheriting the previous
       test's inverter silently runs against the wrong protocol. */
    delete inverter;
    inverter = nullptr;

    // Selection globals must be owned by each test's own fixture.
    user_selected_second_battery = false;
    user_selected_triple_battery = false;

    init_hal();
  }
};

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);

  // Add a listener to reset the datalayer and events before each test
  ::testing::UnitTest::GetInstance()->listeners().Append(new DataLayerResetListener);

  RegisterCanLogTests();
  RegisterStillAliveTests();

  return RUN_ALL_TESTS();
}

void store_settings_equipment_stop(void) {}
