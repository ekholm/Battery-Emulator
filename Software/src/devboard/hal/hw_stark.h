#ifndef __HW_STARK_H__
#define __HW_STARK_H__

#include "hal.h"

/*
Stark CMR v1 - DIN-rail module with 4 power outputs, 1 x rs485, 1 x can and 1 x can-fd channel.
For more information on this board visit the project discord or contact johan@redispose.se

GPIOs on extra header
* GPIO  2
* GPIO 17 (only available if can channel 2 is inactive)
* GPIO 19
* GPIO 14 (JTAG TMS)
* GPIO 12 (JTAG TDI)
* GPIO 13 (JTAG TCK)
* GPIO 15 (JTAG TDO)
*/

class StarkHal : public Esp32Hal {
 public:
  const char* name() { return "Stark CMR Module"; }

  //Always enable BMS power on Stark CMR, it does not collide with any pin definitions
  virtual bool always_enable_bms_power() { return true; }

  // Not needed, GPIO 16 has hardware pullup for PSRAM compatibility
  virtual gpio_num_t PIN_5V_EN() { return GPIO_NUM_NC; }

  // Not needed, GPIO 17 is used as SCK input of MCP2517
  virtual gpio_num_t RS485_EN_PIN() { return GPIO_NUM_NC; }
  virtual gpio_num_t RS485_TX_PIN() { return GPIO_NUM_22; }
  virtual gpio_num_t RS485_RX_PIN() { return GPIO_NUM_21; }
  // Not needed, GPIO 19 is available as extra GPIO via pin header
  virtual gpio_num_t RS485_SE_PIN() { return GPIO_NUM_NC; }

  virtual gpio_num_t CAN_TX_PIN() { return GPIO_NUM_27; }
  virtual gpio_num_t CAN_RX_PIN() { return GPIO_NUM_26; }

  // (No function, GPIO 23 used instead as MCP_SCK)
  virtual gpio_num_t CAN_SE_PIN() { return GPIO_NUM_NC; }

  // CANFD_ADDON defines for MCP2517
  // Stark CMR v1 has GPIO pin 16 for SCK, CMR v2 has GPIO pin 17. Only diff between the two boards
  bool isStarkVersion1() {
    size_t flashSize = ESP.getFlashChipSize();
    if (flashSize == 4 * 1024 * 1024) {
      return true;
    } else {  //v2
      return false;
    }
  }
  virtual gpio_num_t MCP2517_SCK() { return isStarkVersion1() ? GPIO_NUM_16 : GPIO_NUM_17; }
  virtual gpio_num_t MCP2517_SDI() { return GPIO_NUM_5; }
  virtual gpio_num_t MCP2517_SDO() { return GPIO_NUM_34; }
  virtual gpio_num_t MCP2517_CS() { return GPIO_NUM_18; }
  virtual gpio_num_t MCP2517_INT() { return GPIO_NUM_35; }
  virtual uint32_t MCP2517_FREQ() { return 40000000; }

  // No second MCP2518FD is fitted on this board. The declaration used to name CS=GPIO12 /
  // INT=GPIO14 and the settings page OFFERED it, so selecting it drove a chip select at pins
  // where nothing answers - "autodetected crystal: 0MHz" then "CAN-FD 2 Configuration error
  // 0x1", reproduced on silicon. Falling back to the NC defaults in hal.h removes the phantom
  // and frees GPIO12.

  // Contactor handling
  virtual gpio_num_t POSITIVE_CONTACTOR_PIN() { return GPIO_NUM_32; }
  virtual gpio_num_t NEGATIVE_CONTACTOR_PIN() { return GPIO_NUM_33; }
  virtual gpio_num_t PRECHARGE_PIN() {  //Precharge and BMS power pins can be swapped in config
    if (user_selected_gpioopt5 == GPIOOPT5::BMS_POWER_25) {
      return GPIO_NUM_23;
    }
    return GPIO_NUM_25;
  }
  // Pins to be latched across a reset/OTA reboot (RTC-capable pins only): BMS_POWER can be GPIO25
  virtual std::vector<gpio_num_t> reset_hold_pins() { return {GPIO_NUM_25}; }

  virtual gpio_num_t SECOND_BATTERY_CONTACTORS_PIN() { return GPIO_NUM_19; }
  virtual gpio_num_t TRIPLE_BATTERY_CONTACTORS_PIN() { return GPIO_NUM_15; }
  virtual gpio_num_t BMS_POWER() {
    if (user_selected_gpioopt5 == GPIOOPT5::BMS_POWER_25) {
      return GPIO_NUM_25;
    }
    return GPIO_NUM_23;
  }

  // Automatic precharging
  virtual gpio_num_t HIA4V1_PIN() { return GPIO_NUM_19; }
  virtual gpio_num_t INVERTER_DISCONNECT_CONTACTOR_PIN() { return GPIO_NUM_25; }

  // SMA CAN contactor pins
  virtual gpio_num_t INVERTER_CONTACTOR_ENABLE_PIN() { return GPIO_NUM_2; }

  // LED
  virtual gpio_num_t LED_PIN() { return GPIO_NUM_4; }
  virtual uint8_t LED_MAX_BRIGHTNESS() { return 20; }
  // LEDs 1-4 (PRECHARGE, CONTACTOR NEG, CONTACTOR POS, BMS POWER) are chained off the STATUS LED
  // (pixel 0). On boards with the older plain hardwired LEDs there's no physical RGB LED at those
  // positions, so this extra chain data has nowhere to go and is simply a no-op; on boards with
  // the RGB LED PCB it lights them. No user-facing option needed either way.
  virtual uint8_t LED_COUNT() { return 5; }

  // Equipment stop pin
  virtual gpio_num_t EQUIPMENT_STOP_PIN() { return GPIO_NUM_2; }

  // Battery wake up pins
  virtual gpio_num_t WUP_PIN1() { return GPIO_NUM_25; }
  virtual gpio_num_t WUP_PIN2() { return GPIO_NUM_32; }

  // the FLA momentary push-button that can be long-pressed at runtime to start the Wi-Fi AP if not running
  virtual gpio_num_t AP_BUTTON_PIN() { return GPIO_NUM_0; }

  std::vector<comm_interface> available_interfaces() {
    /* No MCP2515: this board routes no chip select for one - MCP2515_CS() is
       the base class's GPIO_NUM_NC - and "available" here means the chip
       select is routed, which is what lets a user fit the module. Declaring
       it anyway was not cosmetic, and what it costs is NOT the "autodetected
       crystal: 0MHz" failure the phantom second MCP2518FD above produced:
       that one has its chip select ROUTED, at pins where no chip answers. An
       UNROUTED one never reaches a chip at all. alloc_pins() rejects any pin
       below zero before anything is driven, so a stored selection of this
       interface passes the availability guard, raises EVENT_GPIO_NOT_DEFINED
       and returns false out of init_CAN() - from a block that sits ABOVE the
       MCP2518FD blocks, so it takes this board's FD interfaces down with it.
       That is the failure the guard at the top of init_CAN() was rewritten to
       stop, reappearing one block below where its erase-and-continue can
       reach. The empty name below only hid it from the dropdown; a value
       already in NVS never goes through the dropdown. */
    return {comm_interface::Modbus, comm_interface::RS485, comm_interface::CanNative, comm_interface::CanFdNative,
            comm_interface::CanFdAddonMcp2518};
  }

  virtual const char* name_for_comm_interface(comm_interface comm) {
    switch (comm) {
      case comm_interface::CanNative:
        return "CAN 1 (Native)";
      case comm_interface::CanFdNative:
        return "CAN FD 2 (Native)";
      /* Deliberately NOT overridden to "": the base name is correct, and a
         board that does not declare an interface should still NAME it, so a
         stale stored selection renders as itself with the page's "(not
         available on this board)" suffix instead of vanishing. Hiding it is
         how this one stayed wrong. */
      case comm_interface::CanFdAddonMcp2518:
        return "CAN FD (MCP2518FD add-on)";
      case comm_interface::CanFdAddonMcp2518_2:
        return "";
      case comm_interface::Modbus:
        return "Modbus";
      case comm_interface::RS485:
        return "RS485";
      case comm_interface::Highest:
        return "";
      default:
        return Esp32Hal::name_for_comm_interface(comm);
    }
  }
};

#endif  // __HW_STARK_H__
