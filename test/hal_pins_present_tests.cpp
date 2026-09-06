#include <gtest/gtest.h>

#include <vector>

#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/utils/events.h"

/* `pins_present()` itself, run rather than read.
 *
 * The predicate exists to tell two alloc_pins() failures apart: a pin that is
 * GPIO_NUM_NC, meaning the board does not have the interface at all, and a pin
 * another component already owns, meaning the map is incoherent. init_CAN()
 * skips the first and still aborts the board on the second.
 *
 * Its only other test is a SOURCE SCAN over comm_can.cpp, which can see that
 * the call is written in the right place and can see nothing about what it
 * does. Two edits that keep the call exactly where it is pass that scan and
 * break the firmware:
 *
 *   - a predicate that always answers yes leaves the whole-board abort firing
 *     for an absent add-on, which is the defect the fix was written for;
 *   - a predicate that ALLOCATES the pins it inspects makes the very next
 *     alloc_pins() call find them already owned, so every board that DOES have
 *     an MCP2515 raises EVENT_GPIO_CONFLICT and aborts - the fix for the board
 *     without the chip would take out every board with it.
 *
 * hal.h compiles into this binary (eleven test files include it), so both are
 * cheap to pin behaviourally, and neither needs comm_can.cpp.
 */
namespace {

// A HAL that is nothing but its pin allocator: the base class supplies
// alloc_pins()/pins_present() and every board getter has a default.
class PinAllocatorHal : public Esp32Hal {
 public:
  const char* name() override { return "pins-present-fixture"; }
#ifdef SDCARD
  uint8_t SD_SPI_BUS() override { return 0; }
#endif
  std::vector<comm_interface> available_interfaces() override { return {}; }
};

class PinsPresentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // init_events() assigns the levels; reset_all_events() only clears state.
    init_events();
    reset_all_events();
  }
};

}  // namespace

TEST_F(PinsPresentTest, EveryPinRealMeansPresent) {
  PinAllocatorHal hal;
  EXPECT_TRUE(hal.pins_present("CAN", GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_5))
      << "a board whose pins are all real pads must be reported as HAVING the interface - "
         "answering no here sends init_CAN() past an add-on that is fitted";
}

TEST_F(PinsPresentTest, OneUndefinedPinMeansTheBoardDoesNotHaveIt) {
  PinAllocatorHal hal;
  EXPECT_FALSE(hal.pins_present("CAN", GPIO_NUM_12, GPIO_NUM_NC, GPIO_NUM_5))
      << "an interface with an undefined pin is not fitted on this board; answering yes puts it "
         "back into alloc_pins() and aborts every interface below it";
  EXPECT_FALSE(hal.pins_present("CAN", GPIO_NUM_NC, GPIO_NUM_13))
      << "the first pin is checked too, not only the later ones";
}

/* The absence must stay visible. alloc_pins() raised EVENT_GPIO_NOT_DEFINED on
 * this path before the fix, and the whole argument for skipping instead of
 * aborting is that nothing is lost by it - so the event, and the component name
 * the message is built from, have to survive. */
TEST_F(PinsPresentTest, AnAbsentInterfaceStillRaisesTheEventAndNamesItsComponent) {
  PinAllocatorHal hal;
  Esp32Hal* const previous = esp32hal;
  esp32hal = &hal;  // get_event_message_string() reads failed_allocator() off the global

  ASSERT_EQ(get_event_pointer(EVENT_GPIO_NOT_DEFINED)->state, EVENT_STATE_INACTIVE)
      << "the fixture starts with the event down, or the assertion below proves nothing";
  EXPECT_FALSE(hal.pins_present("CAN", GPIO_NUM_NC));

  EXPECT_EQ(get_event_pointer(EVENT_GPIO_NOT_DEFINED)->state, EVENT_STATE_ACTIVE)
      << "skipping an absent add-on must not also swallow the diagnostic that says it is absent";
  const std::string message = std::string(get_event_message_string(EVENT_GPIO_NOT_DEFINED).c_str());
  EXPECT_NE(message.find("CAN"), std::string::npos) << "the message must name the component that asked: " << message;

  esp32hal = previous;
}

/* THE ONE THAT COSTS A BOARD. `pins_present()` inspects; `alloc_pins()` claims.
 * If the predicate claims as well, the allocation two lines later in init_CAN()
 * finds the pins already owned, raises EVENT_GPIO_CONFLICT and returns - so a
 * board that HAS the MCP2515 loses CAN entirely, and it is the boards with the
 * chip that this fix was never supposed to touch.
 */
TEST_F(PinsPresentTest, AskingWhetherThePinsExistDoesNotClaimThem) {
  PinAllocatorHal hal;

  ASSERT_TRUE(hal.pins_present("CAN", GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_5));
  EXPECT_TRUE(hal.alloc_pins("CAN", GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_5))
      << "pins_present() allocated the pins it was asked to inspect, so the allocation that "
         "follows it in init_CAN() now conflicts with it - every board that HAS the add-on "
         "aborts on a pin map that is perfectly coherent";
}

/* And the other direction: a genuine conflict must still be a conflict.
 * pins_present() answers only the presence question, so a pad another component
 * owns is present, and it is alloc_pins() that refuses it. If the predicate
 * started answering no here, init_CAN() would SKIP an incoherent map instead of
 * stopping the board on it - the deliberate exception, deleted by the back door.
 */
TEST_F(PinsPresentTest, AnAlreadyOwnedPinIsStillPresentAndIsRefusedByTheAllocatorNotThePredicate) {
  PinAllocatorHal hal;

  ASSERT_TRUE(hal.alloc_pins("SD", GPIO_NUM_12));
  EXPECT_TRUE(hal.pins_present("CAN", GPIO_NUM_12))
      << "a pad another component owns is a real pad - if presence answers no for it, init_CAN() "
         "skips an incoherent pin map instead of stopping the board on it";
  EXPECT_FALSE(hal.alloc_pins("CAN", GPIO_NUM_12)) << "the conflict must still be refused by the allocator";
}
