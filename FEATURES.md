# Features and conversions

*Prepared, not filed. Offered pull-style per maintainer preference: nothing here is a pull
request. Point at whichever entry is useful, ignore the rest.*

*These are the larger offerings - conversions, reworks, improvements. Each links its branch, the
pinned commit and the full diff against upstream `main`, and carries the PR body it would ship
with. Unlike a fix, an entry here may deserve a design conversation before code review - say so
and we will start one.*

**Kept current.** Every entry is re-checked against upstream `main`, and one whose need `main` has
since met is removed rather than left to waste your time. Every branch is rebased onto upstream
`main` at `a2851c23` (2026-09-02), rebuilt, and the host test suite run on the result - all green.

See also [FIXES.md](FIXES.md) for defect repairs, and [FINDINGS.md](FINDINGS.md) for open
questions on hardware we do not run.

---

**Ford Mach-E: hand the UDS transport to the shared superclass, and fix two latent superclass bugs it exposed**
Branch [`mache-uds-superclass`](https://github.com/ekholm/Battery-Emulator/tree/mache-uds-superclass) @ `f4142713` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:mache-uds-superclass)
The same move #2824 makes for the Zoe Gen2, applied to the Mach-E: the driver's hand-rolled diagnostics were a 1:1 duplicate of what `UdsCanBattery` already does. It keeps only what is genuinely Ford's, and the conversion exposed two latent bugs in the superclass itself - a queued sequence lost to a retry race, and a readout that leaves the page pending forever - which are fixed here and benefit four other drivers today.

<details>
<summary>PR body it would ship with</summary>

This does for the Mach-E what #2824 does for the Zoe Gen2: the driver's hand-rolled diagnostics - a 36-PID round-robin with its own request bookkeeping, an ISO-TP reassembly + flow-control state machine for the DTC readout, and the `19 02` / `14 FF FF FF` plumbing with retry deadlines - were a 1:1 duplicate of what `UdsCanBattery` already does. The driver now keeps only what is genuinely Ford's:

- the broadcast decode (cells, temps, SOC, limits), untouched;
- the PID value decode, moved into `handle_pid()` with the same expressions;
- its custom status page, unchanged (the DTC table uses Ford 5-char short codes as the `ford_machE_dtc.json` match key, which the generic UDS DTC section cannot emit);
- two protocol facts, expressed as new superclass knobs usable by any driver:
  - `dtc_status_mask()`: the BECM reports a useful set only under mask `0x8F`. The default stays `0x09` and is now pinned by a test, so no existing UDS driver changes behaviour.
  - `on_dtc_cleared()`: only a *confirmed* erase (`54` ack) puts the page back to "not read yet"; an unconfirmed erase leaves the previously read list untouched, which was the Mach-E's existing deliberate semantic. Default: no-op.

**It also fixes two latent superclass issues the conversion exposed.**

**(1) A queued sequence could be lost to a PID retry race.** `start_sequence()` while a PID request was mid-retry could dispatch on the retry's timeout tick; `send_sequence_message()` then refuses and the queued request is consumed without reaching the wire. A user-triggered DTC read, erase or BMS reset against a slow ECU could vanish on any UDS driver. The pending sequence is now held until the PID transaction resolves, with a regression test driving the race directly.

**(2) The pending-forever readout.** An internal DTC read or clear sequence that timed out fell through to the *subclass* timeout hook and resolved nothing, so a readout against a silent ECU left the web page pending forever. Internal sequences now resolve their own timeouts (a failed read marks `dtc_read_failed`), and internal states no longer leak into `on_uds_sequence_timeout()`. This benefits Zoe Gen1, CMFA, CMP smart car and MG Gen1 today.

**One deliberate wire-visible change:** the PID scan paces at the superclass's one request per 100 ms tick instead of one per 250 ms - same list, same order, readout still pauses the scan. That is the cadence every other UDS driver already runs.

**Tests grow with the conversion (net +8).** The multi-frame DTC readout test now drives the real wire path and additionally pins the `0x8F` mask and the flow-control answer. New: clear-ack resets the page while an unconfirmed clear leaves it alone; a silent-BMS readout resolves as failed; PID decode parity against golden vectors quoting the original parser's expressions; scan-order regression. New superclass tests: the default mask stays `0x09`, the default clear-ack keeps the list, a read timeout marks the read failed. Every new assert is mutation-checked - each reverted change fails its own test.

**Also worth disclosing:** the dead `0x142` poll case (12 V via the disabled `0x7DF` broadcast request) is removed - `polled_12V` still exists and the `EVENT_12V_LOW` check is untouched, and it could never fire before either, because the request frame was commented out. `dtc_status_mask()` is read at sequence start rather than cached.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**MG5: the same conversion, and the DTC readout stops being serial-log-only**
Branch [`mg5-uds-superclass`](https://github.com/ekholm/Battery-Emulator/tree/mg5-uds-superclass) @ `2c005363` · stacked on `mache-uds-superclass` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:mg5-uds-superclass)
The MG5 duplicated the same transport machinery, down to its own 1 KB ISO-TP reassembly context. It keeps what is genuinely MG5's - the broadcast decode, now pinned by golden tests for the first time, and the `0x8A` contactor-close handshake - and its DTC readout moves from the serial log to the standard UDS page with working read and erase buttons.

<details>
<summary>PR body it would ship with</summary>

Same move as the Mach-E conversion this builds on: the MG5's hand-rolled diagnostics - a 15-DID round-robin with in-flight flags and its own pacing, a 1 KB ISO-TP reassembly context with manual flow control every third consecutive frame, and the session/DTC request plumbing - duplicated `UdsCanBattery`. The driver keeps what is genuinely MG5's:

- the broadcast decode (BMS state, cell extremes, temps, SoC/V/I summary, per-cell voltages), untouched, and pinned by golden regression tests for the first time;
- the `0x8A` contactor-close handshake, including its close-triggers-a-DTC-erase reflex (DTC 293 blocks closing), now a deferred superclass `reset_DTC()`;
- the address autodetect (broadcast `0x7DF` until the BMS answers from `0x789` or `0x7ED`; the detected pair is pinned via `setup_uds`, which now also pins the response address - stricter than before);
- the extended-session reflex: the old code re-entered `10 03` on every transaction timeout, and the conversion queues it whenever the diag side has been quiet for 3 s;
- the wide DTC status mask (`19 02 FF`) via `dtc_status_mask()`.

**Behaviour upgrade, deliberate:** the DTC readout used to be printed to the serial log only. It now lands in the datalayer and renders on the standard UDS battery page, with working read and erase buttons, so the wiki's MG5 page gains a real diagnostics story. The 13 of 15 DID decodes that never stored anything (commented-out logging) are gone; SoH keeps its storage.

**Wire-visible changes, deliberate:** the scan paces at the superclass tick instead of one DID per 500 ms; ISO-TP flow control follows the superclass's standard pattern instead of one FC per three consecutive frames; detection costs one repeated `B041` poll, since the scan restarts.

**Tests (net +8):** scan order and broadcast-address start, both autodetect wirings, SoH decode parity (golden, from the original expression), the `19 02 FF` mask, the multi-frame DTC readout into the datalayer, quiet-side session re-entry, and golden broadcast vectors (`0x3AC` summary including the sign and scale of V/I, `0x173` cell extremes). Mutation-checked: the mask, the session reflex and a broadcast scale each fail their test when broken.

**Also worth disclosing:** the session reflex changes trigger (transaction timeout becomes a 3 s quiet window) - same recovery behaviour against a session-dropping BMS, different timing. The superclass respects the ISO-TP single-frame length where the old parser read fixed byte positions regardless, so a malformed short reply now yields a truncated value instead of reading padding; stricter, and considered correct. The 1 KB reassembly buffer is gone, since the superclass buffer is shared machinery.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**T-CAN485: give the SD card its own SPI controller, and check SD writes**
Branch [`sd-spi-bus-hspi`](https://github.com/ekholm/Battery-Emulator/tree/sd-spi-bus-hspi) @ `13bb1e94` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sd-spi-bus-hspi)
The SD card and the MCP2515 add-on share VSPI, and two `SPIClass::begin()` calls on one ESP32 controller cannot coexist - the card mounts, then goes deaf when `init_CAN()` runs. Every later log write failed with nothing reporting it, because the only SD event guards the mount. Two HAL overrides and checked write paths. Measured on hardware for the bus half.

<details>
<summary>PR body it would ship with</summary>

Two defects that only look like one because the second hides the first.

**The collision.** On a T-CAN485 the SD card and the MCP2515 CAN add-on are both on VSPI. Two `SPIClass::begin()` calls on one ESP32 controller cannot coexist: the second silently re-points the controller's single MISO source, and the first device stops being readable. So the sequence a user hits is SD logging plus a second CAN battery - the card mounts, and then goes deaf the moment `init_CAN()` runs. Every log write after that fails, and nothing says so.

The fix is two HAL overrides, and **the second is load-bearing rather than defensive**:

- `SD_SPI_BUS()` to HSPI, which moves the SD off the contended controller.
- `MCP2517_BUS()` to VSPI. It is tempting to say HSPI is free because this board has no FD chip soldered, and that is true of the hardware but false of the HAL: `available_interfaces()` offers `CanFdAddonMcp2518`, its pins are mapped to the same external header the 2515 uses, and the classic-ESP32 default for that bus is HSPI. Moving only the SD would have traded the collision - fixed for SD + 2515, newly created for SD + FD add-on.

With both overrides the truth table closes: SD alone on HSPI, and whichever CAN add-on is present alone on VSPI. Both add-ons at once is physically impossible - they are the same header pins - and is already refused, safely: `init_CAN()` allocates pins *before* it calls `begin()`, so a both-configured board raises `EVENT_GPIO_CONFLICT` and never reaches a second `begin()`. Had those two lines been ordered the other way, the refusal would have arrived too late to matter.

**The survey is complete rather than sampled.** `SD_SPI_BUS()` is pure-virtual inside `#ifdef SDCARD`, so every board compiled with SD must override it - and only two envs define SDCARD at all. The other one is dfrobot_edge101, whose `available_interfaces()` returns native CAN only, so nothing there can claim a second controller. One board had the defect and it is the one changed.

**The silence.** `EVENT_SD_INIT_FAILED` guards the mount and nothing after it, so any post-mount death was invisible - the collision above, but equally a pulled card, a full one, a bad connector. Every SD result on the write paths was discarded: `SD.open()` at four sites, each followed by `file_open = true` regardless of what it returned, and both `write()` calls. The open half is the worse one, because marking the file open after a failed open converts a single failure into an unbounded run of writes to an invalid `File`, every one of them silent.

Both paths now go through checked helpers: the open flag is set from the `File`'s own truthiness, a short write is treated as the failure it is, and a new `EVENT_SD_WRITE_FAILED` says so. (`flush()` returns void and cannot report; the comment says that rather than leaving it looking overlooked.)

Two deliberate choices in that event. It is **WARNING, not ERROR**, because `update_bms_status()` turns any active error into `system_status = FAULT` and an optional log dying must not fault a running emulator. And it is **latched**, so that it survives `clear_event()`: the evidence of this particular failure is data that is *missing*, so the record of it must not be tidiable away by code - only by the user, from the events page. The event text names the routes that actually retry the open.

**Measured on hardware for the bus half.** On a real T-CAN485, three boots each, fix against an unmodified control built alongside it: before, VSPI's MISO input ends up owned by the MCP2515 and HSPI's is owned by nobody; after, the two are held simultaneously - MCP2515 on VSPI, SD on HSPI - 3/3. The host suite covers both halves with ten new cases, and every mutation is caught by exactly its own test: the SD moved back, the second override dropped or pointed at the wrong bus, the open result assumed good again, the write returns discarded, the event unlatched or raised at ERROR.

**One number deliberately not quoted.** CAN initialisation also gets dramatically faster on the instrumented board, but the card slot was empty in both runs, so that interval is a *failing* mount's retry cost and not what a user with a card in the slot would gain. It is a real effect in the right direction and the wrong magnitude to advertise.

**Not verified on hardware: the silence half.** Provoking it honestly needs a card that dies after mounting, and both bench slots are empty. That half is compile- and source-verified only.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Silent assertions: drop the assert message strings, keep every check (−55 KB flash per board)**
Branch [`assertions-silent`](https://github.com/ekholm/Battery-Emulator/tree/assertions-silent) @ `66d242da` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:assertions-silent)
One config line in the shared size defaults; every check still compiled in and still aborts - only the per-assert message strings go. Measured −55,680 B (lilygo) / −55,432 B (devkit) from wiped, flag-verified builds.

<details>
<summary>PR body it would ship with</summary>

This enables `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT` in the shared `sdkconfig.be_size.defaults`, so it applies to every board env identically.

**What it does:** every `assert()`/IDF invariant check is compiled in and still aborts on failure, exactly as today. Only the per-assert message strings (file/expression text) are dropped - and the strings are where the flash mass is.

**Measured** (wiped build dirs, `sdkconfig.<env>` deleted before each build and its regeneration with the expected flag verified - PlatformIO can silently skip a config change under some cache states; the lilygo pair below was built baseline-then-SILENT inside one continuous build-lock hold):

| env | baseline | SILENT | delta | full-DISABLE (reference) |
|---|---|---|---|---|
| lilygo_330 | 1,866,720 B | 1,811,040 B | **−55,680 B (−2.98 %)** | −65,040 B |
| esp32devkit_330 | 1,821,232 B | 1,765,800 B | **−55,432 B (−3.05 %)** | −64,704 B |

That is ~85.6 % of the full `ASSERTIONS_DISABLE` saving with none of its safety cost - and a word of caution on the other half: **`_DISABLE` is the wrong move on a safety-related system.** It compiles the checks OUT, so a broken invariant no longer aborts - the firmware keeps running on falsified assumptions, on hardware that drives contactors. Tests cover the cases we thought of; asserts exist for the ones we didn't. The remaining ~9.4 KB is, in effect, the price of every runtime invariant in the image - the right 9.4 KB to keep. SILENT needs no stability gate at all, which is why it could merge today.

**The honest cost:** an assert failure now aborts with the program counter instead of the message string. The backtrace still prints and decodes against the ELF of that build:

    xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <PC>

**Cost / pace / breakage:** abstraction cost - none, one config line, no code changes; pace - nothing changes in anyone's workflow; breakage plan - revert is the same one line, and any assert that fires still aborts loudly, so a problem presents exactly as today, minus one string.

Note: drafted with AI assistance, reviewed by me.

</details>
