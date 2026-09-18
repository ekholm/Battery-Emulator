#include "CanBattery.h"

CanBattery::CanBattery(CAN_Speed speed) : CanBattery(can_config.battery, speed) {}

CanBattery::CanBattery(CAN_Interface interface, CAN_Speed speed) {
  can_interface = interface;
  initial_speed = speed;
  register_transmitter(this);
  register_can_receiver(this, can_interface, driver_name(), speed);
}

/* The battery's own name, for events raised about the interface it shares.
   Battery has no name() of its own, and this runs from the constructor, where a
   virtual call would not reach the driver anyway - so it goes through the type,
   which is what selected this driver in the first place. */
const char* CanBattery::driver_name() {
  return name_for_battery_type(user_selected_battery_type);
}

bool CanBattery::change_can_speed(CAN_Speed speed) {
  return ::change_can_speed(can_interface, speed, this, driver_name());
}

void CanBattery::reset_can_speed() {
  ::change_can_speed(can_interface, initial_speed, this, driver_name());
}
