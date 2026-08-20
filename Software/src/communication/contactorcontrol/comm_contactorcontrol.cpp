#include "comm_contactorcontrol.h"
#include "../../battery/BATTERIES.h"
#include "../../devboard/hal/hal.h"
#include "../../devboard/safety/safety.h"
#include "../../devboard/utils/led_handler.h"
#include "../../inverter/INVERTERS.h"
#ifndef UNIT_TEST
#include "driver/gpio.h"  // gpio_hold_en / gpio_hold_dis / gpio_deep_sleep_hold_en
#endif
#include <Arduino.h>

// TODO: Ensure valid values at run-time
// User can update all these values via Settings page
bool contactor_control_inverted_logic = false;  //Should we control NC contactors? Extremely rare option
uint16_t precharge_time_ms = 100;               //Precharge time in ms. Adjust depending on capacitance in inverter
bool pwm_contactor_control = false;             //Should the contactors be economized via PWM after they are engaged?
// Should the emulator's GPIO contactor control drive battery i's contactors?
// [0] is the primary (the precharge state machine + pin trio); the extras get
// a single on/off contactor pin each.
bool contactor_control_enabled[MAX_BATTERIES] = {};
bool remote_bms_reset = false;                   //Is it possible to actuate BMS reset via MQTT?
bool periodic_bms_reset = false;                 //Should periodic BMS reset be performed?
uint16_t periodic_bms_reset_interval_h = 24;     //How often the periodic BMS reset runs, in hours (24 or 48)
bool periodic_bms_reset_defer_low_soc = false;   //Defer the reset while SOC is below BMS_RESET_DEFER_SOC_PPTT
bool periodic_bms_reset_skip_balancing = false;  //Skip one period if the pack is balancing

// Parameters
const uint8_t ON = 1;
const uint8_t OFF = 0;

#define NEGATIVE_CONTACTOR_TIME_MS \
  500  // Time after negative contactor is turned on, to start precharge (not actual precharge time!)
#define PRECHARGE_COMPLETED_TIME_MS \
  1000  // After successful precharge, resistor is turned off after this delay (and contactors are economized if PWM enabled)
uint16_t pwm_frequency = 20000;
uint16_t pwm_hold_duty = 250;
#define PWM_ON_DUTY 1023
#define PWM_RESOLUTION 10
#define PWM_OFF_DUTY 0  //No need to have this userconfigurable
#define PWM_Positive_Channel 0
#define PWM_Negative_Channel 1
uint32_t currentTime = 0;
uint32_t lastPowerRemovalTime = 0;
BmsResetState bms_reset;
const uint32_t bmsWarmupDuration = 3000;
#define BMS_RESET_DEFER_SOC_PPTT 1500  // 15.00%, below this the low-SOC guard defers the periodic reset

/* The safety layer decrements CAN_battery_still_alive once per second and latches
   EVENT_CAN_BATTERY_MISSING when it reaches zero, so the BMS may only be silent for
   CAN_STILL_ALIVE seconds. A reset that keeps the BMS powered off for longer than that
   would always trip the event, so for those durations we refresh the liveness counters
   ourselves while the reset runs. Refreshing one second before the window closes keeps
   the counter from ever reaching zero. Durations that fit inside the window are left
   alone and keep the original, unmasked behaviour. */
#define BMS_RESET_CAN_KEEPALIVE_INTERVAL_MS ((uint32_t)(CAN_STILL_ALIVE - 1) * 1000UL)

void set(uint8_t pin, bool direction, uint32_t pwm_freq = 0xFFFF) {

  if (contactor_control_inverted_logic) {
    direction = !direction;  //Invert direction for NC contactors
  }

  if (pwm_contactor_control) {
    if (pwm_freq != 0xFFFF) {
      ledcWrite(pin, pwm_freq);
      return;
    }
  }
  if (direction == 1) {
    digitalWrite(pin, HIGH);
  } else {  // 0
    digitalWrite(pin, LOW);
  }
}

bool bms_power_is_active() {
  return periodic_bms_reset || remote_bms_reset || esp32hal->always_enable_bms_power();
}

// Initialization functions

const char* contactors = "Contactors";

static gpio_num_t extra_battery_contactors_pin(int instance) {
  static_assert(MAX_BATTERIES == 3, "add the new instance's contactor pin");
  return instance == 1 ? esp32hal->SECOND_BATTERY_CONTACTORS_PIN() : esp32hal->TRIPLE_BATTERY_CONTACTORS_PIN();
}

bool init_contactors() {
  // Init contactor pins
  if (contactor_control_enabled[0]) {
    auto posPin = esp32hal->POSITIVE_CONTACTOR_PIN();
    auto negPin = esp32hal->NEGATIVE_CONTACTOR_PIN();
    auto precPin = esp32hal->PRECHARGE_PIN();

    if (!esp32hal->alloc_pins(contactors, posPin, negPin, precPin)) {
      DEBUG_PRINTF("GPIO controlled contactor setup failed\n");
      return false;
    }

    if (pwm_contactor_control) {
      // Setup PWM Channel Frequency and Resolution
      ledcAttachChannel(posPin, pwm_frequency, PWM_RESOLUTION, PWM_Positive_Channel);
      ledcAttachChannel(negPin, pwm_frequency, PWM_RESOLUTION, PWM_Negative_Channel);
      // Set all pins OFF (0% PWM)
      ledcWrite(posPin, PWM_OFF_DUTY);
      ledcWrite(negPin, PWM_OFF_DUTY);
      set_indicator_led(IndicatorLed::CONTACTOR_POS, false);
      set_indicator_led(IndicatorLed::CONTACTOR_NEG, false);
    } else {  //Normal CONTACTOR_CONTROL
      pinMode(posPin, OUTPUT);
      set(posPin, OFF);
      set_indicator_led(IndicatorLed::CONTACTOR_POS, false);
      pinMode(negPin, OUTPUT);
      set(negPin, OFF);
      set_indicator_led(IndicatorLed::CONTACTOR_NEG, false);
    }  // Precharge never has PWM regardless of setting
    pinMode(precPin, OUTPUT);
    set(precPin, OFF);
    set_indicator_led(IndicatorLed::PRECHARGE, false);
  }

  for (int i = 1; i < MAX_BATTERIES; ++i) {
    if (!contactor_control_enabled[i]) {
      continue;
    }
    auto pin = extra_battery_contactors_pin(i);
    if (!esp32hal->alloc_pins(contactors, pin)) {
      DEBUG_PRINTF("%s battery contactor control setup failed\n", Battery::instance_label[i]);
      return false;
    }

    pinMode(pin, OUTPUT);
    set(pin, OFF);
  }

  // Init BMS contactor
  if (bms_power_is_active()) {
    auto pin = esp32hal->BMS_POWER();
    if (!esp32hal->alloc_pins("BMS power", pin)) {
      DEBUG_PRINTF("BMS power setup failed\n");
      return false;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    set_indicator_led(IndicatorLed::BMS_POWER, true);
  }

  return true;
}

static void dbg_contactors(const char* state) {
  logging.print("[");
  logging.print(millis());
  logging.print(" ms] contactors control: ");
  logging.println(state);
}

// GPIO relay topology: the canonical pin-driven contactor control.
class GpioContactorActuator : public ContactorActuator {
 public:
  void apply_step(State step) override {
    switch (step) {
      case START_PRECHARGE:
        set(esp32hal->NEGATIVE_CONTACTOR_PIN(), ON, PWM_ON_DUTY);
        set_indicator_led(IndicatorLed::CONTACTOR_NEG, true);
        dbg_contactors("NEGATIVE");
        datalayer.system.status.contactors_engaged = 3;
        break;
      case PRECHARGE:
        set(esp32hal->PRECHARGE_PIN(), ON);
        set_indicator_led(IndicatorLed::PRECHARGE, true);
        dbg_contactors("PRECHARGE");
        datalayer.system.status.contactors_engaged = 3;
        break;
      case POSITIVE:
        set(esp32hal->POSITIVE_CONTACTOR_PIN(), ON, PWM_ON_DUTY);
        set_indicator_led(IndicatorLed::CONTACTOR_POS, true);
        dbg_contactors("POSITIVE");
        datalayer.system.status.contactors_engaged = 3;
        break;
      case PRECHARGE_OFF:
        set(esp32hal->PRECHARGE_PIN(), OFF);
        set(esp32hal->NEGATIVE_CONTACTOR_PIN(), ON, pwm_hold_duty);
        set(esp32hal->POSITIVE_CONTACTOR_PIN(), ON, pwm_hold_duty);
        set_indicator_led(IndicatorLed::PRECHARGE, false);
        dbg_contactors("PRECHARGE_OFF");
        datalayer.system.status.contactors_engaged = 1;
        break;
      default:
        break;
    }
  }

  unsigned long step_delay_ms(State step) const override {
    switch (step) {
      case PRECHARGE:
        return NEGATIVE_CONTACTOR_TIME_MS;
      case POSITIVE:
        return precharge_time_ms;
      case PRECHARGE_OFF:
        return PRECHARGE_COMPLETED_TIME_MS;
      default:
        return 0;
    }
  }

  bool steps_allowed(unsigned long now_ms) override {
    // Skip running the state machine before system has started up. Conditions are 10sec post boot, and battery comms established
    // Gives the system some time to detect any faults from battery before blindly just engaging the contactors
    return (now_ms >= INTERVAL_10_S) && datalayer.system.status.battery_link[0].detected;
  }

  bool hold_open_for_estop_pause() override { return true; }

  void open_all(bool fault) override {
    set(esp32hal->PRECHARGE_PIN(), OFF);
    set(esp32hal->NEGATIVE_CONTACTOR_PIN(), OFF, PWM_OFF_DUTY);
    set(esp32hal->POSITIVE_CONTACTOR_PIN(), OFF, PWM_OFF_DUTY);
    set_indicator_led(IndicatorLed::PRECHARGE, false);
    set_indicator_led(IndicatorLed::CONTACTOR_NEG, false);
    set_indicator_led(IndicatorLed::CONTACTOR_POS, false);
    if (fault) {
      set_event(EVENT_ERROR_OPEN_CONTACTOR, 0);
      datalayer.system.status.contactors_engaged = 2;
    } else {
      datalayer.system.status.contactors_engaged = 0;
    }
  }

  void publish_tick(State state) override {
    // DC is only live to the inverter once precharge is fully complete (COMPLETED). Re-evaluated every
    // call, so any transition (open, fault-latch, precharge) is reflected within one 10 ms tick.
    datalayer.system.status.dc_bus_live = (state == COMPLETED);
  }
};

static GpioContactorActuator gpio_contactor_actuator;
PrechargeFsm precharge_fsm{gpio_contactor_actuator};

// Main functions of the handle_contactors include checking if inverter allows for closing, checking battery 2, checking BMS power output, and actual contactor closing/precharge via GPIO
void handle_contactors() {
  if (inverter && inverter->controls_contactor()) {
    datalayer.system.status.inverter_allows_contactor_closing = inverter->allows_contactor_closing();
  }

  auto bms_power_pin = esp32hal->BMS_POWER();

  if (bms_power_pin != GPIO_NUM_NC) {
    handle_BMSpower();  // Some batteries need to be periodically power cycled
  }

  for (int i = 1; i < MAX_BATTERIES; ++i) {
    if (contactor_control_enabled[i]) {
      handle_contactors_battery(i);
    }
  }

  if (contactor_control_enabled[0]) {
    precharge_fsm.tick(millis());
  }
}

void handle_contactors_battery(int instance) {
  bool engage = (precharge_fsm.state() == ContactorActuator::COMPLETED) &&
                datalayer.system.status.battery_link[instance].allowed_contactor_closing;

  set(extra_battery_contactors_pin(instance), engage ? ON : OFF);
  datalayer.system.status.battery_link[instance].contactors_engaged = engage;
}

/* PERIODIC_BMS_RESET - Once every configured interval (24h or 48h) we remove power from the BMS_power pin for 30 seconds.
The user can optionally defer the reset while SOC is low, and skip a single period while balancing.
REMOTE_BMS_RESET - Allows the user to remotely powercycle the BMS by sending a command to the emulator via MQTT.

This makes the BMS recalculate all SOC% and avoid memory leaks
During that time we also set the emulator state to paused in order to not try and send CAN messages towards the battery
Feature is only used if user has enabled PERIODIC_BMS_RESET */

void bms_power_off() {
  digitalWrite(esp32hal->BMS_POWER(), LOW);
  set_indicator_led(IndicatorLed::BMS_POWER, false);
}

void bms_power_on() {
  digitalWrite(esp32hal->BMS_POWER(), HIGH);
  set_indicator_led(IndicatorLed::BMS_POWER, true);
}

// Configured period between two automatic resets. Guarded to 24h in case the stored
// value is missing or nonsensical, see load_settings().
static uint32_t bms_reset_interval_ms() {
  uint32_t hours = periodic_bms_reset_interval_h;
  if (hours == 0) {
    hours = 24;
  }
  return (uint32_t)hours * 60UL * 60UL * 1000UL;
}

/* The two guards behave differently on purpose, so the decision is three-way rather than
   a plain yes/no. Only the periodic reset is affected, a remote reset requested by the user
   via MQTT is always carried out. */
enum class PeriodicResetVerdict {
  Run,        // Nothing in the way, reset now
  Defer,      // Stay due and reset as soon as the condition clears
  SkipPeriod  // Give up this occurrence, try again one full period later
};

/* Decides what should happen to a periodic reset whose interval has elapsed. On a non-Run
   verdict the reason is written to the out parameter for logging, and always points at a
   string literal. Note that an unknown SOC reads as 0 and therefore also defers the reset
   while the low SOC guard is enabled, which is the safe direction. */
static PeriodicResetVerdict periodic_bms_reset_verdict(const char** reason) {
  // Low SOC defers: there is little point recalibrating against a nearly empty pack, and we
  // want the reset to happen as soon as it is worthwhile rather than a whole period later.
  if (periodic_bms_reset_defer_low_soc) {
    if (datalayer.batteries[0].status.real_soc < BMS_RESET_DEFER_SOC_PPTT) {
      *reason = "real SOC below 15 percent";
      return PeriodicResetVerdict::Defer;
    }
    if (datalayer.batteries[0].status.reported_soc < BMS_RESET_DEFER_SOC_PPTT) {
      *reason = "scaled SOC below 15 percent";
      return PeriodicResetVerdict::Defer;
    }
  }

  /* Balancing costs the reset a single period. If balancing is still active when the next
     period comes around the reset goes ahead anyway, so a pack that reports balancing more
     or less permanently cannot suppress the reset indefinitely.
     Batteries that are not present report BALANCING_STATUS_UNKNOWN, so they never skip. */
  if (periodic_bms_reset_skip_balancing && !bms_reset.balancing_period_skipped) {
    static constexpr const char* balancing_reasons[MAX_BATTERIES] = {
        "balancing active on battery", "balancing active on battery 2", "balancing active on battery 3"};
    static_assert(MAX_BATTERIES == 3, "add the new instance's balancing reason");
    for (int i = 0; i < MAX_BATTERIES; ++i) {
      if (datalayer_battery(i).status.balancing_status == BALANCING_STATUS_ACTIVE) {
        *reason = balancing_reasons[i];
        return PeriodicResetVerdict::SkipPeriod;
      }
    }
  }

  return PeriodicResetVerdict::Run;
}

/* True when the configured off time outlasts the CAN liveness window and the reset therefore
   needs the counters held up. Only the off time matters here: the surrounding pause and warmup
   phases are short and the BMS is on the bus for part of them. To test the statement in Leaf 
   "GEN4_e_Battery_control_spec_ver1.0.pdf" page 4, "IGN to be OFF for more than 6 min 30 seconds every day. "*/
static bool bms_reset_needs_can_keepalive() {
  return datalayer.batteries[0].settings.user_set_bms_reset_duration_ms > BMS_RESET_CAN_KEEPALIVE_INTERVAL_MS;
}

/* Pretends the batteries were just heard from, and restarts the keepalive interval.
   Batteries that are not configured are skipped, since the safety layer does not look at
   their counters either. */
static void bms_reset_refresh_can_alive(uint32_t now) {
  bms_reset.last_can_keepalive_time = now;
  for (int i = 0; i < MAX_BATTERIES; ++i) {
    if (batteries[i]) {
      datalayer_battery(i).status.CAN_battery_still_alive = CAN_STILL_ALIVE;
    }
  }
}

// Called on every pass through the powered-off and powering-on states of a long reset.
static void bms_reset_can_keepalive_tick(uint32_t now) {
  if (!bms_reset_needs_can_keepalive()) {
    return;
  }
  if (now - bms_reset.last_can_keepalive_time < BMS_RESET_CAN_KEEPALIVE_INTERVAL_MS) {
    return;
  }
  bms_reset_refresh_can_alive(now);
}

void handle_BMSpower() {
  //Skip running the BMS reset state machine if equipment stop is active, as we don't want to powercycle the BMS during that time
  if (datalayer.system.info.equipment_stop_active) {
    return;
  }

  if (periodic_bms_reset || remote_bms_reset) {
    const uint32_t currentTime = millis();

    if (datalayer.system.status.bms_reset_status == BMS_RESET_IDLE) {
      // Idle state, no reset ongoing

      // Check if it's time to perform a periodic BMS reset
      if (periodic_bms_reset && currentTime - bms_reset.last_power_removal_time >= bms_reset_interval_ms()) {
        const char* reason = nullptr;
        switch (periodic_bms_reset_verdict(&reason)) {
          case PeriodicResetVerdict::Defer:
            /* Leave bms_reset.last_power_removal_time alone so the reset stays due. start_bms_reset()
               re-anchors it when the reset finally runs, which is what moves the following
               24h/48h periods to start from the deferred point. Logged once per episode,
               since this branch is evaluated on every loop while the reset is due. */
            if (!bms_reset.period_deferred) {
              bms_reset.period_deferred = true;
              logging.printf("BMS reset: Periodic reset deferred, %s.\n", reason);
            }
            break;

          case PeriodicResetVerdict::SkipPeriod:
            // Restarting the interval here is what turns this into a one period skip.
            bms_reset.last_power_removal_time = currentTime;
            bms_reset.balancing_period_skipped = true;
            logging.printf("BMS reset: Periodic reset skipped for one period, %s.\n", reason);
            break;

          case PeriodicResetVerdict::Run:
            if (bms_reset.period_deferred) {
              bms_reset.period_deferred = false;
              logging.printf("BMS reset: Running previously deferred periodic reset.\n");
            }
            bms_reset.balancing_period_skipped = false;  // Each reset restores the one period allowance
            start_bms_reset();
            break;
        }
      }
    } else if (datalayer.system.status.bms_reset_status == BMS_RESET_WAITING_FOR_PAUSE) {
      // We've already issued a pause, now we're waiting for that to take effect.

      int16_t battery_current_dA = datalayer.batteries[0].status.current_dA;
      int16_t battery2_current_dA = datalayer_battery(1).status.current_dA;  // Should be 0 if batteries[1] is null
      int16_t battery3_current_dA = datalayer_battery(2).status.current_dA;  // Should be 0 if batteries[2] is null

      if (
          // No current, safe to cut power
          (battery_current_dA == 0 && battery2_current_dA == 0 && battery3_current_dA == 0)
          // or reasonably low current and 5 seconds has passed
          || (abs(battery_current_dA) < 10 && abs(battery2_current_dA) < 10 && abs(battery3_current_dA) < 10 &&
              currentTime - bms_reset.last_power_removal_time >= 5000)) {

        bms_power_off();
        bms_reset.last_power_removal_time = currentTime;
        datalayer.system.status.bms_reset_status = BMS_RESET_POWERED_OFF;
      } else if (currentTime - bms_reset.last_power_removal_time >= 10000) {
        // There's still current, and we don't want to weld the contactors, so give up.

        datalayer.system.status.bms_reset_status = BMS_RESET_IDLE;
        set_event(EVENT_PERIODIC_BMS_RESET_FAILURE, 0);  // also printing a log entry
        clear_event(EVENT_PERIODIC_BMS_RESET_FAILURE);
      }
    } else if (datalayer.system.status.bms_reset_status == BMS_RESET_POWERED_OFF) {
      bms_reset_can_keepalive_tick(currentTime);

      // Check if the user configured duration has passed
      if (currentTime - bms_reset.last_power_removal_time >=
          datalayer.batteries[0].settings.user_set_bms_reset_duration_ms) {
        bms_power_on();
        bms_reset.power_on_time = currentTime;
        /* The last periodic refresh can have been up to a full interval ago, which would leave
           the BMS only a sliver of the window to get back on the bus. Refreshing here gives it
           the whole window from power-on, measured from the same moment for every off time. */
        if (bms_reset_needs_can_keepalive()) {
          bms_reset_refresh_can_alive(currentTime);
        }
        datalayer.system.status.bms_reset_status = BMS_RESET_POWERING_ON;
      }
    } else if (datalayer.system.status.bms_reset_status == BMS_RESET_POWERING_ON) {
      bms_reset_can_keepalive_tick(currentTime);

      // Wait for BMS to start up before unpausing
      if (currentTime - bms_reset.power_on_time >= bmsWarmupDuration) {
        // Unpause the battery
        setBatteryPause(false, false, EquipmentStop::UNCHANGED, false);

        // Reset is complete

        datalayer.system.status.bms_reset_status = BMS_RESET_IDLE;
        set_event(EVENT_PERIODIC_BMS_RESET, 0);
        clear_event(EVENT_PERIODIC_BMS_RESET);
      }
    }
  }
}

void start_bms_reset() {
  if (periodic_bms_reset || remote_bms_reset) {
    if (datalayer.system.status.bms_reset_status == BMS_RESET_IDLE) {
      // Record when we started the BMS reset process
      bms_reset.last_power_removal_time = millis();
      // Anchor the keepalive here so the first refresh lands one interval into the reset,
      // rather than immediately or after a gap left over from a previous reset.
      bms_reset.last_can_keepalive_time = bms_reset.last_power_removal_time;

      // Issue a pause, which should stop charge/discharge whilst the reset is ongoing
      setBatteryPause(true, false, EquipmentStop::UNCHANGED, false);

      if (contactor_control_enabled[0]) {
        // We power the contactors directly, so we can avoid closing/opening them
        // during reset.

        // Thus we can cut the BMS power now
        bms_power_off();

        // and jump straight to powered off state, no need to wait.
        datalayer.system.status.bms_reset_status = BMS_RESET_POWERED_OFF;
      } else {
        // The BMS powers the contactors, so we need to wait for the pause to
        // take effect before cutting power to it, or the contactors might drop
        // out under load and be damaged.

        datalayer.system.status.bms_reset_status = BMS_RESET_WAITING_FOR_PAUSE;
      }
    }
  }
}

// Decide whether a specific candidate pin should be latched. Only latch pins the firmware
// is actively driving to a defined level — otherwise there's nothing meaningful to preserve,
// and latching a floating/undriven pin could freeze it in an unwanted state.
static bool should_hold_pin(gpio_num_t pin) {
  if (pin == GPIO_NUM_NC) {
    return false;
  }
  if (pin == esp32hal->BMS_POWER()) {
    return bms_power_is_active();
  }
  return false;  // unknown pins are not held until explicitly supported
}

void hold_pins_across_reset() {
  const auto pins = esp32hal->reset_hold_pins();
  if (pins.empty()) {
    return;
  }
#ifndef UNIT_TEST
  bool any_held = false;
  for (auto pin : pins) {
    if (should_hold_pin(pin)) {
      gpio_hold_en(pin);  // freeze the pad at its current (driven) level
      any_held = true;
    }
  }
  if (any_held) {
    gpio_deep_sleep_hold_en();  // keep the hold(s) engaged through the reset
  }
#endif
}

void release_pins_across_reset() {
#ifndef UNIT_TEST
  // Release every candidate pin unconditionally. gpio_hold_dis() is a no-op on a pin that
  // isn't held, so this also clears a stale hold left over from a previous session in which
  // the feature was enabled but is now disabled.
  for (auto pin : esp32hal->reset_hold_pins()) {
    if (pin != GPIO_NUM_NC) {
      gpio_hold_dis(pin);
    }
  }
#endif
}

#ifdef UNIT_TEST
void reset_bms_reset_state() {
  bms_reset = BmsResetState();
}
#endif
