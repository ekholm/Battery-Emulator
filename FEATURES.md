# Features and conversions

*Prepared, not filed. Offered pull-style per maintainer preference: nothing here is a pull
request. Point at whichever entry is useful, ignore the rest.*

*These are the larger offerings - conversions, reworks, improvements. Each links its branch, the
pinned commit and the full diff against the upstream release `v12.6.0`, and carries the PR body it
would ship
with. Unlike a fix, an entry here may deserve a design conversation before code review - say so
and we will start one.*

**Kept current.** Every entry is re-checked against upstream `main`, and one whose need `main`
has since met is removed rather than left to waste your time.

*Each entry's branch line names the upstream commit its branch is rebased onto. Since the
2026-09-21 base ruling that is the latest upstream RELEASE, not whatever `main` reads today, and a
new release rather than a merge is what triggers the next uplift cycle. The branch was rebuilt
there and the host test suite run on the result, all green, unless its entry says otherwise.*

See also [FIXES.md](FIXES.md) for defect repairs, and [FINDINGS.md](FINDINGS.md) for open
questions on hardware we do not run.

---

**OTA: revert to the previous firmware, and the confirmation path it exposed - requested by the maintainer**
Branch [`ota-revert`](https://github.com/ekholm/Battery-Emulator/tree/ota-revert) @ `fcbb3fa9` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:ota-revert) · one commit
A web-UI control to boot the other OTA slot. Most of the feature is the states it must refuse - a USB-flashed board with no passive image, a slot a rollback already marked aborted, a half-written slot that fails validation at click time - each rendered as a reason, never a dead button. Building it exposed the confirmation path around it, and the second half hardens that: the revert button reports the server's answer instead of blind-reloading, and the page counts consecutive failed polls at one-second cadence, so a single dropped request during the restart cannot read as a rollback, and a rollback verdict stays on screen until it is dismissed instead of being wiped by the page's own refresh; a restart deadline defers while a confirmation is still owed, so a healthy board under load cannot be rolled back by ordinary scheduling; a confirmation is a statement about the image that has run, so the write declines once the boot selection has moved; and an upload beginning is the last moment the running image can still be confirmed, so OTA start arms it - two flag writes in the TCP task, the otadata write stays on the main-task path. Run on hardware: an in-window revert flips slots cleanly and the reverted-into image earns its own confirmation window. Ships with the refusal-ladder, gate and feedback test suites.

<details>
<summary>PR body it would ship with</summary>

Every dual-slot board already has the previous firmware in the passive OTA slot. What was missing was a UI telling the user whether the passive slot is actually usable before they commit to a reboot.

The feature is the set of states in which the control must not be offered. A board flashed over USB has no valid passive image. A slot an automatic rollback already rejected is marked ABORTED and the IDF refuses it - so the control becomes unavailable exactly when a rollback has just fired, and it says why rather than showing a dead button. A revert inside the confirmation window is one-way, because the bootloader marks the departing slot ABORTED before it selects anything; the dialog says so and quotes the window length from the confirmation gate's own constant, so the text and the threshold cannot drift apart.

The decision lives in `ota_revert_assessment()`, a pure function of four facts the caller extracts on the target, so the full state matrix is covered by host tests. `esp_ota_set_boot_partition()` re-validates the image at click time, so a half-written slot is refused there.

Building the revert exposed five problems in the confirmation path. The page now reads the server's reply rather than blind-reloading. One dropped poll during restart is not a rollback - the detection latch is a count of consecutive failures, not a boolean. A rollback verdict is stored with the running version and stays on screen until dismissed rather than being wiped by the page's 15 s reload. The restart deadline defers while a confirmation is still owed, so a healthy board cannot be rolled back by ordinary scheduling. The confirmation write is gated on the running image still being the boot selection - a revert or an OTA upload inside the previous update's window could otherwise ship the arriving image pre-confirmed; OTA start arms the confirmation instead of skipping it.

Three new test files cover the feature: `test/ota_revert_tests.cpp` (the assessment matrix), `test/ota_confirm_tests.cpp` (the confirmation gate and target-aware write), and `test/ota_revert_feedback_tests.cpp` (poll-count and verdict-persistence). The fact extraction that feeds the decision lives in `webserver.cpp`, which is not in the host test binary; that part is bench-verified only. The poll-count logic was also exercised from a real browser to confirm that a single injected connection failure does not trigger the rollback verdict and that the counter clears on an answering poll.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Ford Mach-E: hand the UDS transport to the shared superclass, and fix two latent superclass bugs it exposed**
Branch [`mache-uds-superclass`](https://github.com/ekholm/Battery-Emulator/tree/mache-uds-superclass) @ `377b211f` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:mache-uds-superclass)
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
Branch [`mg5-uds-superclass`](https://github.com/ekholm/Battery-Emulator/tree/mg5-uds-superclass) @ `735f9a75` · on release `v12.6.0` @ `f7d65fc2` · stacked on `mache-uds-superclass` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:mg5-uds-superclass)
The MG5 duplicated the same transport machinery, down to its own 1 KB ISO-TP reassembly context. It keeps what is genuinely MG5's - the broadcast decode, now pinned by golden tests for the first time, and the `0x8A` contactor-close handshake - and its DTC readout moves from the serial log to the standard UDS page with working read and erase buttons.

Note on upstream direction: upstream's MGHS driver (`MG-GEN1`, already on the UDS superclass) has begun absorbing MG5 variants - the 50 kWh LFP is detected today and the hardware-number table knows the 52 kWh NMC this driver serves. If that route wins, this conversion retires with the driver it converts, and retiring it would be fine. Until then it keeps the 52 kWh pack's broadcast decode golden-tested and its DTC readout on the standard page - and if the better end-state is one MG driver, we would rather help that happen than defend this one. Worth a design conversation before code review.

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
Branch [`sd-spi-bus-hspi`](https://github.com/ekholm/Battery-Emulator/tree/sd-spi-bus-hspi) @ `e831f21d` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sd-spi-bus-hspi)
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
Branch [`assertions-silent`](https://github.com/ekholm/Battery-Emulator/tree/assertions-silent) @ `f27e34bc` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:assertions-silent)
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

---

**Boards as declarations: one file per board, its pin block generated into the header, and checks that make the declaration the source of truth**
Branch [`feature/board-capability-tooling`](https://github.com/ekholm/Battery-Emulator/tree/feature/board-capability-tooling) @ `428c36f8` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:feature/board-capability-tooling) · 37 commits
Each of the eight boards gets one declaration, `Software/boards/<board>.yaml`, in which a feature owns its pins and a shared bus is declared once and referenced by whatever sits on it - so pin sharing is visible rather than implied by two getters happening to return the same GPIO. The board also declares its chip and flash size, and those are cross-checked against the env that builds it and against its own pins (the ESP32-S3 has no GPIO 22-25), so a wrong one is caught by the declaration contradicting itself. A generator rewrites the constant block inside the existing HAL header, between markers: the header stays the one file a reader opens, and getters whose pin is chosen at runtime or by variant stay hand-written below the block. A verifier proves the migration changed nothing - every generated line is verbatim from the header as it was before the tooling existed - and hardware no original header had, such as the Edge101's Ethernet PHY, has to be acknowledged by name with a reason rather than passing quietly.
The declarations are checked rather than trusted, and each refusal has its own test: a required pin missing or declared NC, a reference to an undeclared bus, an unknown driver or field, more instances than the driver has, both card interfaces on one board, two outputs on one GPIO, an Ethernet pin colliding with the SD chip select, a pad the declared chip does not have. Add-on modules - a CAN controller on a header, the isolated dual-FD card - are described too, so a signal the module receives bound to a pin the chip can only read becomes a build error naming board, add-on, signal and pin instead of a line that silently never asserts. Every declared board must also be compiled by CI, which is how the 3LB got its first build env. A check job runs all of it.
From the same declarations it emits each board's capability set as a constant expression - what a registry would ask instead of testing the board name - plus the pad and pin-role tables an on-device pin validator would need. Nothing in the firmware reads those yet: this is the build-time half. It also answers one question outright, in a checked-in report: whether a single image could identify its board at runtime by probing for parts. Today it cannot do so safely - reaching the part that separates the remaining candidates drives pins that are contactor, precharge or wake-up lines on the other boards in contention - and a pin change that alters that answer arrives as a diff.
This is the code behind [dalathegreat/Battery-Emulator#2737](https://github.com/dalathegreat/Battery-Emulator/issues/2737), and the build-time half of what that discussion reframed it towards: boards as configuration rather than as build variants. Worth a design conversation before code review. Published 2026-08-07, head last moved 2026-09-14.

<details>
<summary>PR body it would ship with</summary>

This is the code behind issue #2737. Eight YAML board declarations drive a Python generator that rewrites the constant-pin block inside each existing HAL header, between markers. A reviewer opening `hw_lilygo.h` still opens one file; the generated block sits between `// BEGIN GENERATED` and `// END GENERATED` markers and the rest of the file is unchanged.

**What is generated and what stays hand-written.** The generator writes the constant getters - pins that are a single GPIO number. Getters whose GPIO is chosen at runtime (a `setting:` or `variant:` in the YAML) emit nothing and stay hand-written below the block; the role is still declared so validation is not bypassed. `tools/board_gen.py` is the generator. `tools/board_verify_transcription.py` is the migration proof: it requires every generated member line to appear verbatim in the header as it was before the tooling existed. 228 member lines are checked across all eight boards. A pin that changed would fail it.

**What else the generator emits.** `Software/src/devboard/hal/capabilities.h`: a `BoardCap` enum built from the union of declared features, a `constexpr` capability set per board, and `BE_BOARD_CAPS` bound to the active board. No firmware code reads this for runtime gating yet - the enum is the build-time half. Also `Software/src/devboard/settings/pin_settings_rows.inc`: one settings row per user-movable role, keyed as `PIN<id>` with ids in the append-only sidecar `tools/pin_role_ids.json`. The settings page does not yet splice this in.

**Proof firmware is unchanged.** ELF section identity was checked across five locally buildable envs after the headers gained their generated blocks: every allocated section identical, every symbol identical in name and size. One later commit found and measured a 64-byte vtable inflation introduced by an emitted keyword, and measured the fix back to zero.

**Suggested review order:**

*Core tooling:* `980b2e9b` (declarations + generator foundation), `5ea46827` (generate in-place, feature-centric schema with shared bus declarations), `156b0e27` (baseline tracks merge-base rather than a hardcoded SHA). These establish the YAML shape and the marker convention.

*Capability layer:* `d0e0fee5` (validate capabilities against declared pin map), `702b0c51` (emit `capabilities.h`), `a6ac3360` (Ethernet support and cross-feature pin-conflict check). The key design decision is that `sd_mmc` and `sd_spi` both `provide: SdCard`, so a consumer asks one question rather than an OR.

*Board completeness:* `a910aeae` (Edge101, eighth board), `85d91d6a` (acknowledgement mechanism for hardware the pre-tooling headers never had - needed for Edge101's Ethernet getters), `11c69965` (chip and flash size declarations cross-checked against platformio.ini and against the board's own pin ranges), `a9e5fb8a` (3LB build env, which the board had lacked since 2024), `1b636b5d` (CI must compile every declared board - the structural check that would have caught the 3LB gap).

*Module add-on validation:* `df167e595` (add-on module templates and direction check). A signal the module receives that is bound to an input-only pad is now a build error naming board, add-on, signal and pin. `mcp2515` gains an `optional:` reset line, because three of the four boards carrying that part tie RST high.

*Probe plan:* `90dd4cfc` and its test commits. The declarations already know every board's pin roles and SPI wiring, so the question "which boards can safely identify themselves at runtime by probing for parts" is answerable from them. The answer today is that three of the four undecided groups cannot be resolved safely - the probes that would separate them drive contactor, precharge or wake-up pins on the boards still in contention. The report is checked in as `Software/boards/probe-plan.md` and regenerates from the declarations, so a pin change that alters the safety answer arrives as a diff.

*Wizard series:* `8779e71d` (pad properties per chip and role-constraint tables, needed for the on-device validator) and `e6c7ca93` (PIN row emission and role-id sidecar). CLI-only; no firmware consumer yet.

*Review fixes at the tip:* `8b3d0a42` (member-visibility correction, 64-byte vtable, measured) and `49c4fd86` (MCP2515 interrupt requirement, asymmetry with the CAN-FD case pinned from both sides).

`tools/test_board_validation.py` (759 lines) is the main refusal suite; `tools/test_probe_plan.py` (220 lines) and `tools/test_transcription_verify.py` (143 lines) cover the other two tools. A check job in `.github/workflows` runs all three plus `board_gen.py --check`.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**TESLA-LEGACY: a test that pins the capacity-by-hardware-ID table**
Branch [`tesla-legacy-100kwh-capacity`](https://github.com/ekholm/Battery-Emulator/tree/tesla-legacy-100kwh-capacity) @ `83936fca` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:tesla-legacy-100kwh-capacity)
**The defect this branch was written for is gone - you fixed it in `b5d9df5f8` ("Fix capacity autodetect on 100kWh packs"), landing the same one-line change it carried: hardware IDs 79 and 89 now report 100000 Wh rather than 70000.** That fix shipped without a test, so what is offered here is only the coverage, and it is offered because nothing currently pins any of the six groups.

`test/battery/tesla_legacy_capacity_tests.cpp` walks every hardware-ID group in `update_values()` and asserts the capacity each reports against the label the switch carries - all six groups, not just the one that was wrong - so a future edit to that switch cannot silently move a group's value again. It also pins two behaviours around it: hardware ID 0 leaves a preloaded capacity alone, and a known hardware ID overwrites a stored capacity on every update, which is why a user cannot correct such a value from the settings page and why it has to be right at the source.

No behaviour change: the branch adds a test file and touches no driver. The original defect report is #2673, an owner running a Model X 100 kWh whose page read "Total capacity: 70.0 kWh".

<details>
<summary>PR body it would ship with</summary>

Upstream commit `b5d9df5f8` ("Fix capacity autodetect on 100kWh packs", 2026-09-19) shipped the same one-line fix this branch carried: `case 79:` and `case 89:` now set 100000 rather than 70000, the value of the 70 kWh group four cases above them. That fix shipped without tests, and nothing currently pins any of the other five groups either. This branch offers the coverage alone; there is no behaviour change.

`test/battery/tesla_legacy_capacity_tests.cpp` (103 lines, one new file) walks every hardware-ID group in `update_values()` and asserts the capacity each reports against the label the switch carries - all six groups, not only the one that was wrong. A future edit to that switch cannot now silently move any group's value.

Two behaviours around the switch are also pinned: hardware ID 0 leaves a preloaded capacity alone, and a known hardware ID overwrites a stored capacity on every update. The second matters to users directly, because it is why a corrected value entered from the settings page does not persist - the table has to be right at the source.

The original defect is upstream issue #2673, an owner running a Model X 100 kWh whose page read "Total capacity: 70.0 kWh". The mismatch had been in the switch since its first commit (PR #1946).

Note: drafted with AI assistance, reviewed by me.
</details>

---
**Flash writes no longer starve the CAN receive FIFOs: a broker, sector erases, and the measurement that says what is left**
Branch [`flash-write-interleave`](https://github.com/ekholm/Battery-Emulator/tree/flash-write-interleave) @ `55801b8c` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:flash-write-interleave)
A flash program or erase parks both cores: the cache is off for the whole operation, no task runs, and nothing drains the CAN controllers' receive FIFOs. That is why frames go missing during a settings save or an OTA upload. This funnels every runtime flash write through a broker that drains CAN first, runs ONE operation, and yields so the drain happens again before the next - turning a storm into a train of short windows. It also switches OTA erases from 64 KB blocks to 4 KB sectors, and measures what is left: on a T-CAN485 the longest gap between CAN drains during an OTA falls from **319-332 ms to 81-82 ms**. It does not claim zero loss, and says exactly why.

*Overlaps the CAN lane (the next entry).* The sector-erase half of this branch is the same `CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE` option the lane carries as [`ota-erase-granularity`](https://github.com/ekholm/Battery-Emulator/tree/ota-erase-granularity), so take that half from one of them, not both. The broker half is not in the lane: an A/B on the lane, with and without this broker under a settings storm and an OTA upload, measured no difference on its IRAM-resident receive paths.

<details>
<summary>PR body it would ship with</summary>

A flash program or erase parks both cores. The cache is off for the whole operation, no task runs, and nothing drains the CAN controllers' receive FIFOs - which is why frames go missing during a settings save or an OTA upload. The FIFOs are what has to cover the window, and today they are asked to cover a whole save rather than a single write.

**The broker.** Every runtime flash write funnels through `run()`, which pre-drains (asks the CAN owner task to empty its queues, and waits until a pass that STARTED after the request says it has), runs one operation, then yields so the owner drains again before the next. A storm becomes a train of short windows with drainage between them. The platform arrives through a `Hooks` struct, so the policy is executed by the tests rather than read: the fixture is the clock, the yield and the CAN owner at once.

**What it is routed through.** `BatteryEmulatorSettingsStore` brokers each put and each remove - one NVS key is the shortest flash operation that API can issue, so a save of N changed keys becomes N short windows instead of one stall as long as the save. Batching into a single commit would NOT have helped: Preferences commits per key and `nvs_commit()` is a no-op in IDF, so the entry is already in flash when `nvs_set_*` returns. Five settings routes in the webserver drove Preferences directly and would have bypassed the broker; they go through the store now. Reads are left alone - they run with the cache on and stall nothing.

**Sector erases, and the honest price.** A flash erase is atomic; nothing on the LX6 can shorten a command once issued, and `SPI_FLASH_AUTO_SUSPEND` does not exist on this chip. So the erase unit IS the unit of CAN loss, and the two available units are an order of magnitude apart. Arduino's `UpdateClass` calls `partitionEraseRange()` on 64 KB boundaries whenever a whole block remains, so an OTA is a train of multi-hundred-millisecond stalls. `CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE` takes the sector branch instead - and the win is not only the shorter command, because `CONFIG_SPI_FLASH_YIELD_DURING_ERASE` re-enables the cache BETWEEN sectors, which is where the drain gets to run.

Measured on a LilyGo T-CAN485 (classic ESP32, 4 MB), longest gap between two CAN drains during an OTA upload:

| erase granularity | longest drain gap, three runs |
|---|---|
| 64 KB blocks | 319 / 332 / 330 ms |
| 4 KB sectors | 82.5 / 81.0 / 81.6 ms |

**The upload runs about half as fast** - erasing 64 KB as sixteen sector commands takes ~1.1 s where one block command takes ~0.33 s. A full 1.88 MB OTA still completes and verifies in 40 s, so the trade stands for a battery emulator on a live bus, but it is stated at its real size rather than as "slightly slower".

**And it does not deliver zero loss on its own.** 81-82 ms is one sector erase on this part, and no ESP32 CAN controller has a receive FIFO that deep. The remaining residual belongs to a driver-side RAM ring, which this measurement says must be sized for 85+ ms rather than for the ~50 ms an erase was assumed to cost. That is the follow-on work, not this branch.

**Two corrections that came out of reviewing it, both kept in the history because they change what the numbers mean:**

- *The instrument was measuring the wrong thing.* The first silicon run reported a 1,071,517 µs "cache-off window". That number is real but it is not a window: with sector erases the driver serves one brokered call as a train of erase commands with the cache back ON between them. One call, many windows - and the instrument reported the call. It now measures the gap between two consecutive drains by the CAN owner, which is what the FIFOs actually have to cover and which counts every cause of starvation rather than only the ones the broker knows about.
- *The pre-drain's promise was too broad.* The header said "empty every receive FIFO". Checked per driver, `receive_can()` reads a SOFTWARE queue on all three controller classes; the chips' hardware FIFOs are emptied by an ISR, or by a task an ISR wakes, and those are exactly what a cache-off window stops. The pre-drain is still worth doing - it buys the software queues headroom for the burst that lands when the window ends - but what the hardware FIFOs must survive is the window itself, which is why shortening the window is what moved the measured number.

**Two defects fixed in review, each pinned by a mutation-checked test.** The nesting depth count was a plain `uint32_t` read-modify-written from every task that writes flash, and two of them do so concurrently in the ordinary case - `core_loop` saves the inverter watchdog setting from its 1 s branch on one core while the AsyncTCP task brokers OTA chunks on the other. A lost increment leaves the count stuck above zero and every write for the rest of the boot is treated as nested: no pre-drain, no gap, no measurement, and nothing in the log to say the broker stopped brokering. It is atomic now. Separately, the performance page's per-burst line ended in the LIFETIME drain-timeout count and the storm mirrors were never cleared when a new storm opened, so a fresh burst showed the finished one's worst numbers.

**A known limitation, stated on `run()` rather than left to be discovered:** telling concurrency APART from nesting needs a caller-context hook the platform-free policy does not have, so a write issued by another task while one is in flight is treated as nested - it skips its own pre-drain and is charged to the window already open.

**A method note for anyone repeating the measurement:** the first attempt measured no difference at all, because a `pio run` that does not print "Compile Arduino IDF libs" links the PREVIOUS sdkconfig's libraries. The generated sdkconfig said the option was on while the binary was built without it. Wipe `.pio/build/<env>` and the generated `sdkconfig.<env>`, and check for that line.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**CAN: the whole lane, measured - one branch, three offers, twelve PR candidates inside it**
Branch [`can-lane`](https://github.com/ekholm/Battery-Emulator/tree/can-lane) @ `96a4042f` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-lane) · [diff vs `v12.6.0`](https://github.com/dalathegreat/Battery-Emulator/compare/v12.6.0...ekholm:Battery-Emulator:can-lane) · 10 merges over 124 commits

One branch carries the complete CAN offering, built from twelve reviewed branches in
dependency order; each of those is a PR candidate on its own and is linked below, so a maintainer
can take the whole, one offer, or one fix. The story is one sentence in three parts: a board's CAN
declaration matches its wiring, every failure is per-interface and visible, and the receive path
drains at zero measured loss with the flash cache off. The last part is a table, not a sentence -
each row driven to the ceiling of its link, which is the wire or the sending board (the FD row) -
or, on the S3 native row, something on the receiving board's side of the bus; never the drain:

| interface | chip | offered at the link's ceiling | loss |
|---|---|---|---|
| native TWAI | classic ESP32 | ~3507 f/s | 0.000 % |
| native TWAI | ESP32-S3 | ~3855 f/s (see below) | 0.000 % |
| MCP2515 (baseline drain) | S3 | ~4108 f/s | 0.000 % |
| MCP2517FD, classic mode | S3 | ~4107 f/s | 0.000 % |
| MCP2518FD, FD mode (500 kbit arbitration, 2 Mbit data, BRS) | S3 | ~6373 f/s | 0.000 % |

Loss is derived from the flood's own sequence on the receiving board and cross-checked against the
sender's accepted-send count; receive rings (192 deep) never passed 6 on the classic rows and 12 on
the FD row. The FD row's ceiling is the sender, not the wire: the peer is a classic ESP32 driving an
MCP2518FD over SPI, and the bus arithmetic puts the wire above 8 500 f/s, so the drain was never
offered the wire's full rate - what it was offered, over eleven rates from 200 to 20 000 f/s asked,
it took with no gap, no reorder and no hardware overflow. Before this lane the native path lost
three quarters of the same flood (74.4 % measured, ring overflowing), so the zeros are not a
property of the bench.

**One row moved when the lane was lifted onto `v12.6.0`, and we do not yet know why.** Before the
lift the S3 native row reached the wire's ~4107 f/s; after it, ~3855. Swapping images between the
two boards shows the change follows the RECEIVING board's firmware, not the sender's: the sender
asks for the same number of frames and gets more of them refused. The receiving board also runs a
battery on that same bus, so it transmits there too; whether the lifted firmware sends more, or
causes error frames, is still being measured. Nothing is lost either way - every frame the sender
got onto the wire arrived.

**Through a flash write the table is not the whole story, and the entry says where it stops.**
Measured on this lane before its lift onto `v12.6.0`: with three boards flooded at once while each takes a 20-save settings storm and then an OTA upload
(4 KB sector erase): native TWAI on the classic ESP32 loses NOTHING at any rate to 2 000 f/s, its
ring peaking at 175 of 192; the MCP2518FD path is lossless through the storm at every rate and
through the OTA to 500 f/s, and at 2 000 f/s its ring overflows during the upload; the MCP2515 on
the S3 is the baseline drain and loses frames during an OTA from 100 f/s up. The lane also carries
an IRAM interrupt drain for the MCP2515, meant to close that last gap; it builds for the classic
ESP32 only and has not yet been measured on silicon.

**Built from, in merge order** (each is a PR candidate on its own; indentation is what it sits on):

- [`can-init-per-interface`](https://github.com/ekholm/Battery-Emulator/tree/can-init-per-interface) - the trunk: a failed chip stops at that chip
  - [`mcp2515-mode-and-bitrate`](https://github.com/ekholm/Battery-Emulator/tree/mcp2515-mode-and-bitrate) - verify the mode, refuse an underivable bitrate
  - [`native-can-lifecycle`](https://github.com/ekholm/Battery-Emulator/tree/native-can-lifecycle) - the native flag tracks the peripheral both ways
    - [`can-zero-loss-drain`](https://github.com/ekholm/Battery-Emulator/tree/can-zero-loss-drain) - IRAM-resident drains, FD interrupt back, pin policy
- [`native-can-drain-batch`](https://github.com/ekholm/Battery-Emulator/tree/native-can-drain-batch) - the native path drains a batch, like every other interface
- [`can-absent-interface`](https://github.com/ekholm/Battery-Emulator/tree/can-absent-interface) - boards declare what they route; an absent controller fails safe
- [`absent-can-addon-inert`](https://github.com/ekholm/Battery-Emulator/tree/absent-can-addon-inert) - an add-on the board does not have is not an incoherent map
- [`select-unrepresented-value`](https://github.com/ekholm/Battery-Emulator/tree/select-unrepresented-value) - a settings select shows the stored value it has no option for
- [`replay-unreachable-interface`](https://github.com/ekholm/Battery-Emulator/tree/replay-unreachable-interface) - a replay that cannot reach a wire is refused with the reason
- [`inverter-driver-defects`](https://github.com/ekholm/Battery-Emulator/tree/inverter-driver-defects) - 21-driver protocol suite and the three defects it found
- [`ota-erase-granularity`](https://github.com/ekholm/Battery-Emulator/tree/ota-erase-granularity) - a firmware upload erases in 4 KB sectors, so no single erase parks the cores past a keepalive
- [`can-host-testability`](https://github.com/ekholm/Battery-Emulator/tree/can-host-testability) - `comm_can.cpp` compiled into the host suite. It comes last and sits on the lane rather than on the release, because its tests pin the fixed behaviour of the branches above and do not build without them.

The lane merges ten of these; `can-init-per-interface` and `native-can-lifecycle` arrive underneath
the branches stacked on them.

**1. A board offers the interfaces it routes, and one absent controller does not cost the boot**
(`can-absent-interface`, `absent-can-addon-inert`)
Declaration and wiring disagreed in BOTH directions on six of the eight boards: physically wired
controllers were unselectable, and phantom chips were offered and then failed collaterally. On
`main` a failure returns out of `init_CAN()` where it stands, so every interface ordered after the
failing one is never brought up: one stale MCP2515 selection took a board's FD interfaces down with
it. The declaration is corrected everywhere, can express runtime-probed variants, and an interface
whose controller does not answer fails safe: MEASURED, the board boots in 6.1 s, stays reachable,
and prints exactly what failed - a misconfiguration is repairable from the very page that caused it.
An interface the board does not declare is refused before anything is initialised, with an event
that says so and asks the user to check the setting against the fitted hardware; on `main` that
same event tells the user to recompile the firmware. The same event also accompanies a declared
interface that failed to start, beside the event that says why - the chip's init failure, or the GPIO
event when its pins could not be allocated - and its text names both causes. (Where the failing controller is the last one selected,
nothing is ordered behind it and nothing is lost; the fix's value is the interfaces ordered after
it, and the diagnosis, and that is enough.) The second branch
closes the case the first one reaches from the other side: a configuration naming an add-on the
board does not have cost it every interface declared after that add-on, because the pin allocator
returned the same false for "not fitted" and "already owned" and init treated both as an incoherent
map. Absent is now told apart from conflict and left inert, and the FD group's three blocks are
decided as one plan in a header the host suite runs. MEASURED on a Stark with a stored MCP2515 it
does not have: the CAN-FD interface below it comes up, and the absence is still reported. The
settings-page half, a select that renders the stored value it cannot otherwise show, is already on
the shelf as a standalone fix and is included here.

**2. Every start-up and every replay reports what actually happened, per interface**
(`can-init-per-interface`, `mcp2515-mode-and-bitrate`, `native-can-lifecycle`,
`replay-unreachable-interface`)
Four branches, one defect class - success was reported for things that had not happened, and the
layers above believed it. `init_CAN()` aborted every later interface on any failure while its
return was discarded, so which interfaces survived was decided by initialisation order; failures
become per-interface: event, null pointer, continue - and the native path gains the init-failure
event it never had. A chip that never left configuration mode still reported successful init, and
a bitrate the crystal cannot derive was accepted silently and produced a dead bus; the mode we
asked for is now verified, and an underivable bitrate is refused. A frame handed to CAN_NATIVE
while the peripheral was never brought up crashed the board; the initialization flag now tracks
reality in both directions, and the user is told what actually failed instead of the driver's
internal shorthand. A replay aimed at an interface that cannot transmit was indistinguishable, on
every page, from one that was working: frames counted, activity shown, not one byte off the board;
it is now refused with the reason. Two of these fixes (the transmit guard and the missing-add-on
diagnosis) are already on the shelf as standalone entries and are included here.

**3. Keep the controllers draining while the flash cache is off**
(`can-zero-loss-drain`, `native-can-drain-batch`, `ota-erase-granularity`)
Flash writes disable the instruction cache; for that window, non-IRAM code cannot run, including
interrupt handlers not allocated to survive it - and this firmware writes flash while running. The
receive paths become IRAM-resident: the MCP2515 is drained inside an IRAM interrupt through a
register-level SPI service, the FD drain's interrupt is IRAM-resident (a polled variant was tried
and measured to self-deadlock the vendored library under ordinary load - reboot-looping a board at
200 f/s, task watchdog and `rst:0xc`, reproduced on 2026-09-05 - so the interrupt stays), and the
TWAI interrupt is allocated to survive the flash window instead of being masked through it. On the
lane, drain counters land on the performance page so any board on any wire shows its own loss; they are part of the lane only, not of the member branches. The
second branch removes the ceiling that made the native rows unmeasurable at all: the native path
took one frame per 1 kHz tick while every sibling drained a batch, so an idle board lost 74.4 % of
a saturated 500 kbit bus; it now drains a batch bounded by the driver ring's own depth, and the
same flood measures 0.000 % either side of that one commit. The header table is this branch's
acceptance run. The TWAI interrupt fix is already on the shelf as a standalone entry and is
included here. The third branch closes the one flash window a resident drain cannot bridge: an
erase command parks both cores with the cache off, no interrupt runs, and Arduino's update class
erases the OTA partition in 64 KB blocks that each take about 88 ms on this part, 29 of them per
1.9 MB upload. On stock `main` the receiving end of a paced stream saw 15 intervals over 100 ms
per upload at a 10 ms cadence and 6 at 1 ms, enough to miss a pack keepalive, with nothing lost,
only late. Pacing the upload cannot help, because nothing inserts idle time inside one command.
One build option switches the erase to 4 KB sectors: 0 intervals over 100 ms at either cadence,
worst 32.2 ms, and the upload takes about 64 percent longer, once per update. The same erase option is also part of [`flash-write-interleave`](https://github.com/ekholm/Battery-Emulator/tree/flash-write-interleave) (the entry above), beside a flash-write broker; an A/B of that broker on this lane, before its lift, measured no difference, so the broker is not part of the lane.

*Note: maintained with AI assistance, reviewed before publishing.*

---

**Settings: one descriptor table for every setting, and a boot audit of what is actually on the device**
Branch [`refactor/settings-audit`](https://github.com/ekholm/Battery-Emulator/tree/refactor/settings-audit) @ `3458d623` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:refactor/settings-audit)
A setting's key, NVS type, default and range are spread over up to five hand-synced places and have already drifted. This adds one row per setting (a constexpr X-macro table, validated at compile time, nothing reading it yet) and a boot-time audit that reads the table alongside the existing loads and reports - applies nothing - any stored entry whose NVS type disagrees with its row. That mismatch is what would make a table-driven loader silently replace a user's setting with a default.

<details>
<summary>PR body it would ship with</summary>

**Add a settings descriptor table, with nothing reading it yet**

Every setting's identity is spread across up to five hand-synced places: the NVS key, its
storage type and boot default in comm_nvm.cpp, the render default and range in
settings_html.cpp, the save parsing in webserver.cpp. They have already drifted - #2697
fixed six render-vs-boot default mismatches, and many keys have no range stated
anywhere.

This adds one row per setting stating the key, the storage type, the default and the range
once, and nothing that reads it. The consumers replace their copies one at a time in later
changes; until then this is data, and because the table is constexpr with no consumer the
linker drops it.

SettingKind encodes the on-flash NVS type rather than the in-memory one. NVS entries carry
a type tag and Preferences enforces it - getInt() on a key written by putUInt() fails and
silently returns the default - so a uniform integer kind would reset every existing
device's settings the moment a loader started using the table.

One X-macro list generates both the Sid enum and the rows, following events.h, so a setting
cannot exist in one and not the other. table_valid() runs over the whole table in a constant
expression and is asserted at compile time: a duplicate key, a key past the 15-character NVS
ceiling, an inverted range and a default outside its own range are build failures. Reading
the def union is what keeps kind and default honest - touching the string member of a
non-string row is not a constant expression, so such a row fails to compile. All five of
those defect classes were checked to fail the build, not assumed to.

The tests cover what the compile-time check cannot phrase as readable failures - the NVS key
limit, key and placeholder uniqueness, defaults inside their ranges, unsigned rows staying
inside int32_t - and pin two facts: the only two signed keys on the device are MINPERCENTAGE
and CPUTEMPOFFSET, and SF_SECRET sits on the four password-carrying rows and nowhere else.

A further test scans the loader's own source and fails if a key it reads has no row, so
adding a setting without one breaks the suite and names the key. Keys reached through a
variable stay invisible to it on purpose: that is how the legacy static-IP octets are read,
and they deliberately have no rows.

Ranges describe the stored value, which is not always what the page shows: the SOC window,
the current and voltage limits and the BMS reset duration are all scaled between the two.
Rows whose stored 0 means "never stored" admit 0 in their range.

Every setting the firmware stores has a row, including the six added upstream most recently
(BYDBALEN, BYDBALMIN, BYDNATTERM, CHGSTARQ, INVACCREB, INVWDTMO), and each of their values is
read off upstream's own code rather than chosen here: CHGSTARQ is 0..2 because its handler
clamps anything above 2 back to 0, and it is the one setting taken into use without a reboot;
INVWDTMO's bounds and default are the Modbus inverter's own watchdog limits, and it is not
user-editable, so it has no placeholder; INVACCREB and BYDNATTERM take their defaults from
their getBool() calls.

**Audit the stored settings against the table at boot**

The table added in the previous commit is data nobody reads. Before any loader starts
reading it instead of its hand-written lines, there is one thing worth knowing that no test
can establish: whether the table describes what is actually on the devices.

NVS tags every entry with the type it was written as, and Preferences enforces that tag on
read - a getter that disagrees returns the caller's default rather than the stored value. A
row whose kind does not match its tag would therefore make a generic loader replace a user's
setting with a default, and a generic save would write that default back. That is data loss
with no symptom, on settings including the charge and discharge limits and the SOC window,
and it cannot be ruled out by reading the code or by a host test: it depends on what past
firmware wrote to a particular device.

So the table is read here in parallel with the existing loads, at the end of the boot pass,
and every disagreement is reported. Nothing is applied. The values the hand-written loads
produced stand exactly as before.

A key that was never stored is skipped rather than compared - both paths would produce their
own default, which says nothing about the flash. On a mismatch the log names the key, the tag
it is stored under, the tag the table expects, and both readings of it, and one info-level
event carries the row index so a device reports it without a serial console. The result is
reported even when nothing disagrees: a clean device and a device with nothing stored would
otherwise look identical, and the second one proves nothing.

Cost is one existence check and one type read per row during boot, and nothing afterwards.
No polling, no second pass, and no allocation unless something actually disagrees.

The emulated Preferences in test/emul gains the type enum and getType alongside, so the stub
still mirrors the API this file now uses.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Settings: typed accessors generated from the table, and a guard that stops new code addressing settings by key**
Branch [`refactor/settings-accessors`](https://github.com/ekholm/Battery-Emulator/tree/refactor/settings-accessors) @ `60c5604d` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:refactor/settings-accessors) · stacked on `refactor/settings-audit` (includes it)
`setting_get<Sid::X>()` / `setting_save<Sid::X>()` take the key, NVS type and default from the row, so a mistyped access does not compile. A ratchet stops new call sites naming keys by hand; the webserver's literal-key access and the BYD calibration routes (which opened their own NVS handles beside the store) are migrated. Includes the settings-store coverage and the NVS type-tag emulation it needs.

<details>
<summary>PR body it would ship with</summary>

**Cover the settings store, make the emulation real, and stop a read-only store reporting changes**

Every setting is read and written through BatteryEmulatorSettingsStore, and
none of it was tested - the Preferences emulation discarded every write and
returned zero from every read, so a round-trip was not observable at all.

The emulation now keeps values in memory per namespace, surviving a store
being closed and reopened the way real NVS survives a reboot, and models two
constraints that otherwise only appear on hardware: a read-only store cannot
write, and keys longer than the NVS limit of 15 characters are rejected. The
longest keys in the firmware are exactly at that limit (TARGETDISCHVOLT among
them), so a new one that exceeded it would silently never persist.

Writing the tests turned up one defect: the save and remove methods set
settingsUpdated even on a read-only store, where the underlying write is
refused. Nothing reaches that today - the only read-only store is the one the
settings page opens to render values, and it never saves - but it would have
told the user to reboot to apply a change that never happened. The store now
refuses the write outright when it is read-only.

The cases: round-trip per type, persistence across reopening, defaults,
removal and clearing, read-only stores, the key-length limit, and the
settingsUpdated flag - including the first save of a zero, false or empty
value, which the isKey() guards exist to stop being mistaken for a no-op.

**Make the emulation's typed reads honor the NVS type tag**

Real NVS tags every entry with the type it was written as and a typed
getter on a mismatched key returns the caller's default, not the stored
bits. The emulation returned whichever field the getter named, which for
a cross-typed read is a zero that real hardware would never produce. The
shadow audit and the accessor layer both exist because of exactly this
tag behaviour, so the emulation has to model it for their tests to mean
anything.

**Add typed setting accessors generated from the table**

setting_get<Sid::X>() and setting_save<Sid::X>() take the key, the
on-flash NVS type and the default from the setting's own row, so a call
site names only the id: the C++ type it gets back and the store call
used underneath both follow from the row's kind, and a mistyped access
does not compile. This is the accessor half of the follow-up roadmap
stated in the table PR's thread; prohibiting direct key access outside
this layer comes separately.

Nothing in the firmware calls the accessors yet - settings_table.cpp
includes the header so the device toolchain keeps compiling it. The
accessor tests walk every row on the Preferences emulation: after a save
through its accessor, every key carries exactly the NVS type
tag its row declares - the executable form of the boot audit's mismatch
condition, proven over the whole table.

**Stop new call sites reaching a setting by key**

A call site that names a key repeats three facts the table already states - the
spelling, the on-flash NVS type and the default - and NVS punishes the middle one
silently: a typed read of a key stored under a different tag returns the default
instead of the stored value, so a setting is lost with no symptom. That is what the
boot audit exists to catch after the fact. The accessors remove the chance to make the
mistake, because a call site names only the id and the row supplies the rest.

This holds the list of places that still address keys by hand and refuses to let it
grow. Dropping BELOW an entry fails too, so migrating a file has to lower its number
rather than quietly bank the progress - the same shape as the statics ratchet and the
extended-data census.

Two files are exempt permanently rather than listed. The store IS the key-addressed
API, and the loader's whole job is to walk rows and address keys - it does that through
the table, not by hand.

The first batch is migrated as the demonstration: MQTT's two saves, and the six reads
in the webserver whose key is a literal. Their defaults were already what their rows
say - "admin" for the HTTP user, empty for the passwords, None for the battery type -
which is the argument for the accessor in one line: the default stops being a thing
each call site remembers.

What stays behind there is the generic save loops, which take the key from a runtime
variable (`uintSettingNames` and kin). Those cannot use a compile-time id at all; they
go when the loop itself becomes table-driven, and their count is what is left on the
entry.

The scan is substring-based rather than a regex pass over the tree: the regex version
cost several times the rest of the suite, and a check that slow is one that
gets switched off.

All three directions were checked by injecting them - an unlisted file reaching a key,
a listed file growing, and a listed file finishing without lowering its entry.

**Migrate the webserver's remaining literal-key settings access**

The previous commit said what was left in webserver.cpp was "the generic loops". That
was wrong, and only checking made it visible: most of what was left still named a key
as a literal and could move today. Nearly all of them are saves, which carry no default
at all, so there was nothing to weigh - they were left behind for no reason beyond the
first batch being called a demonstration and stopping there.

Moved, and what remains is genuinely blocked rather than merely unvisited:

  - the generic save loops, whose key is a runtime variable (`uintSettingNames` and
    kin) and which cannot take a compile-time id until the loop is table-driven
  - raw Preferences handles in the BYD routes
  - the calls on those handles, which bypass the settings store entirely

The raw handles are worth a second look later: they open the NVS namespace directly
beside a store that is already open, which is a different problem from naming a key and
is not something an accessor fixes.

The two settings upstream added most recently that the settings page reads, CHGSTARQ and
INVACCREB, go through their accessors too.

**Give the emulation's NVS tag enforcement test teeth**

Review of the accessor layer. Reverting the emulation commit that
makes typed reads honor the tag passed the entire suite: the one probe of
that behaviour used a bool row whose default is false, indistinguishable
from the zero field a tag-ignoring read returns. Cross-typed probes now
use rows whose defaults are NOT the cross-typed zero, at both the store
and the accessor level, and the emulation's Value fields are
zero-initialized so a regression fails deterministically instead of
reading whatever the stack held. A second new case saves through the
accessor and reads back through the raw typed getter, pinning the row's
literal key spelling from outside the abstraction - the existing
round-trip test was accessor-in accessor-out, self-consistent even under
a row-lookup skew.

Both mutations re-verified caught: stripping the emulation's tag guards
fails the two new cross-type tests; flipping the BoolU8 save dispatch to
saveUInt fails the whole-table tag walk by row name, as designed.

**Scan the remove and exists verbs in the direct-access guard**

Review of the guard. Deleting or probing a key by name repeats the
key's spelling just like reading one, but the scan's verb list stopped at
get/save/put, so the first future removeKey or settingExists call site
would have passed unnoticed. Added with zero grandfathered uses - the
census found none outside the exempt store - and verified caught by
injection. The receiver-rename evasion (a store instance not named
settings or prefs) stays open by design: this is a tripwire against the
copied idiom, not a boundary, and the cost of closing it is the regex
pass the file's own comment rejects.

**Take the BYD routes off their own NVS handles**

The five BYD calibration routes opened Preferences directly, beside a
store the rest of the webserver already uses. They were thought
blocked; review showed they are not - the keys are table rows with
matching kinds in the same namespace, all compile-time - so they are
migrated here with the accessor layer as it stands.

Beyond the key spelling, the raw handles bypassed what the store does:
no read-only guard, and no skip-identical check, so every hit on one of
these routes issued a write whether or not the value had changed. Through
setting_save<Sid::X> the type comes from the row - a mistyped save is
now a compile error rather than a mistagged NVS entry - and the tags
written are unchanged (BoolU8 -> putBool, U32 -> putUInt), so an
already-provisioned device reads back exactly what it did before.

The direct-access guard's webserver.cpp entry drops by the number it
printed on its own when the sites went away. What is left are the
generic save loops, whose key is a runtime variable; they stay until the
loops themselves are table-driven. webserver.cpp no longer includes
Preferences.h - the store's header owns that.

**Pin the BYD routes' on-flash contract, both directions**

The migration was first claimed safe for a provisioned device on
construction alone, on the grounds that no webserver harness exists. The harness is not what the claim needs: the risk is on flash,
not in the routing. A device provisioned by the old firmware has to read
back through the accessor exactly what the old raw putBool/putUInt
wrote, and what the new route writes has to be visible to a reader still
using the old typed call - both testable on the Preferences emulation,
which enforces NVS tags.

Stored values are deliberately not the row defaults, since a silently
defaulting read returns the default and would pass a test that used one.
The second test pins what the raw handles were giving up: they called
putX unconditionally, so a repeated route hit made an NVS write call -
and reported a change - every time, even though NVS itself leaves an
identical value alone.

Mutation-checked both ways - flipping BYDAUTOCALEN's row from BoolU8 to
U32 fails the contract test in both directions, and removing the store's
skip-identical branch fails the second.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Drivers: protected base destructors, a TYPE on every driver class, and Tesla variants as their own classes**
Branch [`refactor/pr1-prep`](https://github.com/ekholm/Battery-Emulator/tree/refactor/pr1-prep) @ `e535409b` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:refactor/pr1-prep)
Groundwork for a compile-time descriptor table, with no behaviour change: deleting a driver through its base becomes a compile error (it was undefined behaviour) at no flash cost; every battery, inverter, charger and shunt class states its enum `TYPE` beside its `Name`; the Tesla Model 3/Y and S/X variants become subclasses instead of one class reading a global to find out what it is.

<details>
<summary>PR body it would ship with</summary>

Preparation for a descriptor-table refactor. No behaviour change anywhere:
everything is additive, a mechanical rename, or a compile-time guarantee.

1. Protected non-virtual destructors on the polymorphic bases (Battery,
   InverterProtocol, Transmitter, CanReceiver). Deleting through a base
   pointer is undefined behaviour without a virtual destructor; making it a
   compile error instead gives the same correctness guarantee at no flash
   cost - no destructor pairs or thunks in any vtable. Instances are used
   polymorphically but never deleted through the base, in production as in
   the tests: the Tesla variant test deletes through the concrete factory
   types, and the parameterized fixtures abandon instances instead of
   deleting them (the old deletes also left dangling register_transmitter
   entries; abandonment mirrors device lifetime). The one test that did
   delete through the base globals (the BYD Atto 3 balance-API case) now
   deletes through the concrete type, which is what the protected
   destructor is there to enforce.

2. static constexpr TYPE on every battery, inverter, charger and shunt
   class, alongside the existing Name statics - groundwork for replacing the
   hand-synced factory/name switches with a compile-time descriptor table.

3. Tesla variant split: TeslaModel3YBattery / TeslaModelSXBattery
   subclasses of TeslaBattery. Previously two enum values mapped to one
   class that read user_selected_battery_type deep inside the driver to
   resolve its own variant. The variant-dependent tail of setup() (pack
   design limits, reported protocol name) is now a pure virtual
   apply_variant_config() implemented per subclass and called at the same
   point in the sequence, so the variant is expressed by the constructed
   type alone. The Name3Y / NameSX special case is gone (each subclass
   carries its own Name and TYPE), the factory constructs the right
   subclass, and mqtt.cpp's Tesla type-sniffing becomes a capability query
   on the instance (supports_tesla_dcdc_metrics).

4. Tesla contactor alias cleanup: battery_contactor was a historical alias
   of BMS_contactorState (its own 0x20A parse has been commented out; it was
   assigned verbatim from the 0x212 parse). Deleted, with its readers
   switched to BMS_contactorState directly. Value 4 meant closed and 0 meant
   SNA under both the old and the current enum, so this is a pure rename.

Adds tests constructing both Tesla variants with user_selected_battery_type
deliberately set to None, asserting the variant-dependent pack limits and
reported protocol name resolve from the constructed type alone.

Note: drafted with AI assistance, reviewed by me.

</details>
