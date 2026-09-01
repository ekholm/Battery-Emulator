#pragma once

#include <stdint.h>

/* MCP2515 bit-timing arithmetic, kept in its own translation unit.

It lives apart from mcp2515_lite.cpp for one reason: that file reaches for
Arduino and FreeRTOS, so it has no host build, and for five years the only way
to check this arithmetic was to read it. It was read, and got wrong, in a
comment; a later reading found the defect below. Nothing here needs a
chip, a bus or an RTOS - it is integer arithmetic over two arguments - so it
belongs somewhere a test can call it.
*/

// How far the achievable bitrate may miss the requested one before the request
// is refused, in parts per thousand.
//
// 0.5% is the ordinary oscillator budget a CAN node is designed to. What bounds
// it in ISO 11898-1 is the RESYNCHRONISATION margin, not bit stuffing: the
// tolerable deviation df is derived from the phase segments and SJW over the
// longest stretch the receiver must ride without a resynchronising edge, and
// for ordinary segment choices that lands somewhere under a couple of percent
// of relative error between two nodes - so half a percent each, in opposite
// directions, is already most of the budget. (A later review corrected this paragraph:
// it used to attribute the limit to what bit stuffing "can absorb over an
// 11-bit identifier", which is not the mechanism - stuffing GUARANTEES the
// edges that resynchronisation needs, it does not absorb clock error.)
// A prescaler that misses by more than
// that is not a tolerance question at all - see the table in
// mcp2515_timing.cpp: on the oscillators this driver actually meets, every
// achievable rate is either exact or more than 16% out. The threshold has wide
// margin on both sides rather than sitting near any real case.
#define MCP2515_TIMING_TOLERANCE_PERMILLE 5

// Compute CNF1/CNF2/CNF3 for `can_rate` bit/s from an `f_osc` Hz oscillator.
//
// Returns false - writing nothing to `cnf` - when the arguments are degenerate
// (null buffer, zero rate, zero oscillator) OR when the closest rate this
// oscillator can produce misses the requested one by more than
// MCP2515_TIMING_TOLERANCE_PERMILLE. Until this change the second case returned
// TRUE: the prescaler was clamped, the least-wrong of two TQ layouts was
// picked, and a chip running at half the requested bitrate reported a
// successful init.
//
// `achieved_rate`, when not null, receives the rate the returned timing
// actually produces - or, on a refusal, the closest this oscillator could get.
// It is written whenever the arguments are non-degenerate, so a caller can say
// what it would have got instead of only that it failed.
bool mcp2515_calculate_timing(uint32_t f_osc, uint32_t can_rate, uint8_t* cnf, uint32_t* achieved_rate = nullptr);
