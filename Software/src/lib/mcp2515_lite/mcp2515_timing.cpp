#include "mcp2515_timing.h"

/* The arithmetic is unchanged from the version that lived as a file-static in
mcp2515_lite.cpp; what is new is the verdict at the end.

Two fixed TQ layouts are tried - 16 TQ per bit and 8 TQ per bit - the prescaler
for each is rounded to the nearest integer and clamped to the [1,64] the CNF1
field can hold, and whichever lands closer to the requested rate wins. That is
a reasonable way to PICK a layout. It was never a way to decide whether the
request was answerable, because "least wrong" has an answer even when both
options are hopeless, and the old code returned that answer as success.

What the two oscillators this driver actually meets can produce (16 MHz is what
hw_lilygo2can declares; autodetectOscillatorFrequency() returns 8 or 16 MHz and
nothing else):

     osc     requested   achieved    error
    8 MHz     100 kbit    100 kbit    0.00%
    8 MHz     125 kbit    125 kbit    0.00%
    8 MHz     200 kbit    166 kbit   16.67%   <- refused from here on
    8 MHz     250 kbit    250 kbit    0.00%
    8 MHz     500 kbit    500 kbit    0.00%
    8 MHz     800 kbit    500 kbit   37.50%   <- refused
    8 MHz    1000 kbit    500 kbit   50.00%   <- refused
   16 MHz     100 kbit    100 kbit    0.00%
   16 MHz     125 kbit    125 kbit    0.00%
   16 MHz     200 kbit    200 kbit    0.00%
   16 MHz     250 kbit    250 kbit    0.00%
   16 MHz     500 kbit    500 kbit    0.00%
   16 MHz     800 kbit   1000 kbit   25.00%   <- refused
   16 MHz    1000 kbit   1000 kbit    0.00%

Every real case is either exact or more than 16% out, with nothing in between,
so the threshold is not a knife edge here: any tolerance between roughly 0.1%
and 15% produces this same column of verdicts. 0.5% is chosen because it is the
number a CAN node is designed to rather than because the boundary is delicate -
see the header. Note the 200 kbit/s row: it is in the CAN_Speed enum, it is not
one of the four cases the review listed, and an 8 MHz part answered it with 166.
*/

// One TQ layout: the rounded, clamped prescaler and the rate it yields.
namespace {

struct Layout {
  uint32_t brp;
  uint32_t rate;
  uint32_t error;
};

Layout evaluate(uint32_t f_osc, uint32_t can_rate, uint32_t tq_per_bit) {
  // 64-bit divisor. In 32 bits this wraps, and at can_rate 2^27 (for
  // the 16 TQ layout) or 2^28 (for 8 TQ) it wraps to EXACTLY zero, so the
  // rounding division below divided by zero and the process died - reachable
  // through the header's documented contract, which admits any uint32_t rate
  // and names only null/zero/zero as the degenerate arguments. No live path
  // reaches it: CAN_Speed tops out at 1000 kbit/s. Every value the 32-bit
  // form handled without wrapping computes identically here, so this is a
  // widening and not a behaviour change.
  const uint64_t divisor = (uint64_t)tq_per_bit * 2u * can_rate;
  uint64_t brp = ((uint64_t)f_osc + (divisor / 2)) / divisor;  // Integer rounding
  if (brp < 1) {
    brp = 1;
  } else if (brp > 64) {
    brp = 64;
  }
  // brp is clamped to [1,64] above, so this product cannot overflow.
  const uint32_t rate = f_osc / (uint32_t)(tq_per_bit * 2 * brp);
  const uint32_t error = (rate > can_rate) ? (rate - can_rate) : (can_rate - rate);
  return {(uint32_t)brp, rate, error};
}

}  // namespace

bool mcp2515_calculate_timing(uint32_t f_osc, uint32_t can_rate, uint8_t* cnf, uint32_t* achieved_rate) {
  if (!cnf || can_rate == 0 || f_osc == 0) {
    return false;
  }

  // TQ = 16 (will fail for 500kbit@8MHz) and TQ = 8 (lower resolution)
  const Layout tq16 = evaluate(f_osc, can_rate, 16);
  const Layout tq8 = evaluate(f_osc, can_rate, 8);
  const bool use_tq8 = (tq8.error < tq16.error);
  const Layout& best = use_tq8 ? tq8 : tq16;

  if (achieved_rate) {
    *achieved_rate = best.rate;
  }

  // Refuse a rate this oscillator cannot get close to. Widened to 64 bits so
  // the comparison cannot overflow at any rate the uint32_t argument admits.
  if ((uint64_t)best.error * 1000u > (uint64_t)can_rate * MCP2515_TIMING_TOLERANCE_PERMILLE) {
    return false;
  }

  if (use_tq8) {
    cnf[0] = (uint8_t)(tq8.brp - 1);
    cnf[1] = 0x8A;  // BTLMODE=1, SAM=0, PHSEG1=1, PRSEG=2
    cnf[2] = 0x01;  // PHSEG2=1
  } else {
    cnf[0] = (uint8_t)(tq16.brp - 1);
    cnf[1] = 0xA5;  // BTLMODE=1, SAM=0, PHSEG1=4, PRSEG=5
    cnf[2] = 0x03;  // PHSEG2=3
  }

  return true;
}
