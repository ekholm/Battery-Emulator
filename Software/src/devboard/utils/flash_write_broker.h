#ifndef _FLASH_WRITE_BROKER_H_
#define _FLASH_WRITE_BROKER_H_

#include <stdint.h>
#include <atomic>
#include <functional>

/**
 * A flash program or erase parks BOTH cores. The SPI flash controller cannot
 * serve instruction fetches while it writes, so the cache is turned off, every
 * task stops for the duration of the operation, and nothing drains the CAN
 * controllers' receive FIFOs. A settings save or an OTA upload is a long train
 * of such operations, which is why CAN frames go missing during one.
 *
 * The broker makes the write side cooperate with the CAN side. Every runtime
 * flash write funnels through run(), which does:
 *
 *   1. pre-drain - ask the CAN owner task to run its receive pass, and wait for
 *      it to say it has, so the queues enter the window as empty as they can
 *      be. WHICH queues, precisely (checked per driver): receive_can()
 *      reads a SOFTWARE queue on all three controller classes - the native
 *      TWAI ring (ACAN_ESP32::mDriverReceiveBuffer), the MCP2518FD ring, and
 *      the MCP2515's FreeRTOS rx queue, whose SPI reads happen in a task of
 *      its own. The chips' hardware FIFOs are emptied by an ISR, or by a task
 *      an ISR wakes, and those are exactly what a cache-off window stops. So
 *      the pre-drain buys headroom in the software queues for the burst that
 *      arrives when the window ends; it does NOT shorten what the hardware
 *      FIFOs have to survive on their own. That is the window itself, which is
 *      why this branch also shortens the window (erase in sectors, not blocks)
 *      rather than relying on the drain alone;
 *   2. ONE flash operation - the shortest unit the caller can issue;
 *   3. a drainage gap - yield, so the owner gets to drain again before the next
 *      operation starts.
 *
 * A storm therefore becomes a train of short windows with drainage between
 * them instead of one long stall, and the FIFO depth only has to cover a single
 * operation rather than a whole save.
 *
 * The broker also measures: every window is timed, and the longest window of a
 * storm is reported when the storm ends. That number is what says whether the
 * FIFOs are in fact deep enough - see FlashWriteBroker::StormSummary.
 *
 * Nothing here is ESP32-specific; the platform arrives through Hooks, which is
 * also what lets the policy be tested on the host.
 */
class FlashWriteBroker {
 public:
  /** How long to wait for the CAN owner to acknowledge a drain request. The
   * owner drains every millisecond, so this only expires when it is stuck - and
   * a stuck owner is not a reason to refuse a settings save. */
  static constexpr uint32_t DRAIN_ACK_TIMEOUT_MS = 5;

  /** A storm is over once this long passes with no flash write at all. */
  static constexpr uint32_t STORM_IDLE_MS = 1000;

  /** What one storm cost, handed to the report hook when the storm ends. */
  struct StormSummary {
    /** Flash operations the storm contained */
    uint32_t operations;
    /** The longest single brokered operation, in microseconds.
     *
     * NOT the same as the longest cache-off window, and measured on silicon it
     * can be far larger: an OTA chunk asks the flash driver to erase a whole
     * 64 KB block, and the driver serves that as a train of sector erases with
     * the cache back ON between them. One brokered call, many windows. Use it
     * to see which writes are expensive; use longest_drain_gap_us for what the
     * FIFOs actually have to survive. */
    uint32_t longest_operation_us;
    /** Time spent inside flash operations, summed over the storm */
    uint64_t total_operation_us;
    /** The longest the CAN owner went without draining, in microseconds. This
     * is the number the receive FIFOs have to cover, and it is measured from
     * the drain side, so it counts every cause of starvation rather than only
     * the ones the broker knows about. */
    uint32_t longest_drain_gap_us;
    /** Drain requests the owner never acknowledged in time */
    uint32_t drain_timeouts;
  };

  /** The platform. Every hook may be left empty; an empty hook is simply not
   * called, which turns the corresponding step off. */
  struct Hooks {
    /** Microsecond clock. Without it no window can be measured. */
    std::function<uint64_t()> now_us;
    /** Yield the CPU for about one owner pass. */
    std::function<void()> gap;
    /** True when the caller is allowed to block waiting for an acknowledgement.
     * False for the owner task itself - it cannot wait for its own drain - and
     * before an owner exists at all. */
    std::function<bool()> may_wait;
    /** Drain every receive FIFO here and now, in the caller's context. Called
     * instead of the rendezvous when may_wait() is false. */
    std::function<void()> drain_in_line;
    /** Called when a storm ends, with what it cost. */
    std::function<void(const StormSummary&)> report;
  };

  void set_hooks(const Hooks& hooks) { hooks_ = hooks; }

  /**
   * Run one flash operation under the broker: pre-drain, operation, gap.
   *
   * A template rather than a std::function parameter so that a capturing lambda
   * costs nothing at the call site - this sits in front of every settings key
   * and every OTA chunk.
   *
   * Re-entrant calls (an operation that itself writes flash) run the inner
   * operation directly: the drain and the gap belong to the outermost window,
   * which is the one that is actually cache-off.
   *
   * KNOWN LIMITATION: "re-entrant" is decided from a single depth count,
   * so a write issued by ANOTHER task while one is in flight is treated as a
   * nested one - it skips its own pre-drain and drainage gap, and its time is
   * charged to the operation already open. That case is real rather than
   * theoretical: core_loop saves the inverter watchdog setting from its 1 s
   * branch on one core while the AsyncTCP task brokers OTA chunks on the other.
   * The depth count is atomic so the two cannot corrupt it into a state where
   * every later write looks nested, but telling concurrency apart from nesting
   * needs a caller-context hook the platform-free policy does not have yet.
   */
  template <typename Operation>
  void run(Operation&& op) {
    const bool outermost = begin_operation();
    op();
    end_operation(outermost);
  }

  /** Called by the CAN owner task immediately BEFORE it drains every FIFO.
   * The request count is latched here rather than after the drain, or a request
   * that arrived while the FIFOs were already being read would be answered by a
   * pass that never saw it. Also where the drain gap is measured. */
  void drain_starting();

  /** Called by the CAN owner task immediately AFTER that drain. */
  void drain_completed() { drain_served_.store(drain_in_progress_); }

  /** Called periodically by the owner so the last storm of a burst gets
   * reported without waiting for the next flash write to arrive. */
  void poll();

  /** Live counters, for the performance page. Reset by reset_stats(). */
  struct Stats {
    uint32_t operations = 0;
    uint32_t longest_operation_us = 0;
    uint32_t last_operation_us = 0;
    uint32_t longest_drain_gap_us = 0;
    uint32_t drain_timeouts = 0;
    uint64_t total_operation_us = 0;
    /** The storm in progress, or the last one if idle. Cleared when a new storm
     * opens, so a page read between two storms cannot show the previous one's
     * worst numbers as if they belonged to the new one. */
    uint32_t storm_longest_operation_us = 0;
    uint32_t storm_longest_drain_gap_us = 0;
    uint32_t storm_operations = 0;
    uint32_t storm_drain_timeouts = 0;
  };

  const Stats& stats() const { return stats_; }
  void reset_stats();

 private:
  bool begin_operation();
  void end_operation(bool outermost);
  void pre_drain();
  void close_storm_if_idle(uint64_t now_us);

  Hooks hooks_;
  Stats stats_;

  /** Bumped by a writer asking for a drain, matched by the owner's
   * acknowledgement. Two counters rather than a flag so an acknowledgement that
   * arrives for an older request cannot satisfy a newer one. */
  std::atomic<uint32_t> drain_requested_{0};
  std::atomic<uint32_t> drain_served_{0};
  /** Owner-task only: the request count the pass in progress will answer. */
  uint32_t drain_in_progress_ = 0;

  /** Nesting depth. Atomic because two tasks on two cores enter run() at the
   * same time in the ordinary case (see run()): a lost increment on a plain
   * counter would leave the depth stuck above zero, and every write for the
   * rest of the boot would be silently treated as nested - no pre-drain, no
   * drainage gap, no measurement. */
  std::atomic<uint32_t> depth_{0};
  uint64_t operation_start_us_ = 0;
  uint64_t last_operation_end_us_ = 0;
  uint64_t last_drain_start_us_ = 0;
  bool storm_open_ = false;
  StormSummary storm_ = {};
};

/** The one broker every runtime flash write goes through. */
FlashWriteBroker& flash_write_broker();

#endif  // _FLASH_WRITE_BROKER_H_
