#include <gtest/gtest.h>

#include <string>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/utils/events.h"

/* The two GPIO events must keep naming the components they were raised for.
 *
 * get_event_message_string() is re-rendered every time an event is published -
 * the events page, MQTT and ESP-NOW all call it - so a message built from state
 * that anything else can overwrite names whoever failed an allocation MOST
 * RECENTLY, not the components the event is about.
 *
 * Both events used to read one shared pair of names written by alloc_pins(), so
 * a missing-pin failure after a pin conflict re-pointed the conflict's message
 * at the wrong component, and a conflict after a missing pin did the same in
 * reverse. Neither event is ever cleared on a misconfigured board - both are
 * raised again on every boot - so the wrong text is what the user reads for the
 * life of the board.
 *
 * These drive the real alloc_pins() path and read the rendered string, because
 * the defect is not in raising the event but in what the message says later.
 */
namespace {

class FakeHal : public Esp32Hal {
 public:
  const char* name() override { return "gpio event name test board"; }
  std::vector<comm_interface> available_interfaces() override { return {}; }
};

class GpioEventNamesFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_all_events();
    delete esp32hal;
    esp32hal = new FakeHal();
  }

  void TearDown() override {
    delete esp32hal;
    esp32hal = nullptr;
    reset_all_events();
  }

  std::string message_for(EVENTS_ENUM_TYPE event) { return get_event_message_string(event).str(); }
};

}  // namespace

TEST_F(GpioEventNamesFixture, AConflictNamesTheClaimantAndTheHolder) {
  esp32hal->alloc_pins("Battery", GPIO_NUM_18);
  ASSERT_FALSE(esp32hal->alloc_pins("Charger", GPIO_NUM_18));

  // The ROLES, not merely the presence of both names: the claimant is the one
  // that just failed, the holder is the one already using the pin, and a
  // message that swaps them sends the user to the wrong setting.
  const std::string message = message_for(EVENT_GPIO_CONFLICT);
  EXPECT_NE(message.find("The pin used by 'Charger' is already allocated by 'Battery'"), std::string::npos) << message;
}

TEST_F(GpioEventNamesFixture, AMissingPinNamesTheComponentThatWantedIt) {
  ASSERT_FALSE(esp32hal->alloc_pins("Inverter", GPIO_NUM_NC));

  EXPECT_NE(message_for(EVENT_GPIO_NOT_DEFINED).find("Inverter"), std::string::npos);
}

/* The defect, in the direction that hit the conflict message. */
TEST_F(GpioEventNamesFixture, AConflictKeepsItsNamesAfterALaterMissingPin) {
  esp32hal->alloc_pins("Battery", GPIO_NUM_18);
  ASSERT_FALSE(esp32hal->alloc_pins("Charger", GPIO_NUM_18));

  // A different component now fails for a different reason, as a misconfigured
  // board does - one bad setting rarely arrives alone.
  ASSERT_FALSE(esp32hal->alloc_pins("Inverter", GPIO_NUM_NC));

  const std::string message = message_for(EVENT_GPIO_CONFLICT);
  EXPECT_NE(message.find("Charger"), std::string::npos) << message;
  EXPECT_NE(message.find("Battery"), std::string::npos) << message;
  EXPECT_EQ(message.find("Inverter"), std::string::npos)
      << "the conflict message picked up a later, unrelated allocation failure: " << message;
}

/* And the same defect in the other direction. */
TEST_F(GpioEventNamesFixture, AMissingPinKeepsItsNameAfterALaterConflict) {
  ASSERT_FALSE(esp32hal->alloc_pins("Inverter", GPIO_NUM_NC));

  esp32hal->alloc_pins("Battery", GPIO_NUM_18);
  ASSERT_FALSE(esp32hal->alloc_pins("Charger", GPIO_NUM_18));

  const std::string message = message_for(EVENT_GPIO_NOT_DEFINED);
  EXPECT_NE(message.find("Inverter"), std::string::npos) << message;
  EXPECT_EQ(message.find("Charger"), std::string::npos)
      << "the missing-pin message picked up a later, unrelated conflict: " << message;
}

/* A second conflict SHOULD win - it is the same event, and the newest failure is
 * the one worth reading. This pins that the fix separated the two events rather
 * than freezing the first value each event ever saw. */
TEST_F(GpioEventNamesFixture, ALaterConflictReplacesTheEarlierOnesNames) {
  esp32hal->alloc_pins("Battery", GPIO_NUM_18);
  ASSERT_FALSE(esp32hal->alloc_pins("Charger", GPIO_NUM_18));

  esp32hal->alloc_pins("Shunt", GPIO_NUM_19);
  ASSERT_FALSE(esp32hal->alloc_pins("Display", GPIO_NUM_19));

  const std::string message = message_for(EVENT_GPIO_CONFLICT);
  EXPECT_NE(message.find("Display"), std::string::npos) << message;
  EXPECT_NE(message.find("Shunt"), std::string::npos) << message;
}
