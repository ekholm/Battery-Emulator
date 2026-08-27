// Tests for the flash-write broker: the write side of "zero CAN loss
// across flash writes". The property under test is an ORDER - drain, one
// operation, gap - plus the measurement that says whether the receive FIFOs are
// deep enough to cover the longest window the order still leaves.
//
// These execute the policy rather than reading it: the platform arrives through
// FlashWriteBroker::Hooks, so a test can be the clock, the yield and the CAN
// owner at once, and every claim below is a claim about what the broker DID.

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "../Software/src/devboard/utils/flash_write_broker.h"

namespace {

// A fake platform. Time only moves when a test says so, so every window length
// below is exact rather than "about".
class BrokerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    FlashWriteBroker::Hooks hooks;
    hooks.now_us = [this]() {
      return now_us_;
    };
    hooks.gap = [this]() {
      trace_.push_back("gap");
      now_us_ += gap_cost_us_;
      if (owner_drains_on_gap_) {
        owner_pass();
      }
    };
    hooks.may_wait = [this]() {
      return may_wait_;
    };
    hooks.drain_in_line = [this]() {
      trace_.push_back("drain_in_line");
    };
    hooks.report = [this](const FlashWriteBroker::StormSummary& summary) {
      reports_.push_back(summary);
      trace_.push_back("report");
    };
    broker_.set_hooks(hooks);
  }

  // What the CAN owner task does every millisecond: drain every FIFO, bracketed
  // so the broker knows which requests that pass answers.
  void owner_pass() {
    broker_.drain_starting();
    trace_.push_back("drain");
    broker_.drain_completed();
  }

  // One brokered flash operation that takes the given time.
  void write(uint32_t cost_us) {
    broker_.run([&]() {
      trace_.push_back("write");
      now_us_ += cost_us;
    });
  }

  FlashWriteBroker broker_;
  std::vector<std::string> trace_;
  std::vector<FlashWriteBroker::StormSummary> reports_;
  uint64_t now_us_ = 1000;
  uint32_t gap_cost_us_ = 1000;
  bool may_wait_ = true;
  bool owner_drains_on_gap_ = true;
};

// The order is the whole contract: nothing may be written before the owner has
// said the FIFOs are empty, and nothing may follow the write before the owner
// has had a chance to drain what arrived during it.
TEST_F(BrokerFixture, BrokersOneWriteAsDrainThenWriteThenGap) {
  write(20000);

  EXPECT_EQ(trace_, (std::vector<std::string>{"gap", "drain", "write", "gap", "drain"}));
}

// The wait ends on the acknowledgement, not on a fixed number of yields.
TEST_F(BrokerFixture, WaitsUntilTheOwnerAcknowledges) {
  owner_drains_on_gap_ = false;
  gap_cost_us_ = 100;
  int gaps = 0;
  FlashWriteBroker::Hooks hooks;
  hooks.now_us = [this]() {
    return now_us_;
  };
  hooks.may_wait = []() {
    return true;
  };
  hooks.gap = [&]() {
    now_us_ += 100;
    if (++gaps == 3) {
      owner_pass();  // The owner finally gets around to it.
    }
  };
  broker_.set_hooks(hooks);

  write(1000);

  EXPECT_EQ(gaps, 4);  // Three waiting for the acknowledgement, one drainage gap.
}

// A drain that finished BEFORE the request cannot answer it. Without the
// bracket the broker would accept the stale pass and write into a FIFO nobody
// emptied.
TEST_F(BrokerFixture, StaleDrainDoesNotSatisfyALaterRequest) {
  int gaps = 0;
  FlashWriteBroker::Hooks hooks;
  hooks.now_us = [this]() {
    return now_us_;
  };
  hooks.may_wait = []() {
    return true;
  };
  hooks.gap = [&]() {
    now_us_ += 100;
    gaps++;
    if (gaps == 1) {
      broker_.drain_completed();  // The pass that was already running finishes.
    } else {
      owner_pass();  // A pass that started after the request, and so answers it.
    }
  };
  broker_.set_hooks(hooks);

  // The owner is mid-pass - it has already read the FIFOs - when the request
  // arrives. Its acknowledgement therefore says nothing about frames that
  // landed since, and must not release the writer.
  broker_.drain_starting();

  write(1000);

  EXPECT_EQ(gaps, 3);  // Stale acknowledgement, fresh drain, drainage gap.
}

// A wedged owner must not cost the caller its settings save. The write goes
// ahead once the timeout expires, and the timeout is counted - a run full of
// them means the pre-drain is not happening at all.
TEST_F(BrokerFixture, WritesAnywayWhenTheOwnerNeverAcknowledges) {
  owner_drains_on_gap_ = false;

  write(1000);

  EXPECT_EQ(broker_.stats().operations, 1u);
  EXPECT_EQ(broker_.stats().drain_timeouts, 1u);
  EXPECT_NE(std::find(trace_.begin(), trace_.end(), "write"), trace_.end());
}

// The timeout is bounded by the clock, not by the number of yields, so a gap
// that returns instantly cannot spin forever.
TEST_F(BrokerFixture, DrainTimeoutIsBoundedByTheClock) {
  owner_drains_on_gap_ = false;
  gap_cost_us_ = 1;

  const uint64_t before = now_us_;
  write(0);

  EXPECT_LT(now_us_ - before, (uint64_t)(FlashWriteBroker::DRAIN_ACK_TIMEOUT_MS + 2) * 1000);
}

// The owner task itself saves settings (the inverter watchdog write lives in
// its 1 s branch). It cannot wait for its own acknowledgement, so it drains in
// line instead - and must not deadlock.
TEST_F(BrokerFixture, OwnerTaskDrainsInLineInsteadOfWaiting) {
  may_wait_ = false;

  write(1000);

  EXPECT_EQ(trace_, (std::vector<std::string>{"drain_in_line", "write", "gap", "drain"}));
  EXPECT_EQ(broker_.stats().drain_timeouts, 0u);
}

// The window is the cache-off time only. The drain and the gap around it are
// cache-on and must not inflate the number the FIFO sizing is judged against.
TEST_F(BrokerFixture, WindowMeasuresOnlyTheOperation) {
  write(20000);

  EXPECT_EQ(broker_.stats().last_operation_us, 20000u);
  EXPECT_EQ(broker_.stats().longest_operation_us, 20000u);
  EXPECT_EQ(broker_.stats().total_operation_us, 20000u);
}

// The longest window is what the FIFOs have to cover, so the maximum is kept,
// not the latest.
TEST_F(BrokerFixture, KeepsTheLongestWindowNotTheLast) {
  write(20000);
  write(500);

  EXPECT_EQ(broker_.stats().longest_operation_us, 20000u);
  EXPECT_EQ(broker_.stats().last_operation_us, 500u);
  EXPECT_EQ(broker_.stats().operations, 2u);
}

// The gap between drains is what the receive FIFOs have to cover, and it is
// measured on the drain side on purpose - a starvation the broker never caused
// still starves the FIFOs, and must still show up.
TEST_F(BrokerFixture, MeasuresTheGapBetweenDrains) {
  owner_pass();
  now_us_ += 40000;
  owner_pass();
  now_us_ += 1000;
  owner_pass();

  EXPECT_EQ(broker_.stats().longest_drain_gap_us, 40000u);
}

// The first drain has no predecessor, so there is no gap to report. Counting
// one would make the number the uptime at first drain.
TEST_F(BrokerFixture, TheFirstDrainReportsNoGap) {
  now_us_ += 5000000;
  owner_pass();

  EXPECT_EQ(broker_.stats().longest_drain_gap_us, 0u);
}

// One brokered operation is not one cache-off window: an OTA chunk asks for a
// 64 KB erase and the flash driver serves it as sector erases with the cache
// back on between them. The operation is long; the gaps that matter are short.
// Reporting the operation as the window would overstate the loss tenfold.
TEST_F(BrokerFixture, ALongOperationCanBeManyShortGaps) {
  broker_.run([&]() {
    for (int sector = 0; sector < 4; sector++) {
      now_us_ += 30000;  // one sector erase, cache off
      owner_pass();      // the driver yields, the owner drains
    }
  });

  EXPECT_EQ(broker_.stats().longest_operation_us, 120000u);
  EXPECT_EQ(broker_.stats().longest_drain_gap_us, 30000u);
}

// A storm is reported once, when it is over - one settings save and the OTA
// that follows an hour later are two events, and averaging them would hide the
// erase that only the second one does.
TEST_F(BrokerFixture, ReportsAStormWhenItGoesIdle) {
  write(20000);
  write(30000);
  EXPECT_TRUE(reports_.empty());  // Still in progress.

  now_us_ += (uint64_t)FlashWriteBroker::STORM_IDLE_MS * 1000;
  broker_.poll();

  ASSERT_EQ(reports_.size(), 1u);
  EXPECT_EQ(reports_[0].operations, 2u);
  EXPECT_EQ(reports_[0].longest_operation_us, 30000u);
  EXPECT_EQ(reports_[0].total_operation_us, 50000u);
}

// poll() before the idle time has passed must not cut a storm in half; that
// would report a longest window shorter than the storm's real one.
TEST_F(BrokerFixture, PollDoesNotEndAStormEarly) {
  write(20000);
  now_us_ += (uint64_t)FlashWriteBroker::STORM_IDLE_MS * 1000 / 2;
  broker_.poll();

  EXPECT_TRUE(reports_.empty());
}

// A write after the idle gap starts a fresh storm rather than joining the old
// one, and the old one is reported at that point even if nobody polled.
TEST_F(BrokerFixture, ANewStormStartsAfterTheIdleGap) {
  write(30000);
  now_us_ += (uint64_t)FlashWriteBroker::STORM_IDLE_MS * 1000;
  write(500);

  ASSERT_EQ(reports_.size(), 1u);
  EXPECT_EQ(reports_[0].longest_operation_us, 30000u);
  EXPECT_EQ(broker_.stats().storm_operations, 1u);
  EXPECT_EQ(broker_.stats().storm_longest_operation_us, 500u);
}

// Every write in a storm gets its own drain and its own gap. That is what turns
// one long stall into a train of short windows; batching the drains would put
// the FIFO back in front of the whole save.
TEST_F(BrokerFixture, EveryWriteInAStormIsBrokeredSeparately) {
  write(1000);
  write(1000);
  write(1000);

  int drains = 0;
  for (const auto& step : trace_) {
    if (step == "drain") {
      drains++;
    }
  }
  EXPECT_EQ(drains, 6);  // One before each write, one in each drainage gap.
}

// A flash write reached from inside another one is already covered by the outer
// cache-off window. Draining again there would report two windows for one
// stall and yield while the outer caller believes it holds the flash.
TEST_F(BrokerFixture, NestedWriteRunsInsideTheOuterWindow) {
  broker_.run([&]() {
    trace_.push_back("outer");
    now_us_ += 1000;
    broker_.run([&]() {
      trace_.push_back("inner");
      now_us_ += 1000;
    });
  });

  EXPECT_EQ(broker_.stats().operations, 1u);
  EXPECT_EQ(broker_.stats().last_operation_us, 2000u);
  EXPECT_EQ(trace_, (std::vector<std::string>{"gap", "drain", "outer", "inner", "gap", "drain"}));
}

// The storm summary is what reaches the log, and the drain gap is the number
// the whole item is about - reporting the operation times without it would put
// everything except the answer in the line the user reads.
TEST_F(BrokerFixture, TheStormSummaryCarriesTheDrainGapAndTheTimeouts) {
  owner_drains_on_gap_ = false;  // Nothing acknowledges, so the pre-drain times out.
  write(20000);
  owner_pass();
  now_us_ += 40000;
  owner_pass();

  now_us_ += (uint64_t)FlashWriteBroker::STORM_IDLE_MS * 1000;
  broker_.poll();

  ASSERT_EQ(reports_.size(), 1u);
  EXPECT_EQ(reports_[0].longest_drain_gap_us, 40000u);
  EXPECT_EQ(reports_[0].drain_timeouts, 1u);
}

// The performance page reads the storm mirrors, and they describe the storm in
// progress. Carrying the finished storm's worst numbers into the next one would
// report a drain gap that no write in the new storm ever caused.
TEST_F(BrokerFixture, ANewStormDoesNotInheritTheOldStormsWorstNumbers) {
  // The owner drains only when this test says so, and the writer drains in line
  // rather than waiting, so every drain below is one the test placed.
  owner_drains_on_gap_ = false;
  may_wait_ = false;

  write(30000);  // Storm one.
  owner_pass();
  now_us_ += 40000;
  owner_pass();  // A 40 ms gap, inside storm one.
  EXPECT_EQ(broker_.stats().storm_longest_drain_gap_us, 40000u);

  // Let storm one go idle with the owner running normally - an idle second is
  // not a starved second, and must not be recorded as one.
  for (int pass = 0; pass <= FlashWriteBroker::STORM_IDLE_MS; pass++) {
    now_us_ += 1000;
    owner_pass();
  }
  broker_.poll();
  ASSERT_EQ(reports_.size(), 1u);

  write(500);  // Storm two, in which nothing has starved the drain at all.

  EXPECT_EQ(broker_.stats().storm_longest_drain_gap_us, 0u);
  EXPECT_EQ(broker_.stats().storm_longest_operation_us, 500u);
  EXPECT_EQ(broker_.stats().storm_operations, 1u);
  EXPECT_EQ(broker_.stats().storm_drain_timeouts, 0u);
  // The lifetime maximum is a different question and keeps its answer.
  EXPECT_EQ(broker_.stats().longest_drain_gap_us, 40000u);
}

// A timeout in the burst just made is what a user reading the page after a save
// needs; the lifetime count cannot answer it once the device has been up a
// while. The two are separate counters, and only one of them resets.
TEST_F(BrokerFixture, StormTimeoutsAreCountedApartFromTheLifetimeOnes) {
  owner_drains_on_gap_ = false;  // Nothing ever acknowledges: every write times out.
  write(1000);
  EXPECT_EQ(broker_.stats().storm_drain_timeouts, 1u);

  now_us_ += (uint64_t)FlashWriteBroker::STORM_IDLE_MS * 1000;
  write(1000);  // Storm two, and one timeout of its own.

  // One in this storm, two since boot - the two counters must not be the same
  // number wearing different labels.
  EXPECT_EQ(broker_.stats().storm_drain_timeouts, 1u);
  EXPECT_EQ(broker_.stats().drain_timeouts, 2u);
}

// reset_stats() is the only way back to a clean slate, and it has to clear the
// open storm too - leaving it open would report a storm whose counters had been
// zeroed underneath it.
TEST_F(BrokerFixture, ResetStatsClearsTheCountersAndTheOpenStorm) {
  write(20000);
  ASSERT_EQ(broker_.stats().operations, 1u);

  broker_.reset_stats();

  EXPECT_EQ(broker_.stats().operations, 0u);
  EXPECT_EQ(broker_.stats().longest_operation_us, 0u);
  EXPECT_EQ(broker_.stats().longest_drain_gap_us, 0u);
  EXPECT_EQ(broker_.stats().total_operation_us, 0u);

  // The storm is closed, not merely zeroed: polling must not report the storm
  // whose numbers were just thrown away.
  reports_.clear();
  now_us_ += (uint64_t)FlashWriteBroker::STORM_IDLE_MS * 1000;
  broker_.poll();
  EXPECT_TRUE(reports_.empty());
}

// Two tasks enter run() at the same time in the ordinary case, not only in a
// contrived one: core_loop saves the inverter watchdog setting from its 1 s
// branch on one core while the AsyncTCP task brokers an OTA chunk on the other.
// The depth count is the only thing separating "nested" from "in flight", so if
// an increment can be lost the count never returns to zero and every write for
// the rest of the boot is silently treated as nested - no pre-drain, no
// drainage gap, no measurement, and nothing in the logs to say so.
//
// The counters this hammers are deliberately NOT asserted on: they are advisory
// and raced by construction. What is asserted is that the broker still brokers
// afterwards.
TEST(FlashWriteBrokerConcurrency, ConcurrentWritersCannotWedgeTheBroker) {
  FlashWriteBroker broker;
  std::atomic<uint64_t> clock{1000};

  FlashWriteBroker::Hooks racing;
  racing.now_us = [&clock]() {
    return clock.fetch_add(1);
  };
  broker.set_hooks(racing);  // No may_wait and no gap: nothing to block on.

  constexpr int kWritesPerTask = 200000;
  std::thread core_loop_task([&]() {
    for (int i = 0; i < kWritesPerTask; i++) {
      broker.run([]() {});
    }
  });
  std::thread async_tcp_task([&]() {
    for (int i = 0; i < kWritesPerTask; i++) {
      broker.run([]() {});
    }
  });
  core_loop_task.join();
  async_tcp_task.join();

  // One quiet write, watched. A wedged depth count shows up as a write that is
  // neither preceded by a drain nor followed by a gap.
  std::vector<std::string> trace;
  FlashWriteBroker::Hooks watched;
  watched.now_us = [&clock]() {
    return clock.fetch_add(1);
  };
  watched.may_wait = []() {
    return false;
  };
  watched.drain_in_line = [&trace]() {
    trace.push_back("drain");
  };
  watched.gap = [&trace]() {
    trace.push_back("gap");
  };
  broker.set_hooks(watched);

  const uint32_t before = broker.stats().operations;
  broker.run([&trace]() { trace.push_back("write"); });

  EXPECT_EQ(trace, (std::vector<std::string>{"drain", "write", "gap"}));
  EXPECT_EQ(broker.stats().operations, before + 1);
}

// The broker must survive a platform that supplies nothing - the boot path runs
// before the CAN owner exists, and a settings write there still has to work.
TEST(FlashWriteBrokerBare, RunsWithNoHooksAtAll) {
  FlashWriteBroker broker;
  bool ran = false;

  broker.run([&]() { ran = true; });

  EXPECT_TRUE(ran);
  EXPECT_EQ(broker.stats().operations, 1u);
}

}  // namespace
