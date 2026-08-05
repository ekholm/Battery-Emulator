#ifndef _PRECHARGE_FSM_H_
#define _PRECHARGE_FSM_H_

#include <stdint.h>

// The one precharge/contactor state machine. The FSM owns sequencing,
// permission gating and the fault latch; a ContactorActuator owns how its
// topology drives the relays (GPIO pins, S-BOX CAN frames, ...) and any extra
// conditions that topology imposes. State is published into the datalayer by
// the actuator, since what each topology reports differs.
class ContactorActuator {
 public:
  // Today's canonical states. The close sequence runs
  // START_PRECHARGE -> PRECHARGE -> POSITIVE -> PRECHARGE_OFF -> COMPLETED;
  // what each step actually switches is the actuator's business.
  enum State : uint8_t {
    DISCONNECTED,
    START_PRECHARGE,
    PRECHARGE,
    POSITIVE,
    PRECHARGE_OFF,
    COMPLETED,
    SHUTDOWN_REQUESTED
  };

  virtual ~ContactorActuator() = default;

  // Actuation entering a step of the close sequence, including this
  // topology's datalayer publication for the step.
  virtual void apply_step(State step) = 0;

  // Milliseconds that must have elapsed since the previous step fired before
  // this step may fire.
  virtual unsigned long step_delay_ms(State step) const = 0;

  // Extra per-step condition beyond the delay (S-BOX: precharge resistor
  // voltage ratio before the positive relay).
  virtual bool step_gate(State step) { return true; }

  // Extra conditions to leave DISCONNECTED, beyond inverter permission and
  // equipment stop (S-BOX: measurable pack voltage).
  virtual bool start_allowed() { return true; }

  // Gate on executing the timed close sequence at all (GPIO: 10 s boot guard
  // + battery detected). Separate from start_allowed() to preserve today's
  // semantics: the FSM enters START_PRECHARGE freely, only the steps wait.
  virtual bool steps_allowed(unsigned long now_ms) { return true; }

  // Whether an equipment stop while COMPLETED holds the contactors closed
  // until the pause state machine reports PAUSED (opening under load risks
  // arcing/welding), bounded by the FSM's timeout. false = open immediately.
  virtual bool hold_open_for_estop_pause() { return false; }

  // FAULT ticks before the shutdown latch trips.
  virtual unsigned long fault_tick_budget() const { return 1000; }

  // Open everything + publish this topology's open state. fault = true is
  // the latched shutdown path.
  virtual void open_all(bool fault) = 0;

  // Per-tick publication (GPIO: dc_bus_live tracks COMPLETED).
  virtual void publish_tick(State state) {}
};

class PrechargeFsm {
 public:
  using State = ContactorActuator::State;

  explicit PrechargeFsm(ContactorActuator& actuator) : actuator_(actuator) {}

  void tick(unsigned long now_ms);

  State state() const { return state_; }
  // Tests only: place the FSM in a known state.
  void set_state(State state) { state_ = state; }

 private:
  static State next_state(State state);

  ContactorActuator& actuator_;
  State state_ = State::DISCONNECTED;
  unsigned long step_started_ms_ = 0;
  unsigned long fault_ticks_ = 0;
  unsigned long estop_open_wait_start_ms_ = 0;
};

#endif
