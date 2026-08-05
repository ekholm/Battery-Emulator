#include "precharge_fsm.h"

#include "../../datalayer/datalayer.h"
#include "../../devboard/safety/safety.h"
#include "../../devboard/utils/events.h"
#include "../../devboard/utils/logging.h"

// Equipment stop: max time to wait for the pause to reach zero current before
// opening contactors anyway (only for actuators that hold for the pause).
#define ESTOP_OPEN_TIMEOUT_MS 7000

PrechargeFsm::State PrechargeFsm::next_state(State state) {
  switch (state) {
    case State::START_PRECHARGE:
      return State::PRECHARGE;
    case State::PRECHARGE:
      return State::POSITIVE;
    case State::POSITIVE:
      return State::PRECHARGE_OFF;
    case State::PRECHARGE_OFF:
      return State::COMPLETED;
    default:
      return state;
  }
}

void PrechargeFsm::tick(unsigned long now_ms) {
  actuator_.publish_tick(state_);

  // First check if we have any active errors, incase we do, turn off the battery
  if (datalayer.system.status.system_status == FAULT) {
    ++fault_ticks_;
  } else {
    fault_ticks_ = 0;
  }

  if (fault_ticks_ > actuator_.fault_tick_budget()) {
    state_ = State::SHUTDOWN_REQUESTED;
  }

  if (state_ == State::SHUTDOWN_REQUESTED) {
    actuator_.open_all(true);
    return;  // A fault scenario latches the contactor control. It is not possible to recover without a powercycle (and investigation why fault occured)
  }

  // After that, check if we are OK to start turning on the battery
  if (state_ == State::DISCONNECTED) {
    actuator_.open_all(false);

    // battery_link[0].allowed_contactor_closing is the symmetric parallel-join
    // gate: do not close the main battery onto a link another pack holds live
    // with more than 1.5 V difference. Computed by the join arbiter; set true
    // at battery setup so single-battery systems are unaffected.
    if (datalayer.system.status.inverter_allows_contactor_closing && !datalayer.system.info.equipment_stop_active &&
        datalayer.system.status.battery_link[0].allowed_contactor_closing && actuator_.start_allowed()) {
      state_ = State::START_PRECHARGE;
    }
  }

  // In case the inverter or the equipment stop requests contactors to open, jump to Disconnected (recoverable)
  if (state_ == State::COMPLETED) {
    if (!datalayer.system.status.inverter_allows_contactor_closing) {
      // Inverter-commanded opening stays immediate: the inverter has already
      // stopped power transfer before revoking its permission
      state_ = State::DISCONNECTED;
    } else if (datalayer.system.info.equipment_stop_active) {
      if (actuator_.hold_open_for_estop_pause()) {
        // Equipment stop: every e-stop entry point also issues a battery pause,
        // so hold the contactors until the pause state machine reports PAUSED
        // (current below 1.8 A) - opening under load risks arcing/welding.
        // Bounded: after ESTOP_OPEN_TIMEOUT_MS we open anyway and raise an
        // event, mirroring the BMS-reset give-up behavior.
        if (estop_open_wait_start_ms_ == 0) {
          estop_open_wait_start_ms_ = now_ms;
        }
        bool paused = (emulator_pause_status == PAUSED);
        bool timed_out = (now_ms - estop_open_wait_start_ms_) > ESTOP_OPEN_TIMEOUT_MS;
        if (paused || timed_out) {
          if (timed_out) {
            logging.printf("Contactors: Equipment stop wait timed out, opening under load\n");
            set_event(EVENT_ERROR_OPEN_CONTACTOR, 1);
          }
          estop_open_wait_start_ms_ = 0;
          state_ = State::DISCONNECTED;
        }
      } else {
        state_ = State::DISCONNECTED;
      }
    } else {
      estop_open_wait_start_ms_ = 0;
    }
    // Skip running the state machine below if it has already completed
    return;
  }

  if (!actuator_.steps_allowed(now_ms)) {
    return;
  }

  // Handle the actual close sequence: each step fires once its delay since
  // the previous step has elapsed and the actuator's own gate agrees.
  switch (state_) {
    case State::START_PRECHARGE:
    case State::PRECHARGE:
    case State::POSITIVE:
    case State::PRECHARGE_OFF:
      if (now_ms - step_started_ms_ >= actuator_.step_delay_ms(state_) && actuator_.step_gate(state_)) {
        actuator_.apply_step(state_);
        step_started_ms_ = now_ms;
        state_ = next_state(state_);
      }
      break;
    default:
      break;
  }
}
