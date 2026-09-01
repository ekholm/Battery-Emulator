#include <gtest/gtest.h>

#include "../Software/src/lib/mcp2515_lite/mcp2515_timing.h"

/* The MCP2515 took a bitrate its oscillator cannot make
 * and reported success.
 *
 * These are REAL tests, not source scans. That is half the point of the item:
 * the arithmetic was a file-static in a translation unit that reaches for
 * Arduino and FreeRTOS, so nothing could call it and every claim about it -
 * including one written into a comment that had to be corrected - was a
 * claim about code someone had read. It is pure integer arithmetic over two
 * arguments and now lives in its own TU, so it gets asserted instead.
 *
 * The CNF encoding pinned below is the vendored driver's, unchanged here:
 *   CNF1 = BRP - 1
 *   TQ=8  layout -> CNF2 0x8A, CNF3 0x01
 *   TQ=16 layout -> CNF2 0xA5, CNF3 0x03
 * and the achieved rate is f_osc / (2 * TQ_per_bit * BRP) - one time quantum is
 * 2*BRP oscillator periods on this part, which is where the 32 and the 16 in
 * the driver's own divisors come from.
 */
namespace {

constexpr uint32_t OSC_8MHZ = 8000000;
constexpr uint32_t OSC_16MHZ = 16000000;

// Every rate CAN_Speed offers, in bit/s (comm_can.h).
constexpr uint32_t CAN_SPEED_ENUM_BPS[] = {100000, 125000, 200000, 250000, 500000, 800000, 1000000};

struct Timing {
  bool ok;
  uint8_t cnf[3];
  uint32_t achieved;
};

Timing compute(uint32_t f_osc, uint32_t rate) {
  Timing t = {};
  t.cnf[0] = 0xEE;
  t.cnf[1] = 0xEE;
  t.cnf[2] = 0xEE;
  t.ok = mcp2515_calculate_timing(f_osc, rate, t.cnf, &t.achieved);
  return t;
}

uint32_t brp_of(const Timing& t) {
  return static_cast<uint32_t>(t.cnf[0]) + 1;
}

uint32_t tq_of(const Timing& t) {
  return t.cnf[1] == 0x8A ? 8u : 16u;
}

}  // namespace

// ---- The defect itself ------------------------------------------------------

TEST(Mcp2515Timing, RefusesTheFourRatesMeasuredAsSilentSuccesses) {
  // Each of these returned true before this change, having quietly configured the
  // chip for the rate in the comment.
  EXPECT_FALSE(compute(OSC_8MHZ, 1000000).ok) << "8 MHz cannot make 1000 kbit/s; it was giving 500";
  EXPECT_FALSE(compute(OSC_8MHZ, 800000).ok) << "8 MHz cannot make 800 kbit/s; it was giving 500";
  EXPECT_FALSE(compute(20000000, 1000000).ok) << "20 MHz was giving 1250 kbit/s";
  EXPECT_FALSE(compute(12000000, 500000).ok) << "12 MHz was giving 375 kbit/s";
}

TEST(Mcp2515Timing, RefusesTheFifthCaseTheSurveyDidNotList) {
  // 200 kbit/s is in the enum and an 8 MHz part answers it with 166 - 16.67%
  // out. It was not among the four measured cases; the tolerance catches it
  // for the same reason it catches those.
  const Timing t = compute(OSC_8MHZ, 200000);
  EXPECT_FALSE(t.ok);
  EXPECT_EQ(t.achieved, 166666u) << "the closest an 8 MHz part gets to 200 kbit/s";
}

TEST(Mcp2515Timing, RefusesEightHundredOnSixteenMegahertzToo) {
  // The one unreachable rate on the oscillator hw_lilygo2can declares: the
  // least-wrong layout overshoots to 1000 kbit/s, 25% fast.
  const Timing t = compute(OSC_16MHZ, 800000);
  EXPECT_FALSE(t.ok);
  EXPECT_EQ(t.achieved, 1000000u);
}

TEST(Mcp2515Timing, ARefusalWritesNoTimingRegisters) {
  // Nothing may be written on the failure path: applySpeedConfig() leaves the
  // chip at the timing it had, and that is only true if cnf is untouched.
  Timing t = compute(OSC_8MHZ, 1000000);
  ASSERT_FALSE(t.ok);
  EXPECT_EQ(t.cnf[0], 0xEE);
  EXPECT_EQ(t.cnf[1], 0xEE);
  EXPECT_EQ(t.cnf[2], 0xEE);
}

TEST(Mcp2515Timing, ARefusalStillReportsTheClosestAchievableRate) {
  // The out-parameter is what makes the log line diagnostic rather than just
  // negative: "closest 500000" tells the reader the part is an 8 MHz one.
  const Timing t = compute(OSC_8MHZ, 1000000);
  ASSERT_FALSE(t.ok);
  EXPECT_EQ(t.achieved, 500000u);
}

// ---- What must keep working -------------------------------------------------

TEST(Mcp2515Timing, AcceptsEveryRateTheTwoRealOscillatorsCanProduce) {
  // The tolerance must not cost a single working configuration. On 8 MHz and
  // 16 MHz - the only frequencies this driver meets - these are exact.
  for (uint32_t rate : {100000u, 125000u, 250000u, 500000u}) {
    EXPECT_TRUE(compute(OSC_8MHZ, rate).ok) << "8 MHz, " << rate;
    EXPECT_TRUE(compute(OSC_16MHZ, rate).ok) << "16 MHz, " << rate;
  }
  EXPECT_TRUE(compute(OSC_16MHZ, 200000).ok);
  EXPECT_TRUE(compute(OSC_16MHZ, 1000000).ok);
}

TEST(Mcp2515Timing, EveryAcceptedRateIsExactOnTheRealOscillators) {
  // Stronger than "accepted": on these two parts there is no accepted-but-
  // approximate case at all, which is why the threshold has room on both
  // sides. If a future oscillator makes this fail, the tolerance becomes a
  // real decision and this test is the place it gets revisited.
  for (uint32_t f_osc : {OSC_8MHZ, OSC_16MHZ}) {
    for (uint32_t rate : CAN_SPEED_ENUM_BPS) {
      const Timing t = compute(f_osc, rate);
      if (t.ok) {
        EXPECT_EQ(t.achieved, rate) << "osc " << f_osc << ", rate " << rate;
      }
    }
  }
}

TEST(Mcp2515Timing, AcceptedTimingEncodesAPrescalerThatYieldsTheAchievedRate) {
  // Ties CNF1 back to the arithmetic: BRP-1 in the low bits, and the layout
  // named by CNF2 divides down to exactly what was asked for.
  const Timing t = compute(OSC_16MHZ, 500000);
  ASSERT_TRUE(t.ok);
  EXPECT_EQ(t.achieved, 500000u);
  EXPECT_EQ(OSC_16MHZ / (2u * tq_of(t) * brp_of(t)), 500000u);
  EXPECT_LE(brp_of(t), 64u) << "CNF1 holds BRP-1 in six bits";
  EXPECT_GE(brp_of(t), 1u);
}

TEST(Mcp2515Timing, PicksTheEightTqLayoutWhenSixteenCannotReachTheRate) {
  // The case the driver's own comment calls out: 500 kbit/s at 8 MHz needs
  // TQ=8, and the layout constants must be the TQ=8 pair.
  const Timing t = compute(OSC_8MHZ, 500000);
  ASSERT_TRUE(t.ok);
  EXPECT_EQ(t.cnf[1], 0x8A);
  EXPECT_EQ(t.cnf[2], 0x01);
  EXPECT_EQ(t.cnf[0], 0x00) << "BRP = 1";
}

TEST(Mcp2515Timing, PicksTheSixteenTqLayoutWhenItIsTheAccurateOne) {
  const Timing t = compute(OSC_16MHZ, 500000);
  ASSERT_TRUE(t.ok);
  EXPECT_EQ(t.cnf[1], 0xA5);
  EXPECT_EQ(t.cnf[2], 0x03);
  EXPECT_EQ(t.cnf[0], 0x00) << "BRP = 1";
}

// ---- Degenerate arguments (the previous behaviour, kept) --------------------

TEST(Mcp2515Timing, RejectsDegenerateArguments) {
  uint8_t cnf[3] = {0, 0, 0};
  EXPECT_FALSE(mcp2515_calculate_timing(OSC_8MHZ, 500000, nullptr));
  EXPECT_FALSE(mcp2515_calculate_timing(OSC_8MHZ, 0, cnf)) << "zero rate";
  EXPECT_FALSE(mcp2515_calculate_timing(0, 500000, cnf)) << "a failed autodetect stores f_osc = 0";
}

TEST(Mcp2515Timing, AZeroOscillatorLeavesTheAchievedRateAlone) {
  // The degenerate path returns before any arithmetic, so it cannot claim a
  // closest rate - and must not overwrite the caller's variable with one.
  uint8_t cnf[3] = {0, 0, 0};
  uint32_t achieved = 0xDEADBEEF;
  EXPECT_FALSE(mcp2515_calculate_timing(0, 500000, cnf, &achieved));
  EXPECT_EQ(achieved, 0xDEADBEEFu);
}

TEST(Mcp2515Timing, TheOutParameterIsOptional) {
  uint8_t cnf[3] = {0, 0, 0};
  EXPECT_TRUE(mcp2515_calculate_timing(OSC_16MHZ, 500000, cnf));
}

// ---- The tolerance as a stated number ---------------------------------------

TEST(Mcp2515Timing, TheToleranceIsTheDocumentedHalfPercent) {
  EXPECT_EQ(MCP2515_TIMING_TOLERANCE_PERMILLE, 5) << "0.5% - the header says why";
}

TEST(Mcp2515Timing, NoRateIsAcceptedFurtherOutThanTheTolerance) {
  // Sweeps far wider than the enum, so the guarantee is a property of the
  // function rather than of the seven rates we happen to offer. 20 MHz and
  // 12 MHz are in here deliberately: they are the oscillators the review used to
  // demonstrate the defect, and nothing about the check is specific to the
  // two frequencies the driver meets today.
  for (uint32_t f_osc : {8000000u, 12000000u, 16000000u, 20000000u}) {
    for (uint32_t rate = 10000; rate <= 1000000; rate += 10000) {
      const Timing t = compute(f_osc, rate);
      if (!t.ok) {
        continue;
      }
      const uint32_t error = t.achieved > rate ? t.achieved - rate : rate - t.achieved;
      EXPECT_LE(static_cast<uint64_t>(error) * 1000u, static_cast<uint64_t>(rate) * MCP2515_TIMING_TOLERANCE_PERMILLE)
          << "accepted " << rate << " on " << f_osc << " while achieving " << t.achieved;
    }
  }
}

TEST(Mcp2515Timing, TheBoundaryItselfIsInclusive) {
  // The three cases either side of the threshold, which no real (oscillator,
  // rate) pair reaches - every one of those is exact or badly out, so without
  // a synthetic pair the comparison operator is unpinned and `>` could become
  // `>=` unnoticed. Found by searching for an exact 5-per-mille miss: an
  // 11 MHz part asked for 6000 bit/s lands on 6030, dead on the line.
  EXPECT_TRUE(compute(11000000, 7000).ok) << "7015 - 0.21% out, comfortably inside";
  EXPECT_TRUE(compute(11000000, 6000).ok) << "6030 - exactly 0.5%, the boundary is inclusive";
  EXPECT_FALSE(compute(11000000, 9000).ok) << "9046 - 0.51% out, the first refusal past the line";
}

TEST(Mcp2515Timing, TheCheckIsRelativeNotAbsolute) {
  // A fixed absolute slack would pass low rates and fail high ones (or the
  // reverse). Same 6.25% error, both refused; a 4 kbit/s absolute miss is
  // fatal at 100 kbit/s and fine at 1000, and neither is accepted here.
  EXPECT_FALSE(compute(12000000, 100000).ok) << "93750 - 6.25% low";
  EXPECT_FALSE(compute(12000000, 800000).ok) << "750000 - 6.25% low";
}

/* The arithmetic must survive every uint32_t the header's contract admits.
 *
 * The divisor is tq_per_bit * 2 * can_rate. In 32 bits that wraps, and at
 * exactly 2^27 (16 TQ) and 2^28 (8 TQ) it wraps to ZERO - so the rounding
 * division divided by zero and the process died. Nothing on the live path can
 * reach it (CAN_Speed stops at 1000 kbit/s), but the header names only the null
 * buffer and the two zeroes as the arguments it refuses, and the whole point of
 * the extraction was that this function can now be called on its own.
 *
 * The tolerance comparison had already been widened to 64 bits for the same
 * class of reason; the divisor that feeds it had not.
 */
TEST(Mcp2515Timing, SurvivesTheRatesWhereTheDivisorWouldWrapToZero) {
  for (uint32_t rate : {1u << 27, 1u << 28, 0xFFFFFFFFu}) {
    uint8_t cnf[3] = {0, 0, 0};
    uint32_t achieved = 0;
    // Must return an answer rather than crash, and the answer must be "no":
    // no oscillator this driver meets comes within 0.5% of 134 Mbit/s.
    EXPECT_FALSE(mcp2515_calculate_timing(OSC_16MHZ, rate, cnf, &achieved)) << "rate " << rate << " was accepted";
    EXPECT_LE(achieved, OSC_16MHZ / 16) << "no layout can exceed f_osc/16";
  }
}

/* The widening must not have moved any answer that already worked. Sweeping the
 * whole plausible parameter space is what makes "behaviour-preserving" a
 * measurement rather than a claim about a diff.
 */
TEST(Mcp2515Timing, TheAcceptedRegionIsUnchangedAcrossTheParameterSpace) {
  int accepted = 0;
  for (uint32_t osc = 1000000; osc <= 25000000; osc += 250000) {
    for (uint32_t rate = 10000; rate <= 1000000; rate += 5000) {
      uint8_t cnf[3] = {0, 0, 0};
      uint32_t achieved = 0;
      if (!mcp2515_calculate_timing(osc, rate, cnf, &achieved)) {
        continue;
      }
      accepted++;
      // Everything accepted is inside the tolerance, and its CNF1 encodes a
      // prescaler that really yields the rate reported.
      const uint32_t err = achieved > rate ? achieved - rate : rate - achieved;
      EXPECT_LE((uint64_t)err * 1000u, (uint64_t)rate * MCP2515_TIMING_TOLERANCE_PERMILLE)
          << "osc " << osc << " rate " << rate;
      const uint32_t tq = (cnf[1] == 0x8A) ? 8 : 16;
      EXPECT_EQ(osc / (tq * 2 * ((uint32_t)cnf[0] + 1)), achieved) << "osc " << osc << " rate " << rate;
    }
  }
  EXPECT_GT(accepted, 500) << "the sweep must actually reach the accepting region";
}
