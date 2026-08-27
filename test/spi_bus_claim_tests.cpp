/* Make the shared-SPI-controller defect class visible.
 *
 * alloc_pins() allocates GPIO NUMBERS, so two devices that land on one SPI
 * controller with disjoint pins pass it without a word. They then both call
 * SPIClass::begin(), the controller re-points its single MISO input at whoever
 * called last, and the earlier device stops receiving - silently, with no event
 * and no failed return anywhere. That is what a T-CAN485 did with the SD card
 * and the MCP2515 add-on both on VSPI (measured 15/15 boots; the routing
 * registers were read back to confirm it).
 *
 * Esp32Hal::claim_spi_bus() is the sibling allocator for the controller itself.
 * These tests drive it directly rather than booting a board, because the thing
 * under test is the bookkeeping, and because the shape that shows it off - the
 * hw_lilygo before the bus fix - is exactly the shape that no longer exists on a fixed
 * tree.
 */

#include <gtest/gtest.h>

#include <string>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/hal/hal.h"
#include "../Software/src/devboard/utils/events.h"

namespace {

// The HAL is abstract only in name(); everything these tests touch is concrete
// on the base class, so a minimal board is enough.
class FakeHal : public Esp32Hal {
 public:
  const char* name() override { return "wq256 test board"; }
  std::vector<comm_interface> available_interfaces() override { return {}; }
};

/* The affected board, as it was. Pin numbers from hw_lilygo.h at upstream/main:
 * the SD card on sclk 14 / miso 2 / mosi 15, the MCP2515 add-on on sck 12 /
 * miso 34 / mosi 5, and both of them defaulting to the same controller. */
constexpr uint8_t kSharedBus = 2;
constexpr uint8_t kOtherBus = 3;
constexpr gpio_num_t kSdSclk = GPIO_NUM_14;
constexpr gpio_num_t kSdMiso = GPIO_NUM_2;
constexpr gpio_num_t kSdMosi = GPIO_NUM_15;
constexpr gpio_num_t kCanSck = GPIO_NUM_12;
constexpr gpio_num_t kCanMiso = GPIO_NUM_34;
constexpr gpio_num_t kCanMosi = GPIO_NUM_5;

class SpiBusClaimFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    delete esp32hal;
    esp32hal = new FakeHal();
    // init_events() is what assigns the per-event LEVELS; reset_all_events()
    // only clears state, so a fixture that checks a level needs both.
    init_events();
    reset_all_events();
  }

  void TearDown() override {
    delete esp32hal;
    esp32hal = nullptr;
  }

  bool conflict_raised() { return get_event_pointer(EVENT_SPI_BUS_CONFLICT)->state == EVENT_STATE_ACTIVE; }
};

// The whole point: the shape that shipped, reported instead of ignored.
TEST_F(SpiBusClaimFixture, TheShippedTcan485ShapeIsReported) {
  esp32hal->claim_spi_bus("SD Card", kSharedBus, kSdSclk, kSdMiso, kSdMosi);
  EXPECT_FALSE(conflict_raised()) << "one device on a controller is not a conflict";

  esp32hal->claim_spi_bus("CAN", kSharedBus, kCanSck, kCanMiso, kCanMosi);

  EXPECT_TRUE(conflict_raised()) << "the SD card and the MCP2515 add-on shared VSPI with different pins - the "
                                    "collision alloc_pins() cannot see, and the defect wq255 fixes";
}

// And the fixed shape stays quiet, or the guard would cry wolf on every boot of
// the board it was written for.
TEST_F(SpiBusClaimFixture, TheFixedTcan485ShapeIsSilent) {
  esp32hal->claim_spi_bus("SD Card", kOtherBus, kSdSclk, kSdMiso, kSdMosi);
  esp32hal->claim_spi_bus("CAN", kSharedBus, kCanSck, kCanMiso, kCanMosi);

  EXPECT_FALSE(conflict_raised());
}

/* Several devices on one bus with their own chip selects is how SPI is meant to
 * be used. Reporting that would make the guard useless: it would fire on every
 * correct multi-device bus, and the real collisions would be lost in the noise.
 */
TEST_F(SpiBusClaimFixture, SharingAControllerWithTheSameWiringIsAllowed) {
  esp32hal->claim_spi_bus("first device", kSharedBus, kCanSck, kCanMiso, kCanMosi);
  esp32hal->claim_spi_bus("second device", kSharedBus, kCanSck, kCanMiso, kCanMosi);

  EXPECT_FALSE(conflict_raised());
}

// One differing pin is enough - it is the MISO input that gets stolen, but a
// controller cannot hold two of any of the three.
TEST_F(SpiBusClaimFixture, OneDifferingPinIsAConflict) {
  esp32hal->claim_spi_bus("first device", kSharedBus, kCanSck, kCanMiso, kCanMosi);
  esp32hal->claim_spi_bus("second device", kSharedBus, kCanSck, GPIO_NUM_25, kCanMosi);

  EXPECT_TRUE(conflict_raised());
}

/* A board that leaves a device's pins undefined is describing no routing at
 * all, and GPIO_NUM_NC is what alloc_pins_ignore_unused() already treats as
 * "not present". Claiming a bus with nothing but NC must not reserve it, or the
 * first unconfigured device would make every later real one look like a
 * conflict. */
TEST_F(SpiBusClaimFixture, AClaimWithNoPinsReservesNothing) {
  esp32hal->claim_spi_bus("unconfigured device", kSharedBus, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
  esp32hal->claim_spi_bus("SD Card", kSharedBus, kSdSclk, kSdMiso, kSdMosi);

  EXPECT_FALSE(conflict_raised());
}

// Different controllers are the answer, not the problem - the same wiring on
// two buses is impossible in hardware, but the guard must key on the bus.
TEST_F(SpiBusClaimFixture, TheSameDeviceShapeOnTwoControllersIsFine) {
  esp32hal->claim_spi_bus("first device", kSharedBus, kCanSck, kCanMiso, kCanMosi);
  esp32hal->claim_spi_bus("second device", kOtherBus, kSdSclk, kSdMiso, kSdMosi);

  EXPECT_FALSE(conflict_raised());
}

/* The message has to name both devices to be worth raising: "something on your
 * SPI bus conflicts" would leave the user exactly where the silent failure did.
 * The device that loses the bus is the one already holding it. */
TEST_F(SpiBusClaimFixture, TheMessageNamesBothDevices) {
  esp32hal->claim_spi_bus("SD Card", kSharedBus, kSdSclk, kSdMiso, kSdMosi);
  esp32hal->claim_spi_bus("CAN", kSharedBus, kCanSck, kCanMiso, kCanMosi);

  const std::string message = get_event_message_string(EVENT_SPI_BUS_CONFLICT).str();
  EXPECT_NE(message.find("SD Card"), std::string::npos) << message;
  EXPECT_NE(message.find("CAN"), std::string::npos) << message;
}

/* R256. The message is re-rendered every time the event is published - the
 * events page, MQTT (mqtt.cpp) and ESP-NOW (espnow.cpp) all call
 * get_event_message_string() - so it must not read names that anything else can
 * overwrite in the meantime.
 *
 * It did: the SPI conflict shared allocator_name/allocated_name with
 * alloc_pins(), so any later GPIO failure re-pointed BOTH names and the message
 * went on to report a pair of devices that share no bus at all -
 * "'Shunt' shares an SPI controller with 'Charger'". That is worse than saying
 * nothing, because the event exists precisely to name the two devices.
 *
 * It matters here more than for the GPIO events because this one is PERSISTENT:
 * on an affected board it is raised on every boot and re-rendered for the life
 * of the board, with plenty of opportunity for something else to fail an
 * allocation in between. */
TEST_F(SpiBusClaimFixture, TheMessageStillNamesTheRightPairAfterALaterAllocationFailure) {
  esp32hal->claim_spi_bus("SD Card", kSharedBus, kSdSclk, kSdMiso, kSdMosi);
  esp32hal->claim_spi_bus("CAN", kSharedBus, kCanSck, kCanMiso, kCanMosi);
  ASSERT_TRUE(conflict_raised());

  // Something else now fails a GPIO allocation, as a misconfigured board does.
  esp32hal->alloc_pins("Charger", GPIO_NUM_18);
  esp32hal->alloc_pins("Shunt", GPIO_NUM_18);  // conflict: rewrites the gpio names

  const std::string message = get_event_message_string(EVENT_SPI_BUS_CONFLICT).str();
  EXPECT_NE(message.find("SD Card"), std::string::npos) << message;
  EXPECT_NE(message.find("CAN"), std::string::npos) << message;
  // And it must not have picked up the GPIO failure's devices instead.
  EXPECT_EQ(message.find("Shunt"), std::string::npos) << message;
  EXPECT_EQ(message.find("Charger"), std::string::npos) << message;
}

/* WARNING, not ERROR, and this is a behaviour claim rather than a taste one:
 * update_bms_status() turns any active error-level event into system_status =
 * FAULT. A logging device losing its SPI routing must not fault the emulator. */
TEST_F(SpiBusClaimFixture, TheConflictWarnsRatherThanFaultingTheEmulator) {
  esp32hal->claim_spi_bus("SD Card", kSharedBus, kSdSclk, kSdMiso, kSdMosi);
  esp32hal->claim_spi_bus("CAN", kSharedBus, kCanSck, kCanMiso, kCanMosi);
  ASSERT_TRUE(conflict_raised());

  EXPECT_EQ(get_event_level(), EVENT_LEVEL_WARNING);
  EXPECT_NE(datalayer.system.status.system_status, FAULT);
}

}  // namespace
