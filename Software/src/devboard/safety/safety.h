#ifndef SAFETY_H
#define SAFETY_H
#include <stdint.h>
#include <string>
#include "../utils/types.h"

#define MAX_CAN_FAILURES 50
#define BATTERY_MAX_TEMPERATURE_DEVIATION 150  // 150 = 15.0 °C
#define BATTERY_MAXTEMPERATURE 500
#define BATTERY_MINTEMPERATURE -250
#define MAX_CHARGE_DISCHARGE_LIMIT_FAILURES 5

//battery pause status begin
enum battery_pause_status { NORMAL = 0, PAUSING = 1, PAUSED = 2, RESUMING = 3 };
extern bool emulator_pause_request_ON;
extern bool emulator_pause_CAN_send_ON;
extern battery_pause_status emulator_pause_status;
extern bool allowed_to_send_CAN;
//battery pause status end

extern void store_settings_equipment_stop();

typedef enum : uint8_t { UNCHANGED = 0, STOP = 1, RESUME = 2 } EquipmentStop;

/* The safety layer's own mutable state, gathered into one plain struct.
 *
 * It used to live as file-scope and function-local statics in safety.cpp.
 * That is invisible to the unit tests, which run every case in one process:
 * a latch set by one test (a tripped cell-overvoltage block, a fired
 * battery-full gate, a non-zero hysteresis counter) survived into the next
 * one and quietly changed its preconditions, so the suite was order
 * dependent.
 *
 * Data only, no methods - the same shape the datalayer types use, with the
 * behaviour staying in the free functions below. Gathering the variables is
 * the whole point: one thing to clear, and one place to see what the layer
 * actually remembers between ticks.
 */
struct SafetyState {
  // Event gates - one-shot latches so a condition logs once, not every tick
  bool battery_full_event_fired = false;
  bool battery_empty_event_fired = false;

  // Per-instance protection latches and counters
  bool cell_overvoltage_charge_blocked[MAX_BATTERIES] = {};
  bool charge_blocked[MAX_BATTERIES] = {};
  bool discharge_blocked[MAX_BATTERIES] = {};
  uint8_t charge_limit_failures[MAX_BATTERIES] = {};
  uint8_t discharge_limit_failures[MAX_BATTERIES] = {};

  // Component liveness: has a charger/inverter ever been seen on CAN
  bool charger_detected = false;
  bool inverter_detected = false;
  uint8_t slow_start_counter = 0;

  // Housekeeping
  uint8_t hysteresis_heap_seconds = 0;
  uint32_t emulator_restart_request_millis = 0;
};

extern SafetyState safety;

#ifdef UNIT_TEST
/* Test-only. Restores every latch, counter and detection flag the safety layer
   carries between ticks - including the parallel-join drift timers - so a test
   starts from a known point. Not compiled into firmware: the emulator boots
   once and nothing legitimately resets the safety layer at run time. */
void reset_safety_state();
#endif

void update_machineryprotection();
void update_remote_limit_expiry(uint32_t currentMillis);
void graceful_restart();
void update_restart_progress();

//battery pause status begin
void setBatteryPause(bool pause_battery, bool pause_CAN, EquipmentStop equipment_stop = UNCHANGED,
                     bool store_settings = true);
void update_pause_state();
std::string get_emulator_pause_status();
//battery pause status end

#endif
