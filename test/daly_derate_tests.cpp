#include <gtest/gtest.h>

#include <initializer_list>

#include "../Software/src/battery/BATTERIES.h"
#include "../Software/src/battery/DALY-BMS.h"
#include "../Software/src/datalayer/datalayer.h"

#include "emul/HardwareSerial.h"

/* The Daly derates, at the two limits they exist to protect.
 *
 * update_values() tightens discharge power as the pack approaches its minimum
 * discharge voltage, and charge power as the SOC approaches full. Both headrooms
 * are signed differences that go NEGATIVE the moment the pack crosses the limit
 * - which is a normal state, not a fault - and both were computed unsigned, so
 * the product wrapped to about four billion and the clamp under it stopped
 * firing. The derate did not merely get the wrong number: it disengaged.
 *
 * The cases drive the driver's real RS485 parser and then assert the POWER that
 * comes out, because the pack state lives in file statics that nothing else can
 * reach. Each one WALKS THE BOUNDARY - an in-band value, the exact boundary, and
 * one or two values past it. That is not thoroughness for its own sake: most of
 * the assertions expect zero, and a derate pinned to zero everywhere would
 * satisfy all of them while being a different and equally wrong driver. The
 * in-band arm is the only thing standing between the case and that reading.
 */

namespace {

// One Daly reply frame, framed and checksummed as the driver's parser expects.
void daly_reply(uint8_t command, std::initializer_list<uint8_t> data) {
  uint8_t packet[13] = {0xA5, 0x01, command, 8};
  uint8_t i = 4;
  for (uint8_t b : data) {
    if (i >= 12) {
      break;
    }
    packet[i++] = b;
  }
  uint8_t checksum = 0;
  for (uint8_t j = 0; j < 12; j++) {
    checksum += packet[j];
  }
  packet[12] = checksum;
  for (uint8_t b : packet) {
    Serial2.inject_rx({b});
  }
}

/* A Daly driver fed one full set of replies, so update_values() runs against a
 * defined pack rather than whatever the previous case left in the file statics.
 * Those statics are the reason every call sets all four: they persist across
 * tests, and a case that set only what it cared about would depend on order. */
void daly_pack(DalyBms& bms, uint16_t voltage_dV, uint16_t soc_pptt, int8_t temperature_C) {
  Serial2.clear_rx();
  // The BMS reports SOC in tenths of a percent and the driver scales by 10, so
  // the wire field is the pptt the test asks for divided by 10.
  const uint16_t soc_raw = soc_pptt / 10;
  daly_reply(0x90, {(uint8_t)(voltage_dV >> 8), (uint8_t)voltage_dV, 0, 0, 0x75, 0x30, (uint8_t)(soc_raw >> 8),
                    (uint8_t)soc_raw});  // 0x7530 = 30000 = zero current
  daly_reply(0x92, {(uint8_t)(temperature_C + 40), 0, (uint8_t)(temperature_C + 40), 0, 0, 0, 0, 0});
  bms.receive();

  datalayer.battery.info.min_design_voltage_dV = 3000;
  datalayer.battery.settings.user_set_voltage_limits_active = false;
  datalayer.battery.settings.max_user_set_charge_dA = 1000;     // 100 A
  datalayer.battery.settings.max_user_set_discharge_dA = 1000;  // 100 A

  /* The parser is the only route to those file statics, and it runs on bytes
     taken off Serial2 - so if the host serial emulation ever stops delivering
     them, receive() reads nothing and the statics keep whatever the last pack
     left. The cases below DO still fail in that state, which is why "they still
     fail" cannot be the check: they fail on their IN-BAND arm instead of their
     past-the-boundary arm, and that reads as a derate defect when the derate is
     fine and the rig is broken. Assert the ingestion here, where the cause can
     be named. */
  bms.update_values();
  ASSERT_EQ(datalayer.battery.status.voltage_dV, voltage_dV)
      << "the pack never reached the driver: Serial2's RX queue delivered no bytes to the parser, "
         "so every assertion below would be about stale state";
}

/* The five derate parameters are globals shared by the whole suite, as are the
 * datalayer fields daly_pack() writes. Put every one of them back. */
class DalyDerate : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_per_percent = user_selected_daly_power_per_percent;
    saved_per_dV = user_selected_daly_power_per_dV;
    saved_per_dV_start = user_selected_daly_power_per_dV_start;
    saved_per_degree_C = user_selected_daly_power_per_degree_C;
    saved_at_0_degree_C = user_selected_daly_power_at_0_degree_C;
    saved_min_design_voltage_dV = datalayer.battery.info.min_design_voltage_dV;
    saved_user_limits_active = datalayer.battery.settings.user_set_voltage_limits_active;
    saved_user_discharge_voltage_dV = datalayer.battery.settings.max_user_set_discharge_voltage_dV;
    saved_charge_dA = datalayer.battery.settings.max_user_set_charge_dA;
    saved_discharge_dA = datalayer.battery.settings.max_user_set_discharge_dA;
  }

  void TearDown() override {
    Serial2.clear_rx();
    user_selected_daly_power_per_percent = saved_per_percent;
    user_selected_daly_power_per_dV = saved_per_dV;
    user_selected_daly_power_per_dV_start = saved_per_dV_start;
    user_selected_daly_power_per_degree_C = saved_per_degree_C;
    user_selected_daly_power_at_0_degree_C = saved_at_0_degree_C;
    datalayer.battery.info.min_design_voltage_dV = saved_min_design_voltage_dV;
    datalayer.battery.settings.user_set_voltage_limits_active = saved_user_limits_active;
    datalayer.battery.settings.max_user_set_discharge_voltage_dV = saved_user_discharge_voltage_dV;
    datalayer.battery.settings.max_user_set_charge_dA = saved_charge_dA;
    datalayer.battery.settings.max_user_set_discharge_dA = saved_discharge_dA;
  }

 private:
  int saved_per_percent = 0;
  int saved_per_dV = 0;
  int saved_per_dV_start = 0;
  int saved_per_degree_C = 0;
  int saved_at_0_degree_C = 0;
  uint16_t saved_min_design_voltage_dV = 0;
  bool saved_user_limits_active = false;
  uint16_t saved_user_discharge_voltage_dV = 0;
  uint16_t saved_charge_dA = 0;
  uint16_t saved_discharge_dA = 0;
};

}  // namespace

TEST_F(DalyDerate, TheVoltageDerateHoldsAtZeroOnceThePackIsUnderTheMinimum) {
  /* The derate tightens to zero as the pack approaches its minimum discharge
   * voltage. Crossing that minimum is a normal end-of-discharge state, and the
   * headroom is then NEGATIVE - so the case walks the pack across the boundary
   * rather than only sampling one side of it. A headroom cast to unsigned before
   * the multiply wraps to about four billion, which no longer clamps anything,
   * and the derate disengages completely in the unsafe direction.
   *
   * The temperature and SOC arms are pushed out of the way so a zero here can
   * only have come from the voltage arm. */
  user_selected_daly_power_per_dV_start = 20;
  user_selected_daly_power_per_dV = 50;
  user_selected_daly_power_per_degree_C = 0;
  user_selected_daly_power_at_0_degree_C = 100000;

  DalyBms bms;
  daly_pack(bms, 3001, 5000, 25);  // 1 dV of headroom over the 3000 dV minimum
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 1u * 50u) << "inside the band, still derating";

  daly_pack(bms, 3000, 5000, 25);  // exactly at the minimum
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u) << "zero headroom is zero power";

  daly_pack(bms, 2999, 5000, 25);  // one decivolt BELOW the minimum
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u)
      << "past the minimum the derate must HOLD at zero, not wrap and release";

  daly_pack(bms, 2000, 5000, 25);  // far below, where the wrapped product is largest
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u) << "and it stays held however far past";
}

TEST_F(DalyDerate, TheVoltageDerateHoldsAtZeroAgainstTheUsersOwnDischargeLimit) {
  /* The same boundary, reached through the OTHER min_voltage.
   *
   * `min_voltage` is the user's `max_user_set_discharge_voltage_dV` whenever the
   * user limits are active and that value sits above the design minimum, and
   * that is the path this defect is actually met on: a driver who sets a
   * conservative discharge floor crosses it in normal operation, where the
   * design floor is reached only at the bottom of the pack. The sibling case
   * above drives the design minimum, so nothing held the clamp to the SELECTED
   * minimum rather than to a second reading of `min_design_voltage_dV` - which
   * is a refactor away and would restore the release for exactly the users who
   * asked for the tighter limit.
   *
   * `daly_pack()` clears `user_set_voltage_limits_active`, so the two settings
   * fields are re-armed after each pack rather than once before the loop. */
  user_selected_daly_power_per_dV_start = 20;
  user_selected_daly_power_per_dV = 50;
  user_selected_daly_power_per_degree_C = 0;
  user_selected_daly_power_at_0_degree_C = 100000;

  DalyBms bms;
  auto pack_with_user_limit = [&bms](uint16_t voltage_dV) {
    daly_pack(bms, voltage_dV, 5000, 25);
    datalayer.battery.settings.user_set_voltage_limits_active = true;
    datalayer.battery.settings.max_user_set_discharge_voltage_dV = 3400;  // above the 3000 design floor
    bms.update_values();
  };

  pack_with_user_limit(3401);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 1u * 50u)
      << "the derate is measured from the USER's limit, not the design minimum";

  pack_with_user_limit(3400);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u) << "zero headroom is zero power";

  pack_with_user_limit(3399);
  EXPECT_EQ(datalayer.battery.status.max_discharge_power_W, 0u)
      << "past the user's limit the derate must HOLD at zero, 401 dV above the design floor";
}

TEST_F(DalyDerate, TheSocDerateHoldsAtZeroOnceTheBmsReportsOverFull) {
  /* The high-SOC arm is 10000 - SOC, in pptt, and a Daly reporting over 100 %
   * makes that difference negative. It is computed in unsigned, so the same wrap
   * releases the CHARGE limit at exactly the state of charge the arm exists to
   * protect. The wire field is tenths of a percent, so 100.1 % is one count past
   * full and needs no fault to produce.
   *
   * The voltage and temperature arms are pushed out of the way. */
  user_selected_daly_power_per_percent = 100;
  user_selected_daly_power_per_dV_start = 0;
  user_selected_daly_power_per_degree_C = 0;
  user_selected_daly_power_at_0_degree_C = 100000;

  DalyBms bms;
  daly_pack(bms, 3500, 9900, 25);  // 99 % - inside the band, below full
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, (10000u - 9900u) * 100u / 100u)
      << "inside the band, still derating";

  daly_pack(bms, 3500, 10000, 25);  // exactly full
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u) << "at full the charge power is zero";

  daly_pack(bms, 3500, 10100, 25);  // 100.1 % on the wire
  bms.update_values();
  EXPECT_EQ(datalayer.battery.status.max_charge_power_W, 0u)
      << "over full the derate must HOLD at zero, not wrap and release";
}
