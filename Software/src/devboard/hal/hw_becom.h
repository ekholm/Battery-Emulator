#ifndef __HW_BECOM_H__
#define __HW_BECOM_H__

#include "hal.h"

class BEComHal : public Esp32Hal {
 public:
  // ---- BEGIN GENERATED from Software/boards/becom.yaml ----
  // Rebuild with tools/board_gen.py; CI fails if this block and the
  // declaration disagree. Getters below the block are hand-written.

  const char* name() { return "BECom"; }

  // RS485
  virtual gpio_num_t RS485_TX_PIN() { return GPIO_NUM_15; }
  virtual gpio_num_t RS485_RX_PIN() { return GPIO_NUM_17; }
  virtual gpio_num_t RS485_DE_PIN() { return GPIO_NUM_16; }

  // CAN interfaces
  virtual gpio_num_t CAN_TX_PIN() { return GPIO_NUM_8; }
  virtual gpio_num_t CAN_RX_PIN() { return GPIO_NUM_18; }
  virtual gpio_num_t MCP2517_SCK() { return GPIO_NUM_12; }
  virtual gpio_num_t MCP2517_SDI() { return GPIO_NUM_11; }
  virtual gpio_num_t MCP2517_SDO() { return GPIO_NUM_13; }
  virtual gpio_num_t MCP2517_CS() { return GPIO_NUM_14; }
  virtual gpio_num_t MCP2517_INT() { return GPIO_NUM_10; }
  virtual uint32_t MCP2517_FREQ() { return 40000000; }
  virtual int MCP2517_CLKODIV() { return 0b00; }
  virtual gpio_num_t MCP2517_CS2() { return GPIO_NUM_21; }
  virtual gpio_num_t MCP2517_INT2() { return GPIO_NUM_9; }
  virtual uint32_t MCP2517_FREQ2() { return 40000000; }

  // Contactor handling
  virtual gpio_num_t POSITIVE_CONTACTOR_PIN() { return GPIO_NUM_47; }
  virtual gpio_num_t NEGATIVE_CONTACTOR_PIN() { return GPIO_NUM_48; }
  virtual gpio_num_t PRECHARGE_PIN() { return GPIO_NUM_45; }
  virtual gpio_num_t BMS_POWER() { return GPIO_NUM_1; }
  virtual gpio_num_t SECOND_BATTERY_CONTACTORS_PIN() { return GPIO_NUM_37; }

  // Automatic precharging
  virtual gpio_num_t HIA4V1_PIN() { return GPIO_NUM_45; }
  virtual gpio_num_t INVERTER_DISCONNECT_CONTACTOR_PIN() { return GPIO_NUM_47; }

  // SMA CAN contactor pins
  virtual gpio_num_t INVERTER_CONTACTOR_ENABLE_PIN() { return GPIO_NUM_3; }

  // LED
  virtual gpio_num_t LED_PIN() { return GPIO_NUM_5; }
  virtual uint8_t LED_MAX_BRIGHTNESS() { return 40; }

  // Equipment stop pin
  virtual gpio_num_t EQUIPMENT_STOP_PIN() { return GPIO_NUM_46; }

  // Battery wake up pins
  virtual gpio_num_t WUP_PIN1() { return GPIO_NUM_2; }
  virtual gpio_num_t WUP_PIN2() { return GPIO_NUM_41; }

  // Wi-Fi AP button
  virtual gpio_num_t AP_BUTTON_PIN() { return GPIO_NUM_0; }
  // ---- END GENERATED ----

  // Pins to be latched across a reset/OTA reboot (RTC-capable pins only): BMS_POWER is GPIO1
  virtual std::vector<gpio_num_t> reset_hold_pins() { return {GPIO_NUM_1}; }

  std::vector<comm_interface> available_interfaces() {
    return {comm_interface::Modbus, comm_interface::RS485, comm_interface::CanNative, comm_interface::CanFdAddonMcp2518,
            comm_interface::CanFdAddonMcp2518_2};
  }

  virtual const char* name_for_comm_interface(comm_interface comm) {
    switch (comm) {
      case comm_interface::CanNative:
        return "Inverter CAN (Native)";
      case comm_interface::CanFdNative:
        return "";
      case comm_interface::CanAddonMcp2515:
        return "";
      case comm_interface::CanFdAddonMcp2518:
        return "CAN FD Battery 1 (MCP2518)";
      case comm_interface::CanFdAddonMcp2518_2:
        return "CAN FD Battery 2 (MCP2518)";
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

#define HalClass BEComHal

/* ----- Error checks below, don't change (can't be moved to separate file) ----- */
#ifndef HW_CONFIGURED
#define HW_CONFIGURED
#else
#error Multiple HW defined! Please select a single HW
#endif

#endif
