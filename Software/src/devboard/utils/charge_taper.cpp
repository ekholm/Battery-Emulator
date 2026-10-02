#include "charge_taper.h"
#include "../../datalayer/datalayer.h"
#include "../../inverter/INVERTERS.h"
#include "logging.h"

/* Linear charge power taper over the top of the SOC window: full power at
   (100.00% - band), reaching 0W at 100.00% scaled SOC. Battery integration
   independent - operates on the datalayer limits every battery driver rewrites
   each cycle, so in-place scaling never compounds. Runs AFTER
   update_machineryprotection() so its input includes safety-layer zeroing, and
   BEFORE filter_inverter_limits() so the reduction passes through the low pass
   filter instantly (decrease path) while recovery after SOC falls back below
   the band gets the ramp. Independent from the low pass filter setting - the
   user can enable none, either or both. */
void filter_charge_taper_soc(void) {
  static bool taper_engaged_logged = false;
  if (!charge_taper_soc || charge_taper_band_pptt == 0) {
    return;  // Datalayer values pass through untouched
  }
  uint16_t soc = datalayer.aggregate.reported_soc;  // Computed earlier this cycle in update_aggregate_values()
  if (soc > 10000) {
    soc = 10000;  // Defensive: keep (10000 - soc) non-negative if a driver misreports with SOC scaling disabled
  }
  uint16_t band = charge_taper_band_pptt;
  if (band > 10000) {
    band = 10000;
  }
  if (soc >= (10000 - band)) {
    uint32_t charge_W = datalayer.battery.status.max_charge_power_W;

    /* Apply the effective current cap (remote if active, otherwise user, same
       precedence as update_calculated_values()) to the taper input, so the
       band acts on the limit the inverter actually sees instead of on BMS
       headroom above the cap. Idempotent with the min() applied again in
       filter_inverter_limits() and driver-side by BYD-Modbus. */
    uint16_t voltage_dV = datalayer.battery.status.voltage_dV;
    if (voltage_dV <= 10) {
      voltage_dV = datalayer.battery.info.max_design_voltage_dV;
    }
    if (voltage_dV > 10) {
      uint16_t cap_dA = datalayer.battery_settings.remote_settings_limit_charge
                            ? datalayer.battery_settings.max_remote_set_charge_dA
                            : datalayer.battery_settings.max_user_set_charge_dA;
      uint32_t cap_W = ((uint32_t)cap_dA * voltage_dV) / 100;
      if (charge_W > cap_W) {
        charge_W = cap_W;
      }
    }

    uint32_t base_W = charge_W;  // Allowance entering the taper, after the user/remote cap
    charge_W = (charge_W * (uint32_t)(10000 - soc)) / band;

    /* Optional float charge power: hold a minimum charge power through the
       tail of the band, dropping to 0W only at 100.00% scaled SOC. Keeps
       inverters above their minimum stable charging power (avoids standby
       cycling), provides a steady balancing trickle, and guarantees the
       charge session terminates instead of asymptotically stalling below
       full. Only applied when the allowance entering the taper is non-zero,
       so it never overrides a zero coming from the BMS itself or from the
       safety layer (cell overvoltage, battery full, pause states). */
    if (base_W > 0 && charge_taper_floor_W > 0 && soc < 10000 && charge_W < charge_taper_floor_W) {
      charge_W = charge_taper_floor_W;
    }

    datalayer.battery.status.max_charge_power_W = charge_W;

    /* Pull the derived current limit down too, so current-based inverters are
       covered when the low pass filter is disabled. min() only - the
       user/remote/safety clamps already applied to the _dA field are never
       raised. */
    if (voltage_dV > 10) {
      uint32_t charge_dA_from_power = (charge_W * 100) / voltage_dV;
      if (charge_dA_from_power < datalayer.battery.status.max_charge_current_dA) {
        datalayer.battery.status.max_charge_current_dA = (uint16_t)charge_dA_from_power;
      }
    }

    if (!taper_engaged_logged) {
      logging.printf("Charge power tapering based on SOC engaged at %.1f (real) %.1f (scaled)\n",
                     datalayer.battery.status.real_soc / 100.0f, soc / 100.0f);
      taper_engaged_logged = true;
    }
  } else if (soc < (10000 - band - 50)) {  // 0.5% hysteresis before re-arming the log entry
    taper_engaged_logged = false;
  }
}
