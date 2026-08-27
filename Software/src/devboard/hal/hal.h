#ifndef _HAL_H_
#define _HAL_H_

#include <SPI.h>
#include <soc/gpio_num.h>
#include <chrono>
#include <unordered_map>
#include "../../../src/communication/nvm/comm_nvm.h"
#include "../../../src/devboard/utils/events.h"
#include "../../../src/devboard/utils/logging.h"
#include "../../../src/devboard/utils/types.h"

// We need to use #ifdef trickery since the SPI buses are only defined on the
// respective architectures.
// The spare ESP32 SPI buses are called HSPI and VSPI, whereas on a ESP32S3
// they are called FSPI and HSPI.
#ifdef CONFIG_IDF_TARGET_ESP32S3
#define DEFAULT_MCP2515_BUS HSPI
#define DEFAULT_MCP2517_BUS FSPI
#else
#define DEFAULT_MCP2515_BUS VSPI
#define DEFAULT_MCP2517_BUS HSPI
#endif

// Hardware Abstraction Layer base class.
// Derive a class to define board-specific parameters such as GPIO pin numbers
// This base class implements a mechanism for allocating GPIOs.
class Esp32Hal {
 public:
  virtual const char* name() = 0;

  // Time it takes before system is considered fully started up.
  virtual duration BOOTUP_TIME() { return milliseconds(1000); }
  virtual bool system_booted_up();

  // Core assignment
  virtual int CORE_FUNCTION_CORE() { return 1; }
  virtual int MODBUS_CORE() { return 0; }
  virtual int WIFICORE() { return 0; }

  virtual void set_default_configuration_values() {}

  template <typename... Pins>
  bool alloc_pins(const char* name, Pins... pins) {
    std::vector<gpio_num_t> requested_pins = {static_cast<gpio_num_t>(pins)...};

    for (gpio_num_t pin : requested_pins) {
      if (pin < 0) {
        // Must be set BEFORE set_event(): set_event() logs the event message immediately,
        // and get_event_message_string() reads it back via failed_allocator().
        allocator_name = name;
        set_event(EVENT_GPIO_NOT_DEFINED, (int)pin);  // also printing a log entry
        return false;
      }

      auto it = allocated_pins.find(pin);
      if (it != allocated_pins.end()) {
        allocator_name = name;
        allocated_name = it->second.c_str();
        set_event(EVENT_GPIO_CONFLICT, (int)pin);  // also printing a log entry
        return false;
      }
    }

    for (gpio_num_t pin : requested_pins) {
      allocated_pins[pin] = name;
    }

    return true;
  }

  /* Record which device is using an SPI controller, and complain if another one
   * already had it with different wiring.
   *
   * alloc_pins() above cannot see this collision. It compares pin NUMBERS, so
   * two devices on one controller with disjoint pins pass it silently - and
   * then an ESP32 SPI controller sources its MISO input from exactly one GPIO.
   * SPIClass::begin() re-points that input with no way to refuse, so the second
   * begin() takes the bus and the first device stops receiving, with nothing
   * logged anywhere. Measured on a T-CAN485 where the SD card and the MCP2515
   * add-on both defaulted to VSPI: the SD mounted at 115 ms, the MCP2515 began
   * at 1.15 s, and every later log write failed silently.
   *
   * Sharing a controller with the SAME sck/miso/mosi is ordinary SPI practice -
   * several chip selects on one bus - and is not reported. Only a DIFFERENT
   * triple on an already-claimed controller steals the routing.
   *
   * Deliberately NOT a veto, and the reason is worth keeping: on the boards
   * this fires for today it is the SECOND begin() that ends up working, so
   * refusing it would trade a deaf SD card for a dead CAN interface. The event
   * is a warning rather than an error for the same reason - an error would put
   * the emulator in FAULT (see update_bms_status), which is not a proportionate
   * response to a logging device losing its bus. The job here is to make the
   * collision visible; deciding which device should win is a board's business.
   *
   * Pass GPIO_NUM_NC for a pin the device does not use; a claim whose three
   * pins are all unset is ignored, since it says nothing about routing. */
  void claim_spi_bus(const char* name, uint8_t bus, gpio_num_t sck, gpio_num_t miso, gpio_num_t mosi);

  // Helper to forward vector to variadic template
  template <typename Vec, size_t... Is>
  bool alloc_pins_from_vector(const char* name, const Vec& pins, std::index_sequence<Is...>) {
    return alloc_pins(name, pins[Is]...);
  }

  // Base case: no more pins
  inline bool alloc_pins_ignore_unused_impl(const char* name) {
    return alloc_pins(name);  // Call with 0 pins
  }

  // Recursive case: process one pin at a time
  template <typename... Rest>
  bool alloc_pins_ignore_unused_impl(const char* name, gpio_num_t first, Rest... rest) {
    if (first == GPIO_NUM_NC) {
      return alloc_pins_ignore_unused_impl(name, rest...);
    } else {
      return call_alloc_pins_filtered(name, first, rest...);
    }
  }

  // This helper just forwards pins after filtering is done
  template <typename... Pins>
  bool call_alloc_pins_filtered(const char* name, Pins... pins) {
    return alloc_pins(name, pins...);
  }

  // Entry point
  template <typename... Pins>
  bool alloc_pins_ignore_unused(const char* name, Pins... pins) {
    return alloc_pins_ignore_unused_impl(name, static_cast<gpio_num_t>(pins)...);
  }

  virtual bool always_enable_bms_power() { return false; }

  virtual gpio_num_t PIN_5V_EN() { return GPIO_NUM_NC; }
  virtual gpio_num_t RS485_EN_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t RS485_TX_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t RS485_RX_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t RS485_SE_PIN() { return GPIO_NUM_NC; }

  // Direction pin for half-duplex RS485 transceivers with DE and /RE tied together.
  // Default polarity: HIGH = transmit, LOW = receive.
  virtual gpio_num_t RS485_DE_PIN() { return GPIO_NUM_NC; }
  virtual bool RS485_DE_ACTIVE_HIGH() { return true; }

  virtual gpio_num_t CAN_TX_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t CAN_RX_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t CAN_SE_PIN() { return GPIO_NUM_NC; }

  // CAN_ADDON
  // SPI bus number of MCP2515
  virtual uint8_t MCP2515_BUS() { return DEFAULT_MCP2515_BUS; }
  // SCK input of MCP2515
  virtual gpio_num_t MCP2515_SCK() { return GPIO_NUM_NC; }
  // SDI input of MCP2515
  virtual gpio_num_t MCP2515_MOSI() { return GPIO_NUM_NC; }
  // SDO output of MCP2515
  virtual gpio_num_t MCP2515_MISO() { return GPIO_NUM_NC; }
  // CS input of MCP2515
  virtual gpio_num_t MCP2515_CS() { return GPIO_NUM_NC; }
  // INT output of MCP2515
  virtual gpio_num_t MCP2515_INT() { return GPIO_NUM_NC; }
  // Reset pin for MCP2515
  virtual gpio_num_t MCP2515_RST() { return GPIO_NUM_NC; }
  virtual uint32_t MCP2515_FREQ() { return 0; }  // 0 means unknown

  // CANFD_ADDON defines for MCP2517
  virtual uint8_t MCP2517_BUS() { return DEFAULT_MCP2517_BUS; }
  virtual gpio_num_t MCP2517_SCK() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_SDI() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_SDO() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_CS() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_INT() { return GPIO_NUM_NC; }
  virtual uint32_t MCP2517_FREQ() { return 0; }  // 0 means unknown

  // 2nd CANFD Interface: MCP2517/8
  virtual uint8_t MCP2517_BUS2() { return DEFAULT_MCP2517_BUS; }
  virtual gpio_num_t MCP2517_SCK2() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_SDI2() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_SDO2() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_CS2() { return GPIO_NUM_NC; }
  virtual gpio_num_t MCP2517_INT2() { return GPIO_NUM_NC; }
  virtual uint32_t MCP2517_FREQ2() { return 0; }  // 0 means unknown

  // Value for first MCP2517 CLKODIV register (default, divide by 10)
  virtual int MCP2517_CLKODIV() { return 0b11; }

  // CHAdeMO support pin dependencies
  virtual gpio_num_t CHADEMO_PIN_2() { return GPIO_NUM_NC; }
  virtual gpio_num_t CHADEMO_PIN_10() { return GPIO_NUM_NC; }
  virtual gpio_num_t CHADEMO_PIN_7() { return GPIO_NUM_NC; }
  virtual gpio_num_t CHADEMO_PIN_4() { return GPIO_NUM_NC; }
  virtual gpio_num_t CHADEMO_LOCK() { return GPIO_NUM_NC; }
  virtual gpio_num_t CHADEMO_CT_PIN() { return GPIO_NUM_NC; }

  // Contactor handling
  virtual gpio_num_t POSITIVE_CONTACTOR_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t NEGATIVE_CONTACTOR_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t PRECHARGE_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t BMS_POWER() { return GPIO_NUM_NC; }
  virtual gpio_num_t SECOND_BATTERY_CONTACTORS_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t TRIPLE_BATTERY_CONTACTORS_PIN() { return GPIO_NUM_NC; }

  // Output pins to latch at their driven level across a firmware-initiated reset/OTA
  // reboot, so they don't float during the boot window. RTC-capable pins only.
  virtual std::vector<gpio_num_t> reset_hold_pins() { return {}; }

  // Automatic precharging
  virtual gpio_num_t HIA4V1_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t INVERTER_DISCONNECT_CONTACTOR_PIN() { return GPIO_NUM_NC; }

  // SMA CAN contactor pins
  virtual gpio_num_t INVERTER_CONTACTOR_ENABLE_PIN() { return GPIO_NUM_NC; }

  virtual gpio_num_t INVERTER_CONTACTOR_ENABLE_LED_PIN() { return GPIO_NUM_NC; }

#ifdef SDCARD
  // SD card
  virtual uint8_t SD_SPI_BUS() = 0;
  virtual gpio_num_t SD_MISO_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t SD_MOSI_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t SD_SCLK_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t SD_CS_PIN() { return GPIO_NUM_NC; }
#endif  // SDCARD

  // LED
  virtual gpio_num_t LED_PIN() { return GPIO_NUM_NC; }
  virtual uint8_t LED_MAX_BRIGHTNESS() { return 40; }
  // Number of LEDs chained off LED_PIN(). Pixel 0 is always the STATUS LED; boards that replace
  // additional hardwired indicator LEDs with RGB LEDs on the same chain report more than 1 here.
  virtual uint8_t LED_COUNT() { return 1; }

#ifndef SMALL_FLASH_DEVICE
  // i2c display
  virtual gpio_num_t DISPLAY_SDA_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t DISPLAY_SCL_PIN() { return GPIO_NUM_NC; }
#endif  // SMALL_FLASH_DEVICE

  // Equipment stop pin
  virtual gpio_num_t EQUIPMENT_STOP_PIN() { return GPIO_NUM_NC; }

  // Battery wake up pins
  virtual gpio_num_t WUP_PIN1() { return GPIO_NUM_NC; }
  virtual gpio_num_t WUP_PIN2() { return GPIO_NUM_NC; }

  // Momentary push-button that can be long-pressed at runtime to start the Wi-Fi AP. Usually the BOOT button on GPIO0.
  virtual gpio_num_t AP_BUTTON_PIN() { return GPIO_NUM_NC; }

  // Returns the available comm interfaces on this HW
  virtual std::vector<comm_interface> available_interfaces() = 0;

  virtual const char* name_for_comm_interface(comm_interface comm) {
    switch (comm) {
      case comm_interface::Modbus:
        return "Modbus";
      case comm_interface::RS485:
        return "RS485";
      case comm_interface::CanNative:
        return "CAN (Native)";
      case comm_interface::CanFdNative:
        return "";
      case comm_interface::CanAddonMcp2515:
        return "CAN (MCP2515 add-on)";
      case comm_interface::CanFdAddonMcp2518:
        return "CAN FD (MCP2518 add-on)";
      case comm_interface::CanFdAddonMcp2518_2:
        return "";
      default:
        return nullptr;
    }
  }

  String failed_allocator() { return allocator_name; }
  String conflicting_allocator() { return allocated_name; }

  /* Named separately from the two above on purpose - see the fields. */
  String spi_conflict_claimant_name() { return spi_conflict_claimant; }
  String spi_conflict_holder_name() { return spi_conflict_holder; }

 private:
  std::unordered_map<gpio_num_t, std::string> allocated_pins;

  // Who holds an SPI controller, and with which routing. See claim_spi_bus().
  struct SpiBusClaim {
    std::string name;
    gpio_num_t sck;
    gpio_num_t miso;
    gpio_num_t mosi;
  };
  std::unordered_map<uint8_t, SpiBusClaim> claimed_spi_buses;

  // For event logging, store the name of the allocator/allocated
  // for failed gpio allocations.
  String allocator_name;
  String allocated_name;

  /* The SPI conflict keeps its OWN pair. get_event_message_string() is
   * re-rendered every time the event is published - the events page, MQTT and
   * ESP-NOW all call it - so a message that reads these back live names whoever
   * failed an allocation MOST RECENTLY, not the devices the event is about.
   * Sharing allocator_name with alloc_pins() made an SPI conflict report e.g.
   * "'Shunt' shares an SPI controller with 'Charger'" after any later GPIO
   * failure. This event is also persistent on the affected boards - it is
   * raised on every boot - so it gets re-rendered for the life of the board. */
  String spi_conflict_claimant;
  String spi_conflict_holder;
};

extern Esp32Hal* esp32hal;

// Needed for AsyncTCPSock library.
#define WIFI_CORE (esp32hal->WIFICORE())

void init_hal();

#endif
