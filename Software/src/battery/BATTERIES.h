#ifndef BATTERIES_H
#define BATTERIES_H

#include "../datalayer/datalayer.h"
#include "../shunt/Shunt.h"
#include "Battery.h"

// Currently initialized battery instances, primary first. A null entry means
// that instance is not configured/initialized. The array is the source of
// truth; battery/battery2/battery3 are aliases for the existing call sites.
// MAX_BATTERIES lives in datalayer.h beside the datalayer instance array.
extern Battery* batteries[MAX_BATTERIES];

// Primary-instance alias: singleton readers keep their idiom; everything
// per-instance uses batteries[i]. The battery2/battery3 aliases are GONE -
// no code names the secondary instances anymore.

void setup_shunt();

void setup_battery(void);
Battery* create_battery(BatteryType type, int instance = 0);

// Returns true if the given battery integration can be instantiated a second
// resp. third time to run batteries in parallel. Support is opt-OUT: any
// stateless driver runs multiple instances; the exceptions (singleton
// hardware, single transport, pending de-static work) are listed with their
// reasons in battery_supports_extra_instances().
bool battery_supports_double(BatteryType type);
bool battery_supports_triple(BatteryType type);

extern uint16_t user_selected_max_pack_voltage_dV;
extern uint16_t user_selected_min_pack_voltage_dV;
extern uint16_t user_selected_max_cell_voltage_mV;
extern uint16_t user_selected_min_cell_voltage_mV;
extern bool user_selected_use_estimated_SOC;
extern bool user_selected_use_estimated_charge_limits;
extern bool user_selected_LEAF_interlock_mandatory;
extern bool user_selected_tesla_digital_HVIL;
extern uint16_t user_selected_tesla_GTW_country;
extern bool user_selected_tesla_GTW_rightHandDrive;
extern uint16_t user_selected_tesla_GTW_mapRegion;
extern uint16_t user_selected_tesla_GTW_chassisType;
extern uint16_t user_selected_tesla_GTW_packEnergy;
extern uint16_t user_selected_pylon_baudrate;

/* User-selected DALY BMS settings */
extern int user_selected_daly_power_per_percent;
extern int user_selected_daly_power_per_dV;
extern int user_selected_daly_power_per_dV_start;
extern int user_selected_daly_power_per_degree_C;
extern int user_selected_daly_power_at_0_degree_C;

#endif
