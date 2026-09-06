#include <gtest/gtest.h>

#include "../Software/src/communication/can/can_init_plan.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/utils/events.h"

namespace {

// A HAL whose CAN-FD pins the test sets, so a board that has the add-on and one
// that does not are the same object with different answers. Everything else
// inherits the base class, which returns GPIO_NUM_NC.
class FdPlanHal : public Esp32Hal {
 public:
  const char* name() override { return "FD init plan test board"; }
#ifdef SDCARD
  uint8_t SD_SPI_BUS() override { return 0; }
#endif  // SDCARD
  std::vector<comm_interface> available_interfaces() override { return {}; }

  gpio_num_t MCP2517_SCK() override { return sck; }
  gpio_num_t MCP2517_SDO() override { return sdo; }
  gpio_num_t MCP2517_SDI() override { return sdi; }
  gpio_num_t MCP2517_CS() override { return cs; }
  gpio_num_t MCP2517_INT() override { return interrupt; }

  uint8_t MCP2517_BUS() override { return bus; }
  uint8_t MCP2517_BUS2() override { return bus2; }
  gpio_num_t MCP2517_SCK2() override { return sck2; }
  gpio_num_t MCP2517_SDO2() override { return sdo2; }
  gpio_num_t MCP2517_SDI2() override { return sdi2; }
  gpio_num_t MCP2517_CS2() override { return cs2; }
  gpio_num_t MCP2517_INT2() override { return interrupt2; }

  // A board with the add-on fitted: every FD pin a real pad, both chips on the
  // one bus, which is what stark and the BECom declare.
  gpio_num_t sck = GPIO_NUM_17, sdo = GPIO_NUM_34, sdi = GPIO_NUM_5;
  gpio_num_t cs = GPIO_NUM_18, interrupt = GPIO_NUM_35;
  uint8_t bus = 1, bus2 = 1;
  gpio_num_t sck2 = GPIO_NUM_NC, sdo2 = GPIO_NUM_NC, sdi2 = GPIO_NUM_NC;
  gpio_num_t cs2 = GPIO_NUM_12, interrupt2 = GPIO_NUM_14;
};

bool event_is_active(EVENTS_ENUM_TYPE event) {
  const EVENTS_STRUCT_TYPE* entry = get_event_pointer(event);
  return entry != nullptr && entry->occurences > 0;
}

class CanFdInitPlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hal = new FdPlanHal();
    esp32hal = hal;  // pins_present() reports through the global
    reset_all_events();
  }
  FdPlanHal* hal = nullptr;
};

TEST_F(CanFdInitPlanTest, ABoardWithTheAddonRunsEveryBlock) {
  const CanFdInitPlan plan = plan_canfd_init(hal, true, true);
  EXPECT_TRUE(plan.bus);
  EXPECT_TRUE(plan.first_chip);
  EXPECT_TRUE(plan.second_chip);
  EXPECT_FALSE(event_is_active(EVENT_GPIO_NOT_DEFINED));
}

// The defect this commit is for: an absent FD add-on used to abandon every
// interface declared after it, because alloc_pins() returned false and
// init_CAN() returned.
TEST_F(CanFdInitPlanTest, AnAbsentBusSkipsTheWholeFdGroup) {
  hal->sck = GPIO_NUM_NC;
  const CanFdInitPlan plan = plan_canfd_init(hal, true, false);
  EXPECT_FALSE(plan.bus);
  EXPECT_FALSE(plan.first_chip);
  EXPECT_TRUE(event_is_active(EVENT_GPIO_NOT_DEFINED));
}

// THE DEPENDENCY, and the reason this is a plan and not three conditions: the
// bus block creates the SPI object the chip blocks dereference, so a chip whose
// bus was skipped must be skipped too even though its own pins are real pads.
TEST_F(CanFdInitPlanTest, AChipWhoseBusWasSkippedIsSkippedThoughItsOwnPinsExist) {
  hal->sdi = GPIO_NUM_NC;  // bus incomplete; CS and INT are still real
  const CanFdInitPlan plan = plan_canfd_init(hal, true, false);
  EXPECT_FALSE(plan.bus);
  EXPECT_FALSE(plan.first_chip);
}

// The bus is fine and the chip is not: only that chip is skipped.
TEST_F(CanFdInitPlanTest, AnAbsentFirstChipOnAPresentBusIsSkipped) {
  hal->cs = GPIO_NUM_NC;
  const CanFdInitPlan plan = plan_canfd_init(hal, true, false);
  EXPECT_TRUE(plan.bus);
  EXPECT_FALSE(plan.first_chip);
  EXPECT_TRUE(event_is_active(EVENT_GPIO_NOT_DEFINED));
}

TEST_F(CanFdInitPlanTest, TheSecondChipSharingTheBusIsSkippedWhenTheBusIs) {
  hal->sck = GPIO_NUM_NC;
  const CanFdInitPlan plan = plan_canfd_init(hal, false, true);
  EXPECT_FALSE(plan.bus);
  EXPECT_FALSE(plan.first_chip);
  EXPECT_FALSE(plan.second_chip);
}

// The T-2CAN's FD variant puts its second chip on the OTHER SPI bus, so that
// chip does not depend on the first bus block at all.
TEST_F(CanFdInitPlanTest, TheSecondChipOnItsOwnBusDoesNotDependOnTheFirst) {
  hal->bus2 = 2;
  hal->sck2 = GPIO_NUM_16;
  hal->sdo2 = GPIO_NUM_15;
  hal->sdi2 = GPIO_NUM_13;
  hal->sck = GPIO_NUM_NC;  // the FIRST bus is absent
  const CanFdInitPlan plan = plan_canfd_init(hal, false, true);
  EXPECT_FALSE(plan.bus);
  EXPECT_FALSE(plan.first_chip);  // nobody registered it
  EXPECT_TRUE(plan.second_chip);
}

TEST_F(CanFdInitPlanTest, TheSecondChipOnItsOwnBusIsSkippedWhenThatBusIsAbsent) {
  hal->bus2 = 2;
  hal->sck2 = GPIO_NUM_NC;  // the non-FD T-2CAN: CS2/INT2 declared, bus 2 not
  const CanFdInitPlan plan = plan_canfd_init(hal, false, true);
  EXPECT_FALSE(plan.second_chip);
}

// stark and the BECom declare the first chip's pins AND a second chip on the
// same bus. A board that registers only the second one must not get the first
// planned off the back of those pins - nobody asked for that interface.
TEST_F(CanFdInitPlanTest, TheFirstChipIsNotPlannedWhenNobodyRegisteredIt) {
  const CanFdInitPlan plan = plan_canfd_init(hal, false, true);
  EXPECT_TRUE(plan.bus);
  EXPECT_FALSE(plan.first_chip);
  EXPECT_TRUE(plan.second_chip);
}

TEST_F(CanFdInitPlanTest, AnAbsentSecondChipDoesNotStopTheFirst) {
  hal->cs2 = GPIO_NUM_NC;
  const CanFdInitPlan plan = plan_canfd_init(hal, true, true);
  EXPECT_TRUE(plan.first_chip);
  EXPECT_FALSE(plan.second_chip);
}

// Presence is asked only for a chip the caller wants, because pins_present()
// raises an event: a board that declares no second chip must not be told its
// bus-2 pins are missing.
TEST_F(CanFdInitPlanTest, AChipNobodyRegisteredIsNeverAskedAbout) {
  hal->cs2 = GPIO_NUM_NC;
  hal->interrupt2 = GPIO_NUM_NC;
  const CanFdInitPlan plan = plan_canfd_init(hal, true, false);
  EXPECT_TRUE(plan.first_chip);
  EXPECT_FALSE(plan.second_chip);
  EXPECT_FALSE(event_is_active(EVENT_GPIO_NOT_DEFINED));
}

TEST_F(CanFdInitPlanTest, ABoardRegisteringNoFdInterfaceAsksNothingAndRunsNothing) {
  hal->sck = GPIO_NUM_NC;
  hal->cs = GPIO_NUM_NC;
  const CanFdInitPlan plan = plan_canfd_init(hal, false, false);
  EXPECT_FALSE(plan.bus);
  EXPECT_FALSE(plan.first_chip);
  EXPECT_FALSE(plan.second_chip);
  EXPECT_FALSE(event_is_active(EVENT_GPIO_NOT_DEFINED));
}

// Planning is a question, not a claim: the pins must still be free for
// alloc_pins() to take, or the fix for the board WITHOUT the chip would raise
// a conflict on every board with it.
TEST_F(CanFdInitPlanTest, PlanningDoesNotClaimThePins) {
  const CanFdInitPlan plan = plan_canfd_init(hal, true, true);
  ASSERT_TRUE(plan.bus);
  ASSERT_TRUE(plan.first_chip);
  // init_CAN()'s own order: the bus pins, then the chip's.
  EXPECT_TRUE(hal->alloc_pins("CANFD", hal->MCP2517_SCK(), hal->MCP2517_SDO(), hal->MCP2517_SDI()));
  EXPECT_TRUE(hal->alloc_pins("CANFD", hal->MCP2517_CS(), hal->MCP2517_INT()));
  EXPECT_FALSE(event_is_active(EVENT_GPIO_CONFLICT));
}

}  // namespace
