#include "flash_write_broker.h"

FlashWriteBroker& flash_write_broker() {
  static FlashWriteBroker broker;
  return broker;
}

// Ask the CAN owner to run a receive pass, and wait until it says it has.
//
// The wait is the whole point: returning before the owner has drained would put
// the operation in front of software queues as full as they happened to be, and
// they have to hold the burst that lands the moment the window ends. It is
// bounded all the same - a save must not be lost because the owner is wedged -
// and a timeout is counted rather than hidden, since a run full of them means
// the pre-drain is not happening.
//
// What this does NOT do, since the header's step 1 is easy to read too
// generously: it does not empty the controllers' HARDWARE FIFOs. Those
// are served by an ISR, or by a task an ISR wakes, and a cache-off window stops
// both. Their depth is covered by keeping the window short, not by draining
// ahead of it.
void FlashWriteBroker::pre_drain() {
  const bool may_wait = hooks_.may_wait && hooks_.may_wait();

  if (!may_wait) {
    // The caller is the owner itself (it cannot wait for its own
    // acknowledgement), or no owner exists yet. Drain here instead.
    if (hooks_.drain_in_line) {
      hooks_.drain_in_line();
    }
    return;
  }

  if (!hooks_.gap || !hooks_.now_us) {
    return;  // Nothing to wait with.
  }

  const uint32_t wanted = drain_requested_.fetch_add(1) + 1;
  const uint64_t deadline_us = hooks_.now_us() + (uint64_t)DRAIN_ACK_TIMEOUT_MS * 1000;

  // Signed difference, so a wrapped counter still compares correctly.
  while ((int32_t)(drain_served_.load() - wanted) < 0) {
    if (hooks_.now_us() >= deadline_us) {
      stats_.drain_timeouts++;
      storm_.drain_timeouts++;
      stats_.storm_drain_timeouts = storm_.drain_timeouts;
      return;
    }
    hooks_.gap();
  }
}

// Every drain pass, from the CAN owner. The gap since the previous pass is the
// number the receive FIFOs have to cover - measured here, on the drain side,
// because that is where starvation is actually felt. A brokered operation is
// not the same thing: an OTA chunk asks for a 64 KB erase and the flash driver
// serves it as a train of sector erases with the cache back on between them, so
// one long operation can be many short gaps.
void FlashWriteBroker::drain_starting() {
  drain_in_progress_ = drain_requested_.load();

  if (!hooks_.now_us) {
    return;
  }
  const uint64_t now_us = hooks_.now_us();
  if (last_drain_start_us_ != 0) {
    const uint32_t gap_us = (uint32_t)(now_us - last_drain_start_us_);
    if (gap_us > stats_.longest_drain_gap_us) {
      stats_.longest_drain_gap_us = gap_us;
    }
    if (gap_us > storm_.longest_drain_gap_us) {
      storm_.longest_drain_gap_us = gap_us;
      stats_.storm_longest_drain_gap_us = gap_us;
    }
  }
  last_drain_start_us_ = now_us;
}

// A storm is a run of flash writes close enough together to be one event. It
// ends when nothing has been written for STORM_IDLE_MS, and only then are its
// worst numbers worth reporting - a maximum is meaningless averaged across
// unrelated storms.
void FlashWriteBroker::close_storm_if_idle(uint64_t now_us) {
  if (!storm_open_) {
    return;
  }
  if (now_us - last_operation_end_us_ < (uint64_t)STORM_IDLE_MS * 1000) {
    return;
  }

  storm_open_ = false;
  if (hooks_.report) {
    hooks_.report(storm_);
  }
}

bool FlashWriteBroker::begin_operation() {
  if (depth_.fetch_add(1) > 0) {
    return false;  // Nested write: the outermost window already covers it.
  }

  const uint64_t now_us = hooks_.now_us ? hooks_.now_us() : 0;
  close_storm_if_idle(now_us);

  if (!storm_open_) {
    storm_ = {};
    storm_open_ = true;
    // The mirrors the performance page reads follow the storm they describe.
    // Leaving them alone would show the finished storm's worst numbers against
    // the new one until it happened to beat them.
    stats_.storm_operations = 0;
    stats_.storm_longest_operation_us = 0;
    stats_.storm_longest_drain_gap_us = 0;
    stats_.storm_drain_timeouts = 0;
  }

  pre_drain();

  operation_start_us_ = hooks_.now_us ? hooks_.now_us() : 0;
  return true;
}

void FlashWriteBroker::end_operation(bool outermost) {
  depth_.fetch_sub(1);
  if (!outermost) {
    return;
  }

  const uint64_t now_us = hooks_.now_us ? hooks_.now_us() : 0;
  const uint32_t operation_us = (uint32_t)(now_us - operation_start_us_);
  last_operation_end_us_ = now_us;

  stats_.operations++;
  stats_.last_operation_us = operation_us;
  stats_.total_operation_us += operation_us;
  if (operation_us > stats_.longest_operation_us) {
    stats_.longest_operation_us = operation_us;
  }

  storm_.operations++;
  storm_.total_operation_us += operation_us;
  if (operation_us > storm_.longest_operation_us) {
    storm_.longest_operation_us = operation_us;
  }
  stats_.storm_operations = storm_.operations;
  stats_.storm_longest_operation_us = storm_.longest_operation_us;

  // The drainage gap. The owner runs at a fixed cadence, so one yield is one
  // drain pass - enough to empty what arrived during the operation before the
  // next one turns the cache off again.
  if (hooks_.gap) {
    hooks_.gap();
  }
}

void FlashWriteBroker::poll() {
  if (!storm_open_ || !hooks_.now_us) {
    return;
  }
  close_storm_if_idle(hooks_.now_us());
}

void FlashWriteBroker::reset_stats() {
  stats_ = Stats();
  storm_ = StormSummary();
  storm_open_ = false;
}
