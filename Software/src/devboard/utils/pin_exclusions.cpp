#include "pin_exclusions.h"

bool inverter_uses_sma_contactor_pin(InverterProtocolType type) {
  switch (type) {
    case InverterProtocolType::SmaBydH:
    case InverterProtocolType::SmaBydHvs:
    case InverterProtocolType::SmaSBSByd:
      return true;
    default:
      return false;
  }
}

const char* pin_exclusion_conflict(InverterProtocolType inverter, STOP_BUTTON_BEHAVIOR equipment_stop,
                                   gpio_num_t sma_contactor_pin, gpio_num_t equipment_stop_pin) {
  const bool sma_active = inverter_uses_sma_contactor_pin(inverter);
  const bool stop_active = equipment_stop != STOP_BUTTON_BEHAVIOR::NOT_CONNECTED;
  // GPIO_NUM_NC marks "this board has no such pin": nothing is driven, so two
  // NC roles never collide.
  const bool same_real_pin = sma_contactor_pin == equipment_stop_pin && sma_contactor_pin != GPIO_NUM_NC;
  if (sma_active && stop_active && same_real_pin) {
    return "This board wires the SMA inverter's contactor enable and the equipment stop button to the same pin. "
           "They cannot be enabled together: disable the equipment stop button, or select a non-SMA inverter.";
  }
  return nullptr;
}

const char* pin_exclusion_conflict(InverterProtocolType inverter, STOP_BUTTON_BEHAVIOR equipment_stop) {
  return pin_exclusion_conflict(inverter, equipment_stop, esp32hal->INVERTER_CONTACTOR_ENABLE_PIN(),
                                esp32hal->EQUIPMENT_STOP_PIN());
}
