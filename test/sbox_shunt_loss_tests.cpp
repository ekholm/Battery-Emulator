#include <gtest/gtest.h>

#include <Arduino.h>  // Emul: set_millis64() to control the test clock

#include <vector>

#include "../Software/src/datalayer/datalayer.h"
#include "../Software/src/devboard/utils/events.h"
#include "../Software/src/shunt/BMW-SBOX.h"

// The S-BOX contactor sequence decides two things from shunt voltages:
//
//   DISCONNECTED -> PRECHARGE   needs measured_voltage_mV above a threshold
//   POSITIVE -> PRECHARGE_OFF   needs measured_voltage * 0.99 < measured_outvoltage,
//                               which is the PRECHARGE-COMPLETE decision
//
// Both operands arrive in CAN frames (0x210 and 0x220) and are never cleared.
// When the shunt stops sending they keep their last values forever, so the
// second decision can be satisfied by a pair of numbers nobody is measuring any
// more - and the emulator closes the positive contactor believing precharge
// finished. datalayer.shunt.available already knows the shunt is gone; the
// sequence simply did not consult it.
//
// Assertions are on datalayer state and on the relay byte actually put on the
// wire, not on the private FSM enum: those two are the observable contract.

extern std::vector<CAN_frame> g_emul_transmitted_frames;
void clear_transmitted_frames();

namespace {

// Relay patterns written into SBOX_100 byte 0 by the production code.
constexpr uint8_t kAllOpen = 0x55;
constexpr uint8_t kPrechargeOnly = 0x86;
constexpr uint8_t kPrechargeNegative = 0xA6;
constexpr uint8_t kAllThree = 0xAA;
constexpr uint8_t kNegativePositive = 0x6A;

// From BMW-SBOX.h. Kept in step deliberately: if the ladder is retimed these
// tests must be revisited rather than silently pass.
constexpr unsigned long kT1 = 5000;  // precharge -> negative
constexpr unsigned long kT2 = 5000;  // negative -> positive
constexpr unsigned long kShuntTimeout = 1000;

// A live pack, comfortably over the 250 V start threshold.
constexpr uint32_t kPackMv = 300000;
// An output that satisfies voltage * 0.99 < outvoltage, i.e. reads as
// precharge-complete. This is the pair that must NOT be trusted once stale.
constexpr uint32_t kOutputCompleteMv = 299000;
constexpr uint32_t kOutputEmptyMv = 1000;

CAN_frame volts(uint32_t id, uint32_t mv) {
  CAN_frame f = {.FD = false, .ext_ID = false, .DLC = 8, .ID = id, .data = {}};
  f.data.u8[0] = mv & 0xFF;
  f.data.u8[1] = (mv >> 8) & 0xFF;
  f.data.u8[2] = (mv >> 16) & 0xFF;
  return f;
}

class SboxShuntLossTest : public ::testing::Test {
 protected:
  void SetUp() override {
    set_millis64(kBoot);
    now = kBoot;
    datalayer.system.status.battery_allows_contactor_closing = true;
    datalayer.system.status.inverter_allows_contactor_closing = true;
    datalayer.system.info.equipment_stop_active = false;
    clear_transmitted_frames();
  }

  // Deliver both voltage frames, which is also what keeps the shunt "seen".
  void feed_shunt(uint32_t out_mv) {
    sbox.handle_incoming_can_frame(volts(0x210, kPackMv));
    sbox.handle_incoming_can_frame(volts(0x220, out_mv));
  }

  // Advance the clock and run one 10 ms tick of the sequence.
  void tick(unsigned long by_ms = 20) {
    now += by_ms;
    set_millis64(now);
    clear_transmitted_frames();
    sbox.transmit_can(now);
  }

  // The relay pattern the last tick actually transmitted, or 0 if it sent none.
  uint8_t relays() const {
    for (const CAN_frame& f : g_emul_transmitted_frames) {
      if (f.ID == 0x100) {
        return f.data.u8[0];
      }
    }
    return 0;
  }

  // Drive as far as POSITIVE: the state whose exit decision reads the voltages.
  void reach_positive_with_output(uint32_t out_mv) {
    feed_shunt(out_mv);
    tick();  // DISCONNECTED -> PRECHARGE -> (switch) precharge relay on
    ASSERT_EQ(relays(), kPrechargeOnly) << "the sequence did not start";
    for (unsigned long elapsed = 0; elapsed <= kT1; elapsed += 200) {
      feed_shunt(out_mv);
      tick(200);
    }
    ASSERT_EQ(relays(), kPrechargeNegative) << "the negative relay never engaged";
  }

  static constexpr unsigned long kBoot = 100000;
  unsigned long now = kBoot;
  BmwSbox sbox;
};

}  // namespace

// ── Starting the sequence ────────────────────────────────────────────────────

TEST_F(SboxShuntLossTest, TheSequenceStartsWhenTheShuntIsLive) {
  feed_shunt(kOutputEmptyMv);
  tick();
  EXPECT_EQ(relays(), kPrechargeOnly);
}

TEST_F(SboxShuntLossTest, TheSequenceDoesNotStartOnAFrozenVoltageFromADeadShunt) {
  // One frame, then silence. measured_voltage_mV keeps the value forever and
  // still reads as a healthy 300 V pack; only `available` knows better.
  feed_shunt(kOutputEmptyMv);
  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);

  EXPECT_FALSE(datalayer.shunt.available) << "the test did not actually make the shunt stale";
  EXPECT_EQ(datalayer.shunt.measured_voltage_mV, kPackMv) << "the stale reading is still there, which is the point";
  EXPECT_EQ(relays(), kAllOpen) << "starting a contactor sequence on a reading nobody is taking";
  // The discriminator between guarding the START and only guarding the
  // MIDDLE. Without the start gate the sequence briefly enters PRECHARGE and
  // the abort below immediately undoes it - same relays, same datalayer, but a
  // contactor-sequence-abandoned ERROR raised on every tick for a sequence that
  // never began. Verified: dropping the availability term from the start
  // condition makes this line fail and nothing else.
  EXPECT_EQ(get_event_pointer(EVENT_SHUNT_LOST_DURING_PRECHARGE)->state, EVENT_STATE_INACTIVE)
      << "nothing was aborted here - the sequence never started, and saying otherwise cries wolf";
}

// ── Losing the shunt part way through, which is the dangerous case ───────────

TEST_F(SboxShuntLossTest, LosingTheShuntMidSequenceOpensEverythingAndAbandons) {
  reach_positive_with_output(kOutputEmptyMv);
  EXPECT_TRUE(datalayer.shunt.precharging);

  // Stop feeding. The precharge resistor is carrying current right now.
  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);

  EXPECT_EQ(relays(), kAllOpen) << "the precharge resistor must not be left energised on a dead sensor";
  EXPECT_FALSE(datalayer.shunt.precharging);
  EXPECT_FALSE(datalayer.shunt.contactors_engaged);
  EXPECT_FALSE(datalayer.system.status.dc_bus_live);
  EXPECT_EQ(get_event_pointer(EVENT_SHUNT_LOST_DURING_PRECHARGE)->state, EVENT_STATE_ACTIVE)
      << "abandoning a contactor sequence must be reported, not silent";
}

// THE DEFECT, in its exact shape. The last pair the shunt sent happens to
// satisfy voltage * 0.99 < outvoltage. Frozen, that reads as precharge-complete
// forever - so before this fix the positive contactor closed on a measurement
// that had stopped being taken.
TEST_F(SboxShuntLossTest, AStalePairThatReadsAsPrechargeCompleteDoesNotCloseThePositiveContactor) {
  reach_positive_with_output(kOutputCompleteMv);

  // The inequality is satisfied by what is in the datalayer right now.
  ASSERT_LT(datalayer.shunt.measured_voltage_mV * 0.99, datalayer.shunt.measured_outvoltage_mV)
      << "this test is only meaningful if the stale pair would otherwise advance the sequence";

  // Let T2 pass with the shunt silent, so the only thing stopping the advance
  // is the availability check.
  now += kT2 + kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);

  EXPECT_NE(relays(), kAllThree) << "the positive contactor closed on a precharge-complete decision made from "
                                    "two numbers that stopped being measured";
  EXPECT_EQ(relays(), kAllOpen);
  EXPECT_FALSE(datalayer.system.status.dc_bus_live);
}

// The same pair, with the shunt still alive, MUST advance - or the fix has
// simply broken precharge rather than guarded it.
TEST_F(SboxShuntLossTest, ALivePairThatReadsAsPrechargeCompleteStillAdvances) {
  reach_positive_with_output(kOutputCompleteMv);
  for (unsigned long elapsed = 0; elapsed <= kT2; elapsed += 200) {
    feed_shunt(kOutputCompleteMv);
    tick(200);
  }
  EXPECT_EQ(relays(), kAllThree) << "a live shunt reporting a completed precharge must still close the positive";
  EXPECT_FALSE(datalayer.shunt.precharging);
}

// ── Recovery, and the case deliberately left alone ───────────────────────────

TEST_F(SboxShuntLossTest, TheSequenceCanStartAgainOnceTheShuntComesBack) {
  reach_positive_with_output(kOutputEmptyMv);
  now += kShuntTimeout + 100;
  set_millis64(now);
  sbox.transmit_can(now);
  ASSERT_FALSE(datalayer.shunt.available);

  feed_shunt(kOutputEmptyMv);
  tick();
  EXPECT_EQ(relays(), kPrechargeOnly) << "abandoning must be recoverable, not a latch";
}

TEST_F(SboxShuntLossTest, ACompletedSequenceIsNotOpenedByALostShunt) {
  // Deliberate: with the contactors closed and the bus live, opening the
  // positive under load to react to a lost sensor is a worse hazard than the
  // one being avoided. This pins that choice so it is visible rather than
  // accidental - if it is ever reversed, it should be reversed on purpose.
  reach_positive_with_output(kOutputCompleteMv);
  for (unsigned long elapsed = 0; elapsed <= kT2 + 2500; elapsed += 200) {
    feed_shunt(kOutputCompleteMv);
    tick(200);
  }
  ASSERT_EQ(relays(), kNegativePositive) << "the sequence did not reach COMPLETED";
  ASSERT_TRUE(datalayer.system.status.dc_bus_live);

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);

  EXPECT_EQ(relays(), kNegativePositive) << "a completed sequence must not drop its contactors under load";
  EXPECT_TRUE(datalayer.system.status.dc_bus_live);
}

// ── What the ERROR event does after the abort ────────────────────────────────
//
// These need the events to carry their PRODUCTION levels, which the fixture now
// applies. It did not until this branch: reset_all_events() clears every entry's
// state but never its level, and init_events() - the only thing that assigns the
// 171 levels - was never called, so the whole suite ran with every level at 0.
// EVENT_SHUNT_LOST_DURING_PRECHARGE is EVENT_LEVEL_ERROR, and that is what makes
// the difference between an abandoned sequence and a latched one.

TEST_F(SboxShuntLossTest, TheAbortDrivesTheSystemIntoFault) {
  reach_positive_with_output(kOutputEmptyMv);

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);

  ASSERT_EQ(relays(), kAllOpen) << "the abort did not fire";
  EXPECT_EQ(get_event_pointer(EVENT_SHUNT_LOST_DURING_PRECHARGE)->level, EVENT_LEVEL_ERROR);
  EXPECT_EQ(datalayer.system.status.system_status, FAULT)
      << "an ERROR-level event puts the system in FAULT (events.cpp update_bms_status)";
}

TEST_F(SboxShuntLossTest, AMomentaryShuntDropoutRecoversAndDoesNotLatch) {
  // The abort is documented as recoverable, and this is what that has to mean.
  // The chain it has to survive is two files long:
  //
  //   abort -> set_event(ERROR) -> events.level = ERROR
  //         -> update_bms_status() -> system_status = FAULT
  //         -> this driver's tick counts FAULT ticks
  //         -> > MAX_ALLOWED_FAULT_TICKS (2000 = 20 s) -> SHUTDOWN_REQUESTED,
  //            which this file documents as needing a power cycle.
  //
  // So an event left standing after the shunt returns turns a one-second
  // dropout into an unrecoverable shutdown twenty seconds later, MID-SEQUENCE,
  // with a perfectly healthy shunt. Clearing it on recovery is what keeps the
  // word "recoverable" true.
  reach_positive_with_output(kOutputEmptyMv);

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);
  ASSERT_EQ(relays(), kAllOpen) << "the abort did not fire";
  ASSERT_EQ(datalayer.system.status.system_status, FAULT) << "the ERROR event should have faulted the system";

  for (int i = 0; i < 300; ++i) {  // gone for three seconds
    tick(10);
  }

  feed_shunt(kOutputEmptyMv);
  tick();
  EXPECT_EQ(relays(), kPrechargeOnly) << "the sequence must restart once the shunt is back";
  EXPECT_EQ(datalayer.system.status.system_status, ACTIVE)
      << "the abandonment event must be cleared when the shunt returns, or FAULT outlives its cause";

  for (int i = 0; i < 2200; ++i) {  // twenty-two more seconds, all healthy
    feed_shunt(kOutputEmptyMv);
    tick(10);
  }

  EXPECT_EQ(datalayer.system.status.system_status, ACTIVE) << "a healed dropout must not leave the system faulted";
  EXPECT_NE(relays(), 0) << "SHUTDOWN_REQUESTED returns before transmitting - the sequence must not be latched off "
                            "after the shunt has been healthy for twenty-two seconds";
}

TEST_F(SboxShuntLossTest, AShuntThatStaysDeadStillLatches) {
  // The other half, and the reason the clear is on RECOVERY rather than right
  // after the event is raised. A shunt that never comes back leaves the ERROR
  // standing, the fault counter runs, and the machine latches - which is the
  // correct outcome and is also the flap protection that would otherwise be
  // lost: a shunt cycling in and out cannot cycle a precharge resistor for ever,
  // because a dropout it does not recover from ends in the latch.
  reach_positive_with_output(kOutputEmptyMv);

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);
  ASSERT_EQ(relays(), kAllOpen) << "the abort did not fire";

  for (int i = 0; i < 2500; ++i) {  // twenty-five seconds of continued silence
    tick(10);
  }

  EXPECT_EQ(datalayer.system.status.system_status, FAULT);
  EXPECT_EQ(relays(), 0) << "a shunt that stays away must end in the latch";
}

// ── COMPLETED: report, do not switch ─────────────────────────────────────────

TEST_F(SboxShuntLossTest, ACompletedSequenceReportsTheLostShunt) {
  // The contactors stay closed - that is asserted next door. What was missing is
  // the other half of the same sentence: the case wants REPORTING, and nothing
  // anywhere reported it. datalayer.shunt.available had exactly two consumers in
  // the whole tree and neither of them said a word about a live bus that has
  // stopped being measured.
  reach_positive_with_output(kOutputCompleteMv);
  for (unsigned long elapsed = 0; elapsed <= kT2 + 2500; elapsed += 200) {
    feed_shunt(kOutputCompleteMv);
    tick(200);
  }
  ASSERT_EQ(relays(), kNegativePositive) << "the sequence did not complete";

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);

  EXPECT_EQ(get_event_pointer(EVENT_SHUNT_LOST_WITH_BUS_LIVE)->state, EVENT_STATE_ACTIVE)
      << "a live bus that has lost its current measurement must say so";
  EXPECT_EQ(relays(), kNegativePositive) << "and must not open the contactors under load to do it";
}

TEST_F(SboxShuntLossTest, TheBusLiveReportClearsWhenTheShuntReturns) {
  // The same stale-event defect this branch fixes for the ERROR, one level
  // down: a warning that outlives its cause is the event system crying wolf,
  // and the operator who has learned to ignore it will ignore the one that is
  // current. Found by mutation - removing only this clear left the whole suite
  // green.
  reach_positive_with_output(kOutputCompleteMv);
  for (unsigned long elapsed = 0; elapsed <= kT2 + 2500; elapsed += 200) {
    feed_shunt(kOutputCompleteMv);
    tick(200);
  }
  ASSERT_EQ(relays(), kNegativePositive) << "the sequence did not complete";

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);
  ASSERT_EQ(get_event_pointer(EVENT_SHUNT_LOST_WITH_BUS_LIVE)->state, EVENT_STATE_ACTIVE)
      << "the report did not fire, so this test would prove nothing";

  feed_shunt(kOutputCompleteMv);
  tick();
  EXPECT_EQ(get_event_pointer(EVENT_SHUNT_LOST_WITH_BUS_LIVE)->state, EVENT_STATE_INACTIVE)
      << "the shunt is back and measuring - a warning left standing outlives its cause";
  EXPECT_EQ(relays(), kNegativePositive) << "recovery must not disturb the closed contactors";
}

TEST_F(SboxShuntLossTest, TheBusLiveReportIsWarningSoItCannotOpenTheContactorsItRefusedToOpen) {
  // The trap in completing that sentence the obvious way. Reporting COMPLETED
  // with the ERROR event would drive system_status into FAULT, and this driver
  // turns sustained FAULT into SHUTDOWN_REQUESTED - dropping the positive
  // contactor under load after all, by the exact path the COMPLETED exclusion
  // exists to avoid. WARNING reports without faulting.
  EXPECT_EQ(get_event_pointer(EVENT_SHUNT_LOST_WITH_BUS_LIVE)->level, EVENT_LEVEL_WARNING);

  reach_positive_with_output(kOutputCompleteMv);
  for (unsigned long elapsed = 0; elapsed <= kT2 + 2500; elapsed += 200) {
    feed_shunt(kOutputCompleteMv);
    tick(200);
  }
  ASSERT_EQ(relays(), kNegativePositive);

  now += kShuntTimeout + 100;
  set_millis64(now);
  clear_transmitted_frames();
  sbox.transmit_can(now);
  for (int i = 0; i < 2500; ++i) {  // twenty-five seconds of a live bus, no shunt
    tick(10);
  }

  EXPECT_NE(datalayer.system.status.system_status, FAULT) << "the bus-live report must not fault the system";
  EXPECT_EQ(relays(), kNegativePositive) << "the contactors must still be closed after twenty-five seconds";
}
