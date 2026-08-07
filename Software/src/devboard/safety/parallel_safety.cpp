#include "parallel_safety.h"
#include "../../battery/BATTERIES.h"
#include "../../datalayer/datalayer.h"
#include "../utils/events.h"

// True when the pack currently holds the DC link (contactors closed). Prefers
// the state the pack's own BMS reports; Unknown falls back to the BE-commanded
// state.
static bool pack_holds_link(Battery* pack, bool commanded_engaged) {
  if (pack) {
    ContactorState reported = pack->reported_contactor_state();
    if (reported == ContactorState::Closed) {
      return true;
    }
    if (reported == ContactorState::Open) {
      return false;
    }
  }
  return commanded_engaged;
}

// Symmetric half of the 1.5 V parallel-join rule: while another pack holds the
// link and the voltages differ too much, the MAIN battery must not (re-)close
// onto it either. Close-gating only - opening the main under load is its own
// hazard and stays out of scope here. ([0] is unused - the main never blocks
// itself.)
static bool main_blocked_by_joiner[MAX_BATTERIES] = {};

// Drift timers, one per joining pack. File scope rather than function-local so
// reset_parallel_join_state() can clear them: they accumulate across calls by
// design, which in a single-process test binary means across tests too.
static uint8_t seconds_out_of_sync[MAX_BATTERIES] = {};

static void update_main_join_permission() {
  bool blocked = false;
  for (int i = 1; i < MAX_BATTERIES; ++i) {
    blocked = blocked || main_blocked_by_joiner[i];
  }
  datalayer.system.status.battery_link[0].allowed_contactor_closing = !blocked;
}

// The 1.5 V parallel-join rule for one joining pack (instance 1..) against the
// main battery, in both directions: gates the joiner's own permission (alert
// after 3 s out of sync, disengage after 10 s), and computes whether the
// joiner blocks the MAIN battery from (re-)closing onto the link.
static void check_parallel_join(int instance, EVENTS_ENUM_TYPE voltage_diff_event, uint8_t& seconds_out_of_sync) {
  const DATALAYER_BATTERY_TYPE& joiner_datalayer = datalayer_battery(instance);
  DATALAYER_BATTERY_LINK_TYPE& joiner_link = datalayer.system.status.battery_link[instance];

  if (datalayer.batteries[0].status.voltage_dV == 0 || joiner_datalayer.status.voltage_dV == 0) {
    return;  // Both voltage values need to be available to start check
  }
  if (datalayer.batteries[0].status.voltage_dV == 3700 || joiner_datalayer.status.voltage_dV == 3700) {
    return;  // Also abort if both voltages happened to be initialized to the 3700 default value that most integrations use
  }
  uint16_t voltage_diff_towards_main =
      abs(datalayer.batteries[0].status.voltage_dV - joiner_datalayer.status.voltage_dV);

  main_blocked_by_joiner[instance] =
      pack_holds_link(batteries[instance], joiner_link.contactors_engaged) && (voltage_diff_towards_main > 15);
  update_main_join_permission();

  if (voltage_diff_towards_main <= 15) {  // If we are within 1.5V between the batteries
    clear_event(voltage_diff_event);
    seconds_out_of_sync = 0;
    if (datalayer.system.status.system_status == FAULT) {
      // If main battery is in fault state, disengage the joining battery
      joiner_link.allowed_contactor_closing = false;
    } else {  // If main battery is OK, allow the joining battery to close
      joiner_link.allowed_contactor_closing = true;
    }
  } else {  //Voltage between the two packs is too large
    //If we start to drift out of sync between the two packs for more than 10 seconds, open contactors
    //We alert user if we have been out of sync for more than 3 seconds, but we allow 10 seconds before we disengage the joining battery
    if (seconds_out_of_sync < 10) {
      ++seconds_out_of_sync;
      if (seconds_out_of_sync > 3) {
        set_event(voltage_diff_event, (uint8_t)(voltage_diff_towards_main / 10));
      }
    } else {  //10 seconds out of sync, disengage the joining battery
      joiner_link.allowed_contactor_closing = false;
    }
  }
}

void check_parallel_battery_safety(uint8_t batteryNumber) {
  static_assert(MAX_BATTERIES == 3, "add the new instance's voltage-difference event and sync counter");
  // Per-joiner state; [0] unused (the main battery is never a joiner).
  static constexpr EVENTS_ENUM_TYPE voltage_diff_event[MAX_BATTERIES] = {
      EVENT_VOLTAGE_DIFFERENCE_BAT2, EVENT_VOLTAGE_DIFFERENCE_BAT2, EVENT_VOLTAGE_DIFFERENCE_BAT3};

  int instance = batteryNumber - 1;
  if (instance < 1 || instance >= MAX_BATTERIES) {
    return;
  }

  /* Before the checks are started, we need to know the battery is alive via CAN, and that the voltages have ben read*/
  if (datalayer.system.status.battery_link[instance].detected) {
    check_parallel_join(instance, voltage_diff_event[instance], seconds_out_of_sync[instance]);
  } else {
    main_blocked_by_joiner[instance] = false;
    update_main_join_permission();
  }
}

#ifdef UNIT_TEST
void reset_parallel_join_state() {
  for (int i = 0; i < MAX_BATTERIES; ++i) {
    main_blocked_by_joiner[i] = false;
    seconds_out_of_sync[i] = 0;
  }
}
#endif
