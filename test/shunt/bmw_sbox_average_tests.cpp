// BMW-SBOX's "1 second average" divided by the full ten-slot window whether or
// not the window had filled, so for the first second after the shunt started
// reporting, the published average was a FRACTION of the real current: one
// sample in, nine empty slots averaged in as if they were zero-current
// readings, and the consumer saw a tenth of the truth.
//
// It is not an internal number.  KOSTAL-RS485 transmits
// measured_avg1S_amperage_mA to the inverter whenever the SBOX shunt is
// selected, so the understated current goes out on the wire.
//
// The fix divides by the samples actually taken.  The choice, stated: the
// alternative was to publish nothing until the window fills, but the field has
// no validity flag, so "publish nothing" means the consumer keeps reading the
// initial 0 A — a plausible-looking wrong number, which is the same failure
// class in a quieter coat.  An average over the samples that exist is never a
// number no measurement supports, and it converges to the full-window average
// within the second.
//
// These are host tests over the averaging arithmetic; the bench has no SBOX.

#include <gtest/gtest.h>

#include <Arduino.h>

#include "../../Software/src/datalayer/datalayer.h"
#include "../../Software/src/shunt/BMW-SBOX.h"
#include "../../Software/src/shunt/Shunt.h"

namespace {

// One sample is taken per 100 ms of millis(), and ten of them span the second
// the published average claims to cover.
const unsigned long SAMPLE_INTERVAL_MS = 100;
const int SAMPLE_COUNT = 10;

class SboxAverageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    delete shunt;
    shunt = nullptr;
    sbox = new BmwSbox();
    shunt = sbox;
    sbox->setup();
    set_millis64(0);
    now_ms = 0;
    datalayer.shunt.measured_avg1S_amperage_mA = 0;
  }

  void TearDown() override {
    delete shunt;
    shunt = nullptr;
    sbox = nullptr;
  }

  // Advances the clock past the sampling interval and feeds one 0x200 current
  // frame, so each call contributes exactly one sample to the window.
  void take_sample(int32_t mA) {
    now_ms += SAMPLE_INTERVAL_MS + 1;
    set_millis64(now_ms);
    uint32_t raw = static_cast<uint32_t>(mA);
    CAN_frame f = {.FD = false,
                   .ext_ID = false,
                   .DLC = 3,
                   .ID = 0x200,
                   .data = {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
                            static_cast<uint8_t>((raw >> 16) & 0xFF)}};
    sbox->handle_incoming_can_frame(f);
  }

  BmwSbox* sbox = nullptr;
  unsigned long now_ms = 0;
};

// The defect at its sharpest: one sample, and the average IS that sample.
// Before the fix this published -100, a tenth of a 1 A discharge.
TEST_F(SboxAverageTest, FirstSampleAveragesToItselfNotToATenthOfItself) {
  take_sample(-1000);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -1000)
      << "one sample averages to itself; the nine slots it has not filled are not zero-current readings";
}

// Partway through the window the divisor is the count so far, not ten.
// -300, -600, -900 average to -600; divided by ten they would be -180.
TEST_F(SboxAverageTest, PartialWindowDividesByTheSamplesTaken) {
  take_sample(-300);
  take_sample(-600);
  take_sample(-900);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -600) << "three samples must be divided by three, not by ten";
}

// The regression that matters most: once the window HAS filled, the published
// value is exactly what it always was — a mean over all ten slots. This is the
// steady state the driver spends essentially all of its life in.
TEST_F(SboxAverageTest, FullWindowIsUnchangedByTheFix) {
  for (int i = 1; i <= SAMPLE_COUNT; i++) {
    take_sample(-100 * i);  // -100 .. -1000, mean -550
  }
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -550)
      << "a full window must still be the mean of all ten samples";
}

// And the divisor must STOP at ten: sample eleven overwrites slot 0, so the
// average is the mean of the last ten, not a mean over an ever-growing count.
TEST_F(SboxAverageTest, DivisorStopsGrowingOnceTheWindowIsFull) {
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    take_sample(-1000);
  }
  ASSERT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -1000);

  // Slot 0's -1000 is replaced by 0, so the window is nine -1000s and one 0.
  take_sample(0);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -900)
      << "the eleventh sample must replace the first, still over a ten-sample divisor";
}

// The charge direction gets the same treatment — the fix must not be a
// discharge-only correction.
TEST_F(SboxAverageTest, ChargeDirectionAveragesOverItsOwnSampleCount) {
  take_sample(2000);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, 2000);
  take_sample(1000);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, 1500) << "two charge samples must be divided by two";
}

// Frames arriving faster than the sampling interval must not be counted: the
// window is defined by TIME, and inflating the count would shrink every
// average by the number of extra frames.
TEST_F(SboxAverageTest, FramesInsideTheSamplingIntervalDoNotCount) {
  take_sample(-1000);
  // Same millisecond, three more frames: the instantaneous field must follow
  // them, the average must not move.
  for (int i = 0; i < 3; i++) {
    uint32_t raw = static_cast<uint32_t>(-4000);
    CAN_frame f = {.FD = false,
                   .ext_ID = false,
                   .DLC = 3,
                   .ID = 0x200,
                   .data = {static_cast<uint8_t>(raw & 0xFF), static_cast<uint8_t>((raw >> 8) & 0xFF),
                            static_cast<uint8_t>((raw >> 16) & 0xFF)}};
    sbox->handle_incoming_can_frame(f);
  }

  EXPECT_EQ(datalayer.shunt.measured_amperage_mA, -4000);
  EXPECT_EQ(datalayer.shunt.measured_avg1S_amperage_mA, -1000)
      << "only one sample per interval enters the window, however many frames arrive";
}

}  // namespace
