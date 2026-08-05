#include "Battery.h"
#include "../datalayer/datalayer.h"

float Battery::get_voltage() {
  return static_cast<float>(datalayer_battery->status.voltage_dV) / 10.0f;
}
