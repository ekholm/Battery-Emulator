#ifndef OTA_CONFIRM_GATE_H
#define OTA_CONFIRM_GATE_H

#include <stdint.h>

/* WHEN a freshly updated image has earned confirmation, and WHO performs it.
 *
 * The decision and the flash write are deliberately split across two contexts,
 * and the split is about routing, not cost:
 *
 *   CHECK  - the 1 ms core tick, unconditionally. That tick alternating for 42 s
 *            is the strongest liveness witness this firmware has: CAN is being
 *            received, the 10 ms and 1 s sub-tasks are both being reached, and
 *            the task watchdog has been fed the whole time. All this side does
 *            is set a flag.
 *   WRITE  - ordinary main-task context. Confirming an image writes otadata,
 *            which is a runtime flash write like any other and belongs on the
 *            same path as every other one (the planned flash-write broker in
 *            the end state: pre-drain, op, drainage gap, so the cache-off
 *            window lands on empty CAN FIFOs). Initiating it from the 1 ms
 *            tick would bypass
 *            that discipline. It would NOT save the tick a stall - during a
 *            flash op the scheduler is suspended whoever started it - so
 *            routing is the whole of the argument.
 *
 * Nothing here touches esp_ota_*: this is the policy, and it holds no opinion
 * about how the confirmation is written. That is what lets the 42 s boundary be
 * tested on the host, where ota_rollback.cpp cannot be built.
 */

// The uptime a fresh image must survive before it is confirmed.
//
// 42 s clears STA association, the TLS MQTT handshake and autodiscovery, so the
// crashes that only happen on first contact - the first frames parsed, the
// first MQTT or HTTP exchange, a watchdog trip under real load - still fall
// inside the window and still roll back. Criteria that sound stronger were
// rejected on purpose: "all RTOS tasks started" measures starting, not
// surviving, and anything externally dependent (CAN frames seen, WiFi up) would
// never confirm on a bench board with no bus or an offline install, so every
// reboot would revert a perfectly good image forever.
constexpr uint32_t OTA_CONFIRM_UPTIME_MS = 42 * 1000;

// CHECK side. Call unconditionally from a steady-state tick, passing the uptime
// in ms (millis(), not a delta). One guarded compare; sets a flag and no more.
//
// The comparison is against absolute uptime rather than an interval, so the
// millis() wrap at ~49.7 days is not a hazard: the flag latches 42 s into the
// first boot, tens of thousands of wraps before the counter comes back round.
void ota_confirm_check(uint32_t uptime_ms);

// A deliberate action that will replace or restart this image is its own
// liveness witness - the board was well enough to serve the request - so it
// arms the confirmation instead of letting the window run out. Two callers:
//
//   graceful_restart()  - an intentional reboot inside the window keeps the
//            update instead of reverting it. Called when the restart is
//            REQUESTED, not when it is enacted: the 5-10 s the graceful restart
//            spends pausing charge/discharge is what lets the write take the
//            An earlier version of this comment claimed a main task that
//            cannot complete one pass in those seconds is unhealthy, so
//            rolling it back is right "not a gap". Measured, that was wrong:
//            the main task is the LOWEST-priority task on the same core as
//            the highest-priority tick that fires the restart, so a healthy
//            board under sustained load can starve it past the deadline by
//            ordinary scheduling - the wild rollbacks this gate was built
//            against did exactly that. The gap is closed by restart_may_fire()
//            below: the tick DEFERS the restart while a confirmation is still
//            owed, up to a hard cap, so only a truly wedged main task still
//            rolls back - which is then honest.
//   onOTAStart() - an upload has begun, so the boot selection is about to move
//            to the slot being written. Serving a multi-megabyte HTTP upload is
//            the same class of witness as serving a restart request, and this
//            is the LAST moment the running image can still be confirmed: once
//            Update.end() calls esp_ota_set_boot_partition(), the write side
//            declines (BOOT_SELECTION_MOVED) and the image being replaced is
//            left PENDING_VERIFY for the bootloader to rewrite to ABORTED.
//
// The OTA caller is why this is an ARM and not a write. onOTAStart() runs in
// the async TCP task, where the routing rule above says confirmation writes do
// not belong; arming there costs two flag writes and leaves the otadata write
// on the ordinary main-task path, which loop() reaches within microseconds
// because loop() does nothing else. The ordering that matters is therefore
// decided by seconds of upload against one pass of a free-running loop - and
// even in the pathological case where the loop never runs, the write side's own
// BOOT_SELECTION_MOVED arm makes the outcome no worse than not arming at all.
//
// WHAT THE OTA CALLER COSTS, because "no worse" above is only about the loop.
// onOTAStart() is the upload's PRE-callback: it fires on /ota/start, before
// Update.begin(), so an upload that never completes - a bad hash, a dropped
// transfer, the inactivity timeout that ends the attempt - leaves the board
// running the image it was already running, now confirmed, with the rest of its
// window unearned. A crash that would have rolled that image back will not now.
// The witness is weakened from "this image ran for 42 s" to "this image served
// an upload request" - which is the witness a deliberate restart already gets,
// and a restart likewise confirms an image and then re-enters it. That is the
// trade, stated rather than discovered later.
void ota_confirm_request(void);

// Whether a confirmation is owed and not yet serviced. A single volatile word
// read; safe from any task. This is what lets the restart deadline defer.
bool ota_confirm_pending(void);

// WRITE side. True exactly once, on the first call after a confirmation becomes
// owed; false forever after. The caller performs the write.
//
// Once taken, no further confirmation is ever owed - including when the write
// itself fails. There is nothing useful to retry (a failure here means otadata
// is unwritable), and re-arming would repeat it on every pass of a loop that
// runs flat out. The failing write says so in the log instead.
bool ota_confirm_take_pending(void);

/* The restart deadline, and its interplay with an unserviced confirmation.
 *
 * graceful_restart() arms BOTH a confirmation request and a restart deadline.
 * The confirm WRITE runs in the main task (see the routing argument at the top
 * of this file); the deadline used to fire from the 1 ms tick regardless, and
 * on a starved main task that restarted the board with the image unconfirmed -
 * the bootloader then reverted a good update ("my revert did nothing").
 *
 * This predicate is the whole policy, pure so the race window is testable on
 * the host the same way the 42 s boundary is:
 *
 *   - no deadline yet: never fire.
 *   - deadline reached, nothing owed: fire - the normal path, unchanged.
 *   - deadline reached, confirm still owed: DEFER, so the main task can take
 *     its write; it normally needs milliseconds.
 *   - owed past the grace cap: fire anyway. A main task that could not run
 *     once in all that time is wedged, and rolling that image back is the
 *     honest outcome - the cap is what keeps the deferral from turning a wedge
 *     into a board that never restarts.
 *
 * The cap is measured on the ELAPSED time since the restart was ASKED FOR, not
 * on the length of the deferral, so it is one instant - 30 s - whichever
 * deadline was reached: 20 s of grace past the 10 s hard deadline, 25 s past
 * the 5 s paused one. That asymmetry is deliberate and it is the right way
 * round. What costs something is the wait itself - graceful_restart() opens the
 * contactors first, so the board sits paused for the whole of it - and that
 * wait starts when the restart is requested, not when the deadline lands. One
 * bound on the total is therefore the property worth holding; a per-path grace
 * would let the path that pauses SOONER keep the contactors open LONGER.
 *
 * Why 30 s and not, say, the 42 s window: the two numbers measure different
 * things. 42 s is how long a fresh image must SURVIVE before it has earned
 * confirmation; this is how long a main task may be STARVED before we stop
 * believing it will come back. The write itself needs one loop() pass -
 * milliseconds - so the whole of the grace is there to outlast a transient
 * stall, and the stalls actually seen on the bench are seconds to tens of
 * seconds - that is what the EVENT_TASK_OVERRUN payloads on the boards whose
 * updates rolled back were measuring. 30 s clears those with margin while
 * bounding the paused window at three times its old worst case.
 *
 * The deferral can only ever apply BEFORE the first serviced confirmation of a
 * boot: the gate latches (ota_confirm_take_pending() sets `taken` for good, and
 * ota_confirm_request() is a no-op after that), so from the first healthy
 * loop() pass onwards nothing is ever owed again and this predicate is the old
 * one exactly. A board whose main task is running does not inherit the grace.
 *
 * The deadlines are owned HERE, not in the caller: safety.cpp static_asserts
 * they equal its legacy INTERVAL_5_S/INTERVAL_10_S so the two cannot drift.
 * The MQTT/HTTP restart commands need no special casing on top of this:
 * graceful_restart() arms the confirmation for every requester, so any restart
 * asked for inside the verify window now waits for the write by construction.
 */
constexpr uint32_t RESTART_PAUSED_DEADLINE_MS = 5000;
constexpr uint32_t RESTART_HARD_DEADLINE_MS = 10000;
constexpr uint32_t RESTART_CONFIRM_GRACE_MS = 20000;

constexpr bool restart_may_fire(uint32_t elapsed_ms, bool paused, bool confirm_owed) {
  const bool deadline_reached =
      (elapsed_ms > RESTART_PAUSED_DEADLINE_MS && paused) || elapsed_ms > RESTART_HARD_DEADLINE_MS;
  if (!deadline_reached) {
    return false;
  }
  if (confirm_owed && elapsed_ms <= RESTART_HARD_DEADLINE_MS + RESTART_CONFIRM_GRACE_MS) {
    return false;  // the write path gets its chance; see the block comment
  }
  return true;
}

// WHICH image the confirmation may land on, decided before the write.
//
// esp_ota_mark_app_valid_cancel_rollback() takes no partition: it writes VALID
// into the ACTIVE otadata entry - the one with the highest sequence number -
// and that is the running image's entry only until something calls
// esp_ota_set_boot_partition(). Two paths do, and both then call
// graceful_restart(), which arms this gate: /revertFirmware, and a finished
// OTA upload (Update.end() selects the slot it just wrote). On either, a
// confirmation written after the selection moved lands on the TARGET, an
// image that has not run one instruction, while the running image stays
// PENDING_VERIFY and is rewritten to ABORTED by the bootloader on the way
// past. Measured on the bench on an in-window revert (the OTA-then-revert
// acceptance run): the reverted-into slot came up VALID with no window of its
// own. The same shape on an OTA
// inside the previous update's window ships the fresh image pre-confirmed,
// which folds the whole of the rollback protection away for that update.
//
// So the write side asks two things of the running partition - is it pending,
// and is it still the boot selection - and confirms only when both hold. The
// caller reads both from esp_ota_*; this stays pure so the host can pin all
// three outcomes.
enum class OtaConfirmVerdict {
  NOT_PENDING,           // an ordinary boot: nothing to confirm
  BOOT_SELECTION_MOVED,  // pending, but the write would land on another slot: decline
  CONFIRM,               // pending and still selected: write it
};
OtaConfirmVerdict ota_confirm_verdict(bool running_pending_verify, bool running_is_boot_selection);

#ifdef UNIT_TEST
// Test seam only: the gate is a boot-lifetime latch, and the host tests need to
// run more than one boot.
void ota_confirm_gate_reset(void);
#endif

#endif  // OTA_CONFIRM_GATE_H
