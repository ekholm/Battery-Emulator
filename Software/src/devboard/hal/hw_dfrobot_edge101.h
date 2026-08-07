#ifndef __HW_DFROBOT_EDGE101_H__
#define __HW_DFROBOT_EDGE101_H__

#include "hal.h"

/*
DFRobot Edge101 IOT Controller (SKU DFR0886)
ESP32-WROOM-32E, 16 MB flash, no PSRAM. USB-serial via CH9102F.
Isolated CAN (TJA1050), isolated RS485 (TPT75176H), microSD (SPI),
IP101GRI Ethernet PHY (RMII), PCF8563T RTC (I2C 0x51).
Pin map: see arduino-esp32 PR #12742 variants/dfrobot_edge101/pins_arduino.h.

The RS485 transceiver has DE and /RE tied together, so one pin drives both.
The AP button is on GPIO 38, which is input-only on the ESP32 and has no
internal pull-up; the board provides an external one.
*/

class DFRobotEdge101Hal : public Esp32Hal {
 public:
  // ---- BEGIN GENERATED from Software/boards/dfrobot_edge101.yaml ----
  // Rebuild with tools/board_gen.py; CI fails if this block and the
  // declaration disagree. Getters below the block are hand-written.

  const char* name() { return "DFRobot Edge101 IOT Controller"; }

  // RS485
  virtual gpio_num_t RS485_TX_PIN() { return GPIO_NUM_17; }
  virtual gpio_num_t RS485_RX_PIN() { return GPIO_NUM_36; }
  virtual gpio_num_t RS485_DE_PIN() { return GPIO_NUM_16; }

  // CAN interfaces
  virtual gpio_num_t CAN_TX_PIN() { return GPIO_NUM_32; }
  virtual gpio_num_t CAN_RX_PIN() { return GPIO_NUM_35; }

  // LED
  virtual gpio_num_t LED_PIN() { return GPIO_NUM_15; }

  // Wi-Fi AP button
  virtual gpio_num_t AP_BUTTON_PIN() { return GPIO_NUM_38; }
  // ---- END GENERATED ----

  /* Hand-written: this board's card is on SPI, so the group publishes
     SD_SPI_BUS() and sits behind #ifdef SDCARD. The sd_mmc feature has
     no field for either, so declaring it would emit different code. */
  // microSD (SPI)
#ifdef SDCARD
  uint8_t SD_SPI_BUS() override { return VSPI; }
  virtual gpio_num_t SD_MOSI_PIN() { return GPIO_NUM_12; }
  virtual gpio_num_t SD_MISO_PIN() { return GPIO_NUM_39; }
  virtual gpio_num_t SD_SCLK_PIN() { return GPIO_NUM_14; }
  virtual gpio_num_t SD_CS_PIN() { return GPIO_NUM_5; }
#endif  // SDCARD

  std::vector<comm_interface> available_interfaces() {
    return {comm_interface::Modbus, comm_interface::RS485, comm_interface::CanNative};
  }

  virtual const char* name_for_comm_interface(comm_interface comm) {
    switch (comm) {
      case comm_interface::CanNative:
        return "CAN (Native)";
      case comm_interface::CanFdNative:
        return "";
      case comm_interface::CanAddonMcp2515:
        return "CAN (MCP2515 add-on)";
      case comm_interface::CanFdAddonMcp2518:
        return "CAN FD (MCP2518 add-on)";
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

#define HalClass DFRobotEdge101Hal

/* ----- Error checks below, don't change (can't be moved to separate file) ----- */
#ifndef HW_CONFIGURED
#define HW_CONFIGURED
#else
#error Multiple HW defined! Please select a single HW
#endif

#endif
