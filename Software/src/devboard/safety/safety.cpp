#include "safety.h"
#include "../../battery/BATTERIES.h"
#include "../../charger/CHARGERS.h"
#include "../../datalayer/datalayer.h"
#include "../../devboard/utils/logging.h"
#include "../../inverter/INVERTERS.h"
#include "../utils/events.h"
#include "parallel_safety.h"

SafetyState safety;

#define MAX_SOH_DEVIATION_PPTT 2500
// Some inverters take a while to boot and start sending CAN. Suppress the
// inverter-missing error during this startup window (measured from power-on).
#define INVERTER_STARTUP_GRACE_MS 300000  // 300 s
#define CELL_CRITICAL_MV 100              // If cells go this much outside design voltage, shut battery down!
#define LOWEST_ALLOWED_CELLVOLTAGE_RECOVERY_CHARGE_MV 2000  //If cells are below this, recovery charge not allowed
#define MAX_CHARGEPOWER_RECOVERY_CHARGE_DA 50
#define HYSTERESIS_OFFSET_DV 20
#define CELL_HYSTERESIS_MV 20  // Re-allow charge only once max cell drops this far below limit (avoids chatter at knee)

//battery pause status begin
bool emulator_pause_request_ON = false;
bool emulator_pause_CAN_send_ON = false;
bool allowed_to_send_CAN = true;

//component detection

battery_pause_status emulator_pause_status = NORMAL;
//battery pause status end

// Shared CAN-aliveness handling for battery 1/2/3 and the charger: latch the
// detected-event on the first counter refresh, raise the missing-event when the
// counter runs out, decrement and clear it otherwise. The inverter has its own
// logic (long-timeout option, startup grace) and is handled separately.
static void check_can_component_alive(uint8_t& still_alive_counter, bool& detected, EVENTS_ENUM_TYPE detected_event,
                                      EVENTS_ENUM_TYPE missing_event, uint8_t missing_event_data) {
  if (!detected) {
    if (still_alive_counter >= CAN_STILL_ALIVE) {
      detected = true;
      set_event(detected_event, 1);
    }
  }
  if (!still_alive_counter) {
    set_event(missing_event, missing_event_data);
  } else {
    --still_alive_counter;
    clear_event(missing_event);
  }
}

/* Temperature limits are global, and so are the three events they raise. Every configured
   battery therefore has to be evaluated in one pass: a per-battery set/clear pair would let a
   healthy pack clear the event another pack just raised (same reasoning as the
   EVENT_CAN_CORRUPTED_WARNING block further down). The worst offender is the one reported.
   The battery number is only attached when more than one battery is configured, so
   single-battery systems keep their existing event text. */
static void check_battery_temperatures(void) {
  /* Each pack owns its own set of events now, so this is a plain per-pack set/clear with no
     cross-pack reasoning: battery 1 and battery 2 can be overheating at the same time and both
     are reported. That was impossible while the three events were shared. */
  for (uint8_t i = 0; i < MAX_BATTERIES; i++) {
    if (!batteries[i]) {
      continue;  // Pack not configured
    }
    const uint8_t number = i + 1;
    const int16_t max_dC = datalayer.batteries[i].status.temperature_max_dC;
    const int16_t min_dC = datalayer.batteries[i].status.temperature_min_dC;
    const int16_t deviation_dC = (int16_t)labs((long)max_dC - (long)min_dC);

    // Battery is overheated!
    if (max_dC > BATTERY_MAXTEMPERATURE) {
      set_event(EVENT_BATTERY_OVERHEAT, max_dC, number);
    } else {
      clear_event(EVENT_BATTERY_OVERHEAT, number);
    }

    // Battery is too cold to operate optimally
    if (min_dC < BATTERY_MINTEMPERATURE) {
      set_event(EVENT_BATTERY_FROZEN, min_dC, number);
    } else {
      clear_event(EVENT_BATTERY_FROZEN, number);
    }

    /* Not latched: the else branch below could never release a latched event, since
       clear_event() only acts on EVENT_STATE_ACTIVE. The warning now follows the pack. */
    if (deviation_dC > BATTERY_MAX_TEMPERATURE_DEVIATION) {
      set_event(EVENT_BATTERY_TEMP_DEVIATION_HIGH, deviation_dC, number);
    } else {
      clear_event(EVENT_BATTERY_TEMP_DEVIATION_HIGH, number);
    }
  }
}

/* Expire remote-set limits. Wrap-safe subtraction: the naive
   "currentMillis > timestamp + timeout" comparison both overflows near the 49.7-day
   millis() wrap (fresh limits expire immediately) and inverts after it (stale limits
   persist up to another 49.7 days) */
void update_remote_limit_expiry(uint32_t currentMillis) {
  if ((currentMillis - datalayer.batteries[0].settings.remote_set_timestamp) >
      datalayer.batteries[0].settings.remote_set_timeout) {
    datalayer.batteries[0].settings.remote_settings_limit_charge = false;
    datalayer.batteries[0].settings.remote_settings_limit_discharge = false;
    datalayer.batteries[0].settings.max_remote_set_charge_dA = 0;
    datalayer.batteries[0].settings.max_remote_set_discharge_dA = 0;
  }
}

// ---- Per-battery machinery protection --------------------------------------
// One helper runs the full protection suite for one instance. Events that are
// still SHARED cannot be set/cleared per instance (a clean battery would mask
// another's active fault - the documented CAN-corrupted/helper-can-alive
// pattern), so the helper only records verdicts;
// raise_machinery_protection_events() aggregates them. Power-limit zeroing and
// hysteresis latches stay per instance.
//
// The three temperature events are NOT here: upstream gave each pack its own
// triplet (#2799/#2850) and check_battery_temperatures() above raises them
// per pack, so aggregating them again would fight that path - the clean pack's
// clear_event(..., 1) would erase the worst offender the aggregate just set.
struct MachineryProtectionVerdicts {
  bool overvoltage = false;
  uint16_t overvoltage_dV = 0;
  bool undervoltage = false;
  uint16_t undervoltage_dV = 0;
  bool cell_over = false;
  bool cell_critical_over = false;
  bool cell_under = false;
  bool cell_critical_under = false;
  bool full = false;
  bool empty = false;
  bool soh_low = false;
  uint16_t soh_low_pptt = 0;
  bool soc_implausible = false;
  uint16_t soc_implausible_soc = 0;
  bool cell_deviation = false;
  uint16_t cell_deviation_mV = 0;
  bool charge_limit_exceeded = false;
  bool charge_limit_evaluated = false;
  bool discharge_limit_exceeded = false;
  bool discharge_limit_evaluated = false;
  bool soh_difference = false;
  bool soh_difference_evaluated = false;
};

static void check_battery_machinery_protection(int instance, MachineryProtectionVerdicts& v) {
  DATALAYER_BATTERY_TYPE& bat = datalayer_battery(instance);

  // Pause function is on OR we have a critical fault event active
  if (emulator_pause_request_ON || (datalayer.system.status.system_status == FAULT)) {
    bat.status.max_discharge_power_W = 0;
    bat.status.max_charge_power_W = 0;
  }

  // Temperature events are raised per pack in check_battery_temperatures().

  // Battery voltage is over designed max voltage!
  if (bat.status.voltage_dV > bat.info.max_design_voltage_dV) {
    if (!v.overvoltage || bat.status.voltage_dV > v.overvoltage_dV) {
      v.overvoltage_dV = bat.status.voltage_dV;
    }
    v.overvoltage = true;
    bat.status.max_charge_power_W = 0;
  }

  // Battery voltage is under designed min voltage!
  if (bat.status.voltage_dV < bat.info.min_design_voltage_dV) {
    if (!v.undervoltage || bat.status.voltage_dV < v.undervoltage_dV) {
      v.undervoltage_dV = bat.status.voltage_dV;
    }
    v.undervoltage = true;
    bat.status.max_discharge_power_W = 0;
  }

  // Cell overvoltage, further charging not possible. Battery might be imbalanced.
  if (bat.status.cell_max_voltage_mV >= bat.info.max_cell_voltage_mV) {
    v.cell_over = true;
    safety.cell_overvoltage_charge_blocked[instance] = true;  // Latch at the ceiling
  } else if (bat.status.cell_max_voltage_mV < (bat.info.max_cell_voltage_mV - CELL_HYSTERESIS_MV)) {
    safety.cell_overvoltage_charge_blocked[instance] = false;  // Release only once well below the ceiling
  }
  if (safety.cell_overvoltage_charge_blocked[instance]) {
    bat.status.max_charge_power_W = 0;
  }
  // Cell CRITICAL overvoltage, critical latching error without automatic reset. Requires user action to inspect battery.
  if (bat.status.cell_max_voltage_mV >= (bat.info.max_cell_voltage_mV + CELL_CRITICAL_MV)) {
    v.cell_critical_over = true;
  }

  // Cell undervoltage. Further discharge not possible. Battery might be imbalanced.
  if (bat.status.cell_min_voltage_mV <= bat.info.min_cell_voltage_mV) {
    v.cell_under = true;
    bat.status.max_discharge_power_W = 0;
  }
  //Cell CRITICAL undervoltage. critical latching error without automatic reset. Requires user action to inspect battery.
  if (bat.status.cell_min_voltage_mV <= (bat.info.min_cell_voltage_mV - CELL_CRITICAL_MV)) {
    v.cell_critical_under = true;
  }

  //If user is requesting charge to stop at a specific voltage
  if (bat.settings.user_set_voltage_limits_active) {
    // --- Charge limiting with hysteresis ---
    if (bat.status.voltage_dV >= bat.settings.max_user_set_charge_voltage_dV) {
      safety.charge_blocked[instance] = true;  // Latch: block charging once target is hit
    } else if (bat.status.voltage_dV < (bat.settings.max_user_set_charge_voltage_dV - HYSTERESIS_OFFSET_DV)) {
      safety.charge_blocked[instance] = false;  // Only release when voltage drops well below target
    }
    if (safety.charge_blocked[instance]) {
      bat.status.max_charge_power_W = 0;
    }

    // --- Discharge limiting with hysteresis ---
    if (bat.status.voltage_dV <= bat.settings.max_user_set_discharge_voltage_dV) {
      safety.discharge_blocked[instance] = true;
    } else if (bat.status.voltage_dV > (bat.settings.max_user_set_discharge_voltage_dV + HYSTERESIS_OFFSET_DV)) {
      safety.discharge_blocked[instance] = false;
    }
    if (safety.discharge_blocked[instance]) {
      bat.status.max_discharge_power_W = 0;
    }
  }

  // Battery is fully charged. Dont allow any more power into it
  // Normally the BMS will send 0W allowed, but this acts as an additional layer of safety
  if (bat.status.reported_soc == 10000 || bat.status.real_soc == 10000) {  //Either Scaled OR Real SOC% is 100.00%
    v.full = true;
    bat.status.max_charge_power_W = 0;
  }

  // Battery is empty. Do not allow further discharge.
  // Normally the BMS will send 0W allowed, but this acts as an additional layer of safety
  if (datalayer.system.status.system_status == ACTIVE) {
    if (bat.status.reported_soc == 0 || bat.status.real_soc == 0) {  //Either Scaled OR Real SOC% is 0.00%
      v.empty = true;
      bat.status.max_discharge_power_W = 0;
    }
  }

  // Battery is extremely degraded, not fit for secondlifestorage!
  if (bat.status.soh_pptt < 2500) {
    if (!v.soh_low || bat.status.soh_pptt < v.soh_low_pptt) {
      v.soh_low_pptt = bat.status.soh_pptt;
    }
    v.soh_low = true;
  }

  if (batteries[instance] && !batteries[instance]->soc_plausible()) {
    if (!v.soc_implausible) {
      v.soc_implausible_soc = bat.status.real_soc;
    }
    v.soc_implausible = true;
  }

  // Check diff between highest and lowest cell
  uint16_t deviation_mV = std::abs(bat.status.cell_max_voltage_mV - bat.status.cell_min_voltage_mV);
  if (deviation_mV > bat.info.max_cell_voltage_deviation_mV) {
    if (!v.cell_deviation || deviation_mV > v.cell_deviation_mV) {
      v.cell_deviation_mV = deviation_mV;
    }
    v.cell_deviation = true;
  }

  /* Check that the inverter respects the charge/discharge limits we hand it.
     Skipped entirely while a pause is requested or a fault is active: those zero the
     limits above instantly, but the inverter only reads the new values on its next
     poll and then still needs time to ramp down, so comparing during that window
     blames it for a limit it cannot have seen yet. Counters are reset on the way in,
     so the pause never leaves a stale alert behind.
     The limits are unsigned, so cast before comparing against the signed power.
     The gate condition is global, so every instance takes the same branch and the
     aggregation below sees a uniform "evaluated, not exceeded" verdict. */
  v.charge_limit_evaluated = true;
  v.discharge_limit_evaluated = true;
  if (emulator_pause_request_ON || emulator_pause_status != NORMAL ||
      datalayer.system.status.system_status == FAULT) {
    safety.charge_limit_failures[instance] = 0;
    safety.discharge_limit_failures[instance] = 0;
  } else {
    // Inverter is charging with more power than battery wants!
    if (bat.status.active_power_W > (int32_t)(bat.status.max_charge_power_W + 2000)) {
      if (safety.charge_limit_failures[instance] > MAX_CHARGE_DISCHARGE_LIMIT_FAILURES) {
        v.charge_limit_exceeded = true;  // Alert when 2kW over requested max
      } else {
        safety.charge_limit_failures[instance]++;
      }
    } else {  // Also taken when idle at 0W, so a stopped inverter always clears the alert
      safety.charge_limit_failures[instance] = 0;
    }

    // Inverter is pulling too much power from battery!
    if (-bat.status.active_power_W > (int32_t)(bat.status.max_discharge_power_W + 2000)) {
      if (safety.discharge_limit_failures[instance] > MAX_CHARGE_DISCHARGE_LIMIT_FAILURES) {
        v.discharge_limit_exceeded = true;  // Alert when 2kW over requested max
      } else {
        safety.discharge_limit_failures[instance]++;
      }
    } else {  // Also taken when idle at 0W, so a stopped inverter always clears the alert
      safety.discharge_limit_failures[instance] = 0;
    }
  }

  // Check if SOH% between this pack and the primary is too large (extras only)
  if (instance != 0 && (datalayer.batteries[0].status.soh_pptt != 9900) && (bat.status.soh_pptt != 9900)) {
    v.soh_difference_evaluated = true;
    uint16_t soh_diff_pptt;
    if (datalayer.batteries[0].status.soh_pptt > bat.status.soh_pptt) {
      soh_diff_pptt = datalayer.batteries[0].status.soh_pptt - bat.status.soh_pptt;
    } else {
      soh_diff_pptt = bat.status.soh_pptt - datalayer.batteries[0].status.soh_pptt;
    }
    if (soh_diff_pptt > MAX_SOH_DEVIATION_PPTT) {
      v.soh_difference = true;
    }
  }

  // Check that this battery's BMS has been seen and is still sending CAN
  // messages. If we go 60s without messages we raise an error.
  // Per-instance event ids are enum values and cannot be arrayed away.
  struct BatteryAliveEvents {
    EVENTS_ENUM_TYPE detected_event;
    EVENTS_ENUM_TYPE missing_event;
  };
  static_assert(MAX_BATTERIES == 3, "add the new instance's detected/missing event ids");
  static constexpr BatteryAliveEvents alive_events[MAX_BATTERIES] = {
      {EVENT_CAN_BATTERY_DETECTED, EVENT_CAN_BATTERY_MISSING},
      {EVENT_CAN_BATTERY2_DETECTED, EVENT_CAN_BATTERY2_MISSING},
      {EVENT_CAN_BATTERY3_DETECTED, EVENT_CAN_BATTERY3_MISSING},
  };
  check_can_component_alive(bat.status.CAN_battery_still_alive, datalayer.system.status.battery_link[instance].detected,
                            alive_events[instance].detected_event, alive_events[instance].missing_event,
                            can_config.batteries[instance]);
}

static void raise_machinery_protection_events(const MachineryProtectionVerdicts& v) {
  if (v.overvoltage) {
    set_event(EVENT_BATTERY_OVERVOLTAGE, v.overvoltage_dV);
  } else {
    clear_event(EVENT_BATTERY_OVERVOLTAGE);
  }
  if (v.undervoltage) {
    set_event(EVENT_BATTERY_UNDERVOLTAGE, v.undervoltage_dV);
  } else {
    clear_event(EVENT_BATTERY_UNDERVOLTAGE);
  }
  // The cell events latch by design: never cleared automatically
  if (v.cell_over) {
    set_event(EVENT_CELL_OVER_VOLTAGE, 0);
  }
  if (v.cell_critical_over) {
    set_event(EVENT_CELL_CRITICAL_OVER_VOLTAGE, 0);
  }
  if (v.cell_under) {
    set_event(EVENT_CELL_UNDER_VOLTAGE, 0);
  }
  if (v.cell_critical_under) {
    set_event(EVENT_CELL_CRITICAL_UNDER_VOLTAGE, 0);
  }
  if (v.full) {
    if (!safety.battery_full_event_fired) {
      set_event(EVENT_BATTERY_FULL, 0);
      safety.battery_full_event_fired = true;
    }
  } else {
    clear_event(EVENT_BATTERY_FULL);
    safety.battery_full_event_fired = false;
  }
  if (datalayer.system.status.system_status == ACTIVE) {
    if (v.empty) {
      if (!safety.battery_empty_event_fired) {
        set_event(EVENT_BATTERY_EMPTY, 0);
        safety.battery_empty_event_fired = true;
      }
    } else {
      clear_event(EVENT_BATTERY_EMPTY);
      safety.battery_empty_event_fired = false;
    }
  }
  if (v.soh_low) {
    set_event(EVENT_SOH_LOW, v.soh_low_pptt);
  } else {
    clear_event(EVENT_SOH_LOW);
  }
  if (v.soc_implausible) {
    set_event(EVENT_SOC_PLAUSIBILITY_ERROR, v.soc_implausible_soc);
  }
  if (v.cell_deviation) {
    set_event(EVENT_CELL_DEVIATION_HIGH, (v.cell_deviation_mV / 20));
  } else {
    clear_event(EVENT_CELL_DEVIATION_HIGH);
  }
  if (v.charge_limit_exceeded) {
    set_event(EVENT_CHARGE_LIMIT_EXCEEDED, 0);
  } else if (v.charge_limit_evaluated) {
    clear_event(EVENT_CHARGE_LIMIT_EXCEEDED);
  }
  if (v.discharge_limit_exceeded) {
    set_event(EVENT_DISCHARGE_LIMIT_EXCEEDED, 0);
  } else if (v.discharge_limit_evaluated) {
    clear_event(EVENT_DISCHARGE_LIMIT_EXCEEDED);
  }
  if (v.soh_difference) {
    set_event(EVENT_SOH_DIFFERENCE, (uint8_t)(MAX_SOH_DEVIATION_PPTT / 100));
  } else if (v.soh_difference_evaluated) {
    clear_event(EVENT_SOH_DIFFERENCE);
  }
}

void update_machineryprotection() {
  //Check if we start to get low on memory
  if (datalayer.system.info.CPU_free_heap < 62000) {
    safety.hysteresis_heap_seconds++;
    if (safety.hysteresis_heap_seconds >
        5) {  //Trigger after X seconds of low heap, to prevent false positives during spikes
      set_event(EVENT_LOW_HEAP_MEMORY, (datalayer.system.info.CPU_free_heap / 1000));
    }
  } else {
    clear_event(EVENT_LOW_HEAP_MEMORY);
    safety.hysteresis_heap_seconds = 0;
  }

  // Check health status of CAN interfaces
  if (datalayer.system.info.can_native_send_fail) {
    set_event(EVENT_CAN_NATIVE_BUFFER_FULL, 0);
    datalayer.system.info.can_native_send_fail = false;
  } else {
    clear_event(EVENT_CAN_NATIVE_BUFFER_FULL);
  }
  if (datalayer.system.info.can_native_bus_error) {
    set_event(EVENT_CAN_NATIVE_BUS_ERROR, 0);
    datalayer.system.info.can_native_bus_error = false;
  } else {
    clear_event(EVENT_CAN_NATIVE_BUS_ERROR);
  }
  if (datalayer.system.info.can_2515_send_fail) {
    set_event(EVENT_CANMCP2515_BUFFER_FULL, 0);
    datalayer.system.info.can_2515_send_fail = false;
  } else {
    clear_event(EVENT_CANMCP2515_BUFFER_FULL);
  }
  if (datalayer.system.info.can_2515_bus_error) {
    set_event(EVENT_CANMCP2515_BUS_ERROR, 0);
    datalayer.system.info.can_2515_bus_error = false;
  } else {
    clear_event(EVENT_CANMCP2515_BUS_ERROR);
  }
  if (datalayer.system.info.can_2518_send_fail) {
    set_event(EVENT_CANFD_BUFFER_FULL, 0);
    datalayer.system.info.can_2518_send_fail = false;
  } else {
    clear_event(EVENT_CANFD_BUFFER_FULL);
  }
  if (datalayer.system.info.can_2518_bus_error) {
    set_event(EVENT_CANFD_BUS_ERROR, 0);
    datalayer.system.info.can_2518_bus_error = false;
  } else {
    clear_event(EVENT_CANFD_BUS_ERROR);
  }
  if (datalayer.system.info.can_2518_2_send_fail) {
    set_event(EVENT_CANFD_2_BUFFER_FULL, 0);
    datalayer.system.info.can_2518_2_send_fail = false;
  } else {
    clear_event(EVENT_CANFD_2_BUFFER_FULL);
  }
  if (datalayer.system.info.can_2518_2_bus_error) {
    set_event(EVENT_CANFD_2_BUS_ERROR, 0);
    datalayer.system.info.can_2518_2_bus_error = false;
  } else {
    clear_event(EVENT_CANFD_2_BUS_ERROR);
  }

  // Machinery protection for every constructed battery instance: the helper
  // collects per-instance verdicts, the aggregation below owns the shared
  // events - set if ANY instance trips (worst offender's payload), cleared
  // only when ALL are clean, so one clean battery cannot mask another's
  // active fault.
  MachineryProtectionVerdicts verdicts;
  for (int i = 0; i < MAX_BATTERIES; ++i) {
    if (batteries[i]) {
      check_battery_machinery_protection(i, verdicts);
    }
  }
  raise_machinery_protection_events(verdicts);

  if (inverter && inverter->interface_type() == InverterInterfaceType::Can) {

    //Check if we have ever seen the inverter
    if (!safety.inverter_detected) {
      // >= not ==: some drivers refresh the counter above CAN_STILL_ALIVE for a
      // longer timeout (SMA x3, Sofar x2), which would never match equality
      if (datalayer.system.status.CAN_inverter_still_alive >= CAN_STILL_ALIVE) {
        safety.inverter_detected = true;
        set_event(EVENT_CAN_INVERTER_DETECTED, 1);
      }
    }

    // Check if the inverter is still sending CAN messages. If we go 60s without messages we raise a warning
    if (!datalayer.system.status.CAN_inverter_still_alive) {
      // Inverters that are slow to boot get a startup grace window before we fault.
      // millis64: with plain millis() this comparison goes false again for 5 minutes
      // after the 49.7-day wrap, re-suppressing detection of a new inverter-comms loss
      if (!inverter->needs_can_startup_grace() || millis64() > INVERTER_STARTUP_GRACE_MS) {
        set_event(EVENT_CAN_INVERTER_MISSING, can_config.inverter);
      }
    } else {
      // Inverter frames received recently - clear any previously raised missing-event
      // regardless of which timeout mode is active
      clear_event(EVENT_CAN_INVERTER_MISSING);
      // If the inverter is a slow starter, only decrement the counter every third second,
      // tripling the timeout (60 s -> 180 s) to give it more time to start up before we
      // report it as missing
      if (user_selected_inverter_long_CAN_timeout) {
        safety.slow_start_counter++;
        if (safety.slow_start_counter > 2) {  // Counts 1, 2, 3: the decrement runs on every third pass
          datalayer.system.status.CAN_inverter_still_alive--;
          safety.slow_start_counter = 0;
        }
      } else {  //Normal 60s timeout for regular inverters
        datalayer.system.status.CAN_inverter_still_alive--;
      }
    }
  }

  if (charger) {
    // Assuming chargers are all CAN here.
    // Check that the charger has been seen and is still sending CAN messages.
    // If we go 60s without messages we raise a warning
    check_can_component_alive(datalayer.charger.CAN_charger_still_alive, safety.charger_detected,
                              EVENT_CAN_CHARGER_DETECTED, EVENT_CAN_CHARGER_MISSING, charger->interface());
  }


  // Temperature limits are shared by all batteries, so all of them are checked in one pass
  check_battery_temperatures();

  // Too many malformed CAN messages received! EVENT_CAN_CORRUPTED_WARNING is shared by
  // all batteries; evaluate them together so one battery's clean state can no longer
  // clear another battery's active warning. Event data = first offending channel.
  bool can_corrupted = false;
  uint8_t corrupted_channel = can_config.batteries[0];
  if (batteries[0] && datalayer.batteries[0].status.CAN_error_counter > MAX_CAN_FAILURES) {
    can_corrupted = true;
  }
  const CAN_Interface secondary_interface[MAX_BATTERIES] = {can_config.batteries[0], can_config.batteries[1],
                                                            can_config.batteries[2]};
  for (int i = 1; i < MAX_BATTERIES; ++i) {
    if (batteries[i] && !can_corrupted && datalayer_battery(i).status.CAN_error_counter > MAX_CAN_FAILURES) {
      can_corrupted = true;
      corrupted_channel = secondary_interface[i];
    }
  }
  if (can_corrupted) {
    set_event(EVENT_CAN_CORRUPTED_WARNING, corrupted_channel);
  } else {
    clear_event(EVENT_CAN_CORRUPTED_WARNING);
  }

  //Safeties verified, Zero charge/discharge ampere values incase any safety wrote the W to 0
  if (datalayer.batteries[0].status.max_discharge_power_W == 0) {
    datalayer.batteries[0].status.max_discharge_current_dA = 0;
  }
  if (datalayer.batteries[0].status.max_charge_power_W == 0) {
    datalayer.batteries[0].status.max_charge_current_dA = 0;
  }
  //One exception. If user has enabled the emergency recovery charge mode, still allow small amount of charging
  if (datalayer.batteries[0].settings.user_requests_forced_charging_recovery_mode) {

    //We allow the user set value as long as it does not exceed MAX_CHARGEPOWER_RECOVERY_CHARGE_DA
    if (datalayer.batteries[0].settings.max_user_set_charge_dA > MAX_CHARGEPOWER_RECOVERY_CHARGE_DA) {
      datalayer.batteries[0].status.max_charge_current_dA = MAX_CHARGEPOWER_RECOVERY_CHARGE_DA;
    } else {
      datalayer.batteries[0].status.max_charge_current_dA = datalayer.batteries[0].settings.max_user_set_charge_dA;
    }

    // If this is the start of the emergency recovery charge period, capture the current time
    if (datalayer.batteries[0].settings.recovery_charge_start_time_ms == 0) {
      datalayer.batteries[0].settings.recovery_charge_start_time_ms = millis();
      set_event(EVENT_RECOVERY_START, 0);
    } else {
      clear_event(EVENT_RECOVERY_START);
    }

    // Check if the elapsed time exceeds the max recovery charge time
    if (millis() - datalayer.batteries[0].settings.recovery_charge_start_time_ms >=
        datalayer.batteries[0].settings.recovery_charge_max_time_ms) {
      datalayer.batteries[0].settings.user_requests_forced_charging_recovery_mode = false;
      datalayer.batteries[0].settings.recovery_charge_start_time_ms = 0;  // Reset the start time
      set_event(EVENT_RECOVERY_END, 0);
    } else {
      clear_event(EVENT_RECOVERY_END);
    }

    //Check if cellvoltage is too low to safely start recovery. If so, abort!
    if (datalayer.batteries[0].status.cell_min_voltage_mV < LOWEST_ALLOWED_CELLVOLTAGE_RECOVERY_CHARGE_MV) {
      datalayer.batteries[0].settings.user_requests_forced_charging_recovery_mode = false;
      set_event(EVENT_RECOVERY_END, 255);
    }
  }

  //Decrement the forced balancing timer incase user requested it
  if (datalayer.batteries[0].settings.user_requests_balancing) {
    // If this is the start of the balancing period, capture the current time
    if (datalayer.batteries[0].settings.balancing_start_time_ms == 0) {
      datalayer.batteries[0].settings.balancing_start_time_ms = millis();
      set_event(EVENT_BALANCING_START, 0);
    } else {
      clear_event(EVENT_BALANCING_START);
    }

    // Check if the elapsed time exceeds the balancing time
    if (millis() - datalayer.batteries[0].settings.balancing_start_time_ms >=
        datalayer.batteries[0].settings.balancing_max_time_ms) {
      datalayer.batteries[0].settings.user_requests_balancing = false;
      datalayer.batteries[0].settings.balancing_start_time_ms = 0;  // Reset the start time
      set_event(EVENT_BALANCING_END, 0);
    } else {
      clear_event(EVENT_BALANCING_END);
    }
  }
}

//battery pause status begin
void setBatteryPause(bool pause_battery, bool pause_CAN, EquipmentStop equipment_stop, bool store_settings) {
  DEBUG_PRINTF("Battery pause sequence %d %d %d %d\n", pause_battery, pause_CAN, equipment_stop, store_settings);

  // First handle equipment stop / resume
  if (equipment_stop == STOP && !datalayer.system.info.equipment_stop_active) {
    datalayer.system.info.equipment_stop_active = true;
    if (store_settings) {
      store_settings_equipment_stop();
    }

    set_event(EVENT_EQUIPMENT_STOP, 1);
  } else if (equipment_stop == RESUME && datalayer.system.info.equipment_stop_active) {
    datalayer.system.info.equipment_stop_active = false;
    if (store_settings) {
      store_settings_equipment_stop();
    }
    clear_event(EVENT_EQUIPMENT_STOP);
  }

  emulator_pause_CAN_send_ON = pause_CAN;

  if (pause_battery) {

    set_event(EVENT_PAUSE_BEGIN, 1);
    emulator_pause_request_ON = true;
    emulator_pause_status = PAUSING;
    datalayer.batteries[0].status.max_discharge_power_W = 0;
    datalayer.batteries[0].status.max_charge_power_W = 0;
    for (int i = 1; i < MAX_BATTERIES; ++i) {
      if (batteries[i]) {
        datalayer_battery(i).status.max_discharge_power_W = 0;
        datalayer_battery(i).status.max_charge_power_W = 0;
      }
    }

  } else {
    clear_event(EVENT_PAUSE_BEGIN);
    set_event(EVENT_PAUSE_END, 0);
    emulator_pause_request_ON = false;
    emulator_pause_CAN_send_ON = false;
    emulator_pause_status = RESUMING;
    clear_event(EVENT_PAUSE_END);
  }

  //immediate check if we can send CAN messages
  update_pause_state();
}

void graceful_restart() {
  // Pause charge/discharge, and then restart the ESP32 within 5s (as soon as the power stops).

  set_event(EVENT_RESTARTING, 0);

  // Stop charge/discharge so we don't damage the contactors
  setBatteryPause(true, false, EquipmentStop::UNCHANGED, false);

  uint32_t now = millis();
  safety.emulator_restart_request_millis = now > 0 ? now : 1;
}

void update_restart_progress() {
  // If is a restart has been requested, check the time and restart if the
  // conditions are met.

  if (safety.emulator_restart_request_millis > 0) {
    uint32_t now = millis();
    uint32_t elapsed = now - safety.emulator_restart_request_millis;
    // Restart after 5s if the emulator has paused. Always restart after 10s.
    if ((elapsed > INTERVAL_5_S && emulator_pause_status == PAUSED) || elapsed > INTERVAL_10_S) {
      ESP.restart();
    }
  }
}

/// @brief handle emulator pause status and CAN sending allowed
void update_pause_state() {
  bool previous_allowed_to_send_CAN = allowed_to_send_CAN;

  if (emulator_pause_status == NORMAL) {
    allowed_to_send_CAN = true;
  }

  int16_t battery_current_dA = datalayer.batteries[0].status.current_dA;
  int16_t battery2_current_dA = datalayer.batteries[1].status.current_dA;  // Should be 0 if no battery2
  int16_t battery3_current_dA = datalayer.batteries[2].status.current_dA;  // Should be 0 if no battery3
  static const int16_t CURRENT_THRESHOLD_dA = 18;                          // 1.8A in deciAmps

  // in some inverters this values are not accurate, so we need to check if we are consider 1.8 amps as the limit
  if (emulator_pause_request_ON && emulator_pause_status == PAUSING && abs(battery_current_dA) < CURRENT_THRESHOLD_dA &&
      abs(battery2_current_dA) < CURRENT_THRESHOLD_dA && abs(battery3_current_dA) < CURRENT_THRESHOLD_dA) {
    emulator_pause_status = PAUSED;
  }

  if (!emulator_pause_request_ON && emulator_pause_status == RESUMING) {
    emulator_pause_status = NORMAL;
    allowed_to_send_CAN = true;
  }

  allowed_to_send_CAN = (!emulator_pause_CAN_send_ON || emulator_pause_status == NORMAL);

  if (previous_allowed_to_send_CAN && !allowed_to_send_CAN) {
    DEBUG_PRINTF("Safety: Pausing CAN sending\n");
    //completely force stop the CAN communication
    stop_can();
  } else if (!previous_allowed_to_send_CAN && allowed_to_send_CAN) {
    //resume CAN communication
    DEBUG_PRINTF("Safety: Resuming CAN sending\n");
    restart_can();
  }
}

std::string get_emulator_pause_status() {
  switch (emulator_pause_status) {
    case NORMAL:
      return "RUNNING";
    case PAUSING:
      return "PAUSING";
    case PAUSED:
      return "PAUSED";
    case RESUMING:
      return "RESUMING";
    default:
      return "UNKNOWN";
  }
}
//battery pause status

#ifdef UNIT_TEST
void reset_safety_state() {
  safety = SafetyState();
  reset_parallel_join_state();
  emulator_pause_request_ON = false;
  emulator_pause_CAN_send_ON = false;
  emulator_pause_status = NORMAL;
  allowed_to_send_CAN = true;
}
#endif
