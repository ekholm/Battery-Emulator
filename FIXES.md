# Fixes

*Prepared, not filed. Offered pull-style per maintainer preference: nothing here is a pull
request. Point at whichever entry is useful, ignore the rest.*

*Each entry links the branch, the pinned commit and the full diff against upstream `main`, and
carries the PR body it would ship with. An entry becomes a PR only on request.*

*Ordered so that changes applying directly to current `main` come first; entries further down
depend on other work or have not been placed yet. Every entry is independent unless it says so.*

Defect repairs only - small, self-contained, each ready to merge. See also [FEATURES.md](FEATURES.md)
for larger changes and conversions, and [FINDINGS.md](FINDINGS.md) for things we have found but
cannot fix ourselves.

**Kept current.** Every entry is re-checked against upstream `main`, and one whose defect `main` has
since fixed is removed rather than left to waste your time. Last checked at `a2851c23`, 2026-09-02.

*Every branch is rebased onto upstream `main` at `a2851c23` (2026-09-02), rebuilt, and the host
test suite run on the result - all green. The pinned commits below are the rebased, tested ones.*


---

**CAN replay: a malformed log line writes past the end of a global frame buffer**
Branch [`can-replay-dlc-bound`](https://github.com/ekholm/Battery-Emulator/tree/can-replay-dlc-bound) @ `6b1debf6` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-replay-dlc-bound)
`canReplayTask` parses an uploaded log line's DLC with `dlc.toInt()` into a `uint8_t` and then copies as many space-separated tokens as the line supplies, bounded only by that DLC - which accepts 0..255 while the frame's data array holds 64. A line declaring `[200]` followed by 200 tokens writes 136 bytes past the end of a file-scope global, corrupting whatever follows it in .bss. The content is fully user-supplied via the HTTP upload handler.

<details>
<summary>PR body it would ship with</summary>

The replay parser bounds its copy loop with the parsed DLC alone:

```cpp
currentFrame.DLC = dlc.toInt();
...
while (token != NULL && byteIndex < currentFrame.DLC) {
  currentFrame.data.u8[byteIndex++] = strtol(token, NULL, 16);
```

`DLC` is a `uint8_t` accepting 0..255; `data.u8` holds 64. The frame is a file-scope global, so an oversized line corrupts whatever follows it in .bss rather than smashing a return address. The log content comes straight from the HTTP upload handler.

This refuses the line rather than truncating it - a frame that long is not a frame this build can send, and truncating would replay a frame the capture never contained. The refusal checks the PARSED length, not the value after narrowing, so a line declaring `[300]` (which narrows to 44) is refused too; a test pins exactly that distinction. A replay aimed at an interface that cannot reach any wire is refused for the same reason: it would report success while transmitting nothing.

Calibration, stated plainly: real CAN and CAN-FD logs cannot express a DLC above 64, so triggering this needs webserver access and a crafted or corrupted file. Memory safety, not remote attack surface.

Host tests cover the bound, the parsed-vs-narrowed distinction, and the unreachable-interface refusal.
</details>

---

**CAN replay: a log line with nothing after the DLC is a use-after-free**
Branch [`can-replay-null-data`](https://github.com/ekholm/Battery-Emulator/tree/can-replay-null-data) @ `b9b77063` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-replay-null-data) · stacked on `can-replay-dlc-bound` (includes it)
When a log line ends at the DLC, `strtok(NULL, " ")` is called with no live tokenisation state - the prior `strtok` chain ran on a String's internal buffer that has gone out of scope by then. The parse now goes through a host-tested helper that owns its buffer for the whole parse, and the test reproduces the use-after-free rather than asserting around it.

<details>
<summary>PR body it would ship with</summary>

`canReplayTask` tokenises a log line's data bytes with a `strtok` chain that starts on the internal buffer of a `String` produced earlier in the loop. When a line carries a DLC but no data bytes, the loop's next iteration calls `strtok(NULL, " ")` - continuing a tokenisation whose backing buffer has already gone out of scope. Use-after-free on user-supplied input.

The data-byte parse moves into a small helper that owns its buffer for the whole parse and is host-testable in isolation. The accompanying test reproduces the use-after-free shape (it fails against the old code for the defect's actual reason, not a proxy), and the helper is exercised by the DLC-bound tests as well.

Stacked on `can-replay-dlc-bound`: this branch contains that fix, and the two together make the replay parser refuse malformed lines and parse well-formed ones without touching freed memory.
</details>

---

**Triple battery: the predicate and the switch agree again, and the invariant is now a test**
Branch [`battery-instance-support-parity`](https://github.com/ekholm/Battery-Emulator/tree/battery-instance-support-parity) @ `48fe1c8a` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:battery-instance-support-parity)
`battery_supports_triple()` listed five types while the battery3 construction switch had six cases - `CmpSmartCar`'s case was unreachable, because the guard rejected the type before the switch could reach it. The resolution declares the CMP Smart Car triple-capable rather than deleting the dead case, and - the durable half - adds the test the file's own comment has always asked for: both label sets extracted and asserted equal, fallthrough-aware, so the "must match the switch in setup_battery() below" invariant fails a build instead of relying on a comment.

---

**Tesla: two advanced-page fields that report the wrong thing**
Branch [`tesla-page-meaning`](https://github.com/ekholm/Battery-Emulator/tree/tesla-page-meaning) @ `9ef0977d` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:tesla-page-meaning) · includes the lookup-bounds fix beneath it
`HVP_currentSenseMia` was parsed with a two-bit mask on a one-bit field, so it read `Yes` whenever the neighbouring ref-voltage-mismatch bit was set alone - every sibling MIA field in the block masks a single bit correctly; this one was alone in being wrong. The review sweep found a second field of the same class the original fix had missed. Four PCS retry counters (3- and 4-bit) were rendered through a two-entry False/True table, so one retry read `True` and higher counts had no meaning; they now render as the numbers their labels ("Rty Cnt") always promised. Beneath it, the contained bounds fix: every lookup table on the page is bounded, an out-of-range value is named (`UNKNOWN(n)`) instead of dereferenced, and the emul String is null-guarded where Arduino's guards.

---

**Tesla: a second battery stops corrupting the first battery's page**
Branch [`tesla-instance-parity`](https://github.com/ekholm/Battery-Emulator/tree/tesla-instance-parity) @ `c72c916b` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:tesla-instance-parity)
`TESLA-BATTERY.cpp` wrote the shared `datalayer_extended.tesla` struct from every instance - 483 sites, both constructors - so a double-Tesla setup interleaved two packs into one advanced page. Each instance now carries its own extended-struct pointer, set at construction and null for the second battery: the pattern ECMP and Renault Zoe Gen2 already use. The hoisted UDS part-number trigger is covered, and both ends of the guarded extended block are pinned by test.

---

**SOLAX: the contactor-close permission no longer outlives the inverter's open request**
Branch [`solax-contactor-permission-uplift`](https://github.com/ekholm/Battery-Emulator/tree/solax-contactor-permission-uplift) @ `cc21971f` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:solax-contactor-permission-uplift)
When the inverter commanded the contactor open, the state machine reset but `inverter_allows_contactor_closing` stayed true until the next received frame. The revocation now happens in the open-command branch itself. The exposure, stated precisely: the flag was never unbounded - a 2-second silence timeout already clears it - so the window was the inverter's own next transmission or about 2-3 s, whichever came first. The tests pin both the revocation and the timeout backstop, including its AlwaysClosed gate, so neither safety layer can regress silently.

---

**BYD-CAN: the brand filter tested one byte and stored another**
Branch [`byd-brand-filter`](https://github.com/ekholm/Battery-Emulator/tree/byd-brand-filter) @ `02fe696a` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:byd-brand-filter)
The inverter-name filter had two independent defects that composed into "never correct on any input": both comparisons were `>` (so the printable range it was written to accept was exactly what it rejected), and the guard tested `u8[i]` while the body stored `u8[i + 1]`. Both fixed, and the review added the half a fix alone would have missed: a rejected byte clears its slot rather than leaving the previous scan's character behind. This deliberately does not decide the byte-0 mux question - see [FINDINGS.md](FINDINGS.md) - it makes the current reading self-consistent.

---

**Hostname: stop copying it on every read**
Branch [`hostname-no-copy`](https://github.com/ekholm/Battery-Emulator/tree/hostname-no-copy) @ `19dab10f` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:hostname-no-copy)
`custom_hostname` was an `std::string` in a tree whose consumers speak Arduino `String`, so every read paid a conversion copy. It becomes a `String`, both accessors return `const String&`, and all five call sites bind for free - `MDNS.begin()` and `html_escape()` take the reference directly. The quieter win: the file is now host-testable at all, and ships with its tests.

---

**AsyncTCP: the 4 KB stack request is real - proven from the emitted code, and now made explicit**
Branch [`asynctcp-stack-claim`](https://github.com/ekholm/Battery-Emulator/tree/asynctcp-stack-claim) @ `8be78bef` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:asynctcp-stack-claim)
The suspicion was that `CONFIG_ASYNC_TCP_STACK_SIZE 4096` never reached the library, leaving its 16 KB default to win. Refuted twice over: `AsyncTCP.h`'s first include is `system_settings.h` itself, and the emitted object code builds the task-creation argument as 4096 (the counterfactual was also built and read). The change makes the ask explicit - `BE_ASYNC_TCP_STACK_SIZE`, mapped onto the library's config name - and adds a text-reading regression test that reddens if either define disappears. It deliberately does not pin which include supplies the value, since two paths do today.

---

**GPIO events: each failure names its own component**
Branch [`gpio-event-names`](https://github.com/ekholm/Battery-Emulator/tree/gpio-event-names) @ `41996dbb` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:gpio-event-names)
`alloc_pins()` wrote one shared name pair for both GPIO events, and the message string is read back live on every publish - events page, MQTT, ESP-NOW. So a missing-pin failure after a pin conflict re-pointed the conflict's message at the wrong component, and vice versa. Each event now owns its names; the shared pair is gone rather than left behind.

---

**Pin roles: illegal combinations refused at selection time**
Branch [`pin-role-exclusions`](https://github.com/ekholm/Battery-Emulator/tree/pin-role-exclusions) @ `d782dcfe` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:pin-role-exclusions)
A legal-combination table (`pin_exclusions.{h,cpp}`) makes enforced exclusions data entries, with the known-legal shared-pin groups documented beside them with their rationale - so the next exclusion is an entry, not an investigation. Enforcement runs at `/saveSettings`: the would-be pin assignment is computed and an excluded pair is refused before it is stored, instead of discovered at boot. The board knowledge in the table is hand-maintained today; if board capabilities ever become declarative, this table is the natural first consumer.

---

**Safety events that could never fire**
Branch [`driver-dead-safety-events`](https://github.com/ekholm/Battery-Emulator/tree/driver-dead-safety-events) @ `24687344` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-dead-safety-events)
Two drivers raised safety events on conditions that could not occur: the Kia/Hyundai HYBRID's interlock decode had a cast-precedence error, so `EVENT_HVIL_FAILURE` never fired (now fires and clears on `0x5AE`), and CHARGEBYTE's error ladder was ordered so an error while charging never reported `BMS_FAULT` (reordered). One decision stated openly: the E-GMP water-sensor check was dead - the member is initialised to 164 ("no water") and no E-GMP RX path ever writes it, so the event could never fire and the page rendered a constant. The sibling KIA-64 driver decodes the same sensor for real (`u8[3]` of its poll response, 164 = dry), so this was a copied pattern that never got its decode wired. Removed rather than guessed at; one E-GMP trace naming the byte restores it with the KIA-64 decode as the template.

---

**Decode arithmetic: five values fixed, three pinned**
Branch [`driver-decode-arithmetic`](https://github.com/ekholm/Battery-Emulator/tree/driver-decode-arithmetic) @ `4af083c4` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-decode-arithmetic)
Range Rover PHEV's 24-bit current read against the driver's own declared range; TESLA-LEGACY wrapped subzero temperatures (the decode's own range is -40..+87.5 C); IMIEV had swapped channels, mV rounding loss, and - found during testing - uninitialised 88-entry instance arrays publishing heap reads until every sensor reports; RELION-LV's minimum temperature was decoded but never wired. Three further suspicions are pinned as correct-as-is by characterization tests with the evidence named, so the next reader does not re-litigate them.

---

**Family consistency: four fixes where siblings already agree**
Branch [`driver-family-consistency`](https://github.com/ekholm/Battery-Emulator/tree/driver-family-consistency) @ `1e3e94f3` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-family-consistency)
FERROAMP now honours user voltage limits like PYLON and SOLXPOW already do (the siblings' 2.0 V offset deliberately not imported); GROWATT-WIT's capacity guard goes `> 0` to `> 10`, ending a 50,000 dAh fiction from a 0.5 V startup reading; MG-5's `MG5_USE_FULL_CAPACITY` branch - defined nowhere - is deleted; swapped charge/discharge byte labels corrected, values unchanged.

---

**Uninitialised driver arrays: the four that are live**
Branch [`driver-uninit-sweep`](https://github.com/ekholm/Battery-Emulator/tree/driver-uninit-sweep) @ `af911d31` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-uninit-sweep) · includes the memory-safety fixes beneath it
A sweep sorted ten suspect arrays by liveness: four are whole-array memcpys into the datalayer reachable before frames fill them - BOLT-AMPERA, HYUNDAI-IONIQ-28, KIA-HYUNDAI-64 (whose `<300` filter passes high garbage), SANTA-FE-PHEV. All four get `= {0}` plus a poisoned default-init test through their own publish path; the six that are written-before-read are commented at the declaration instead of churned. Beneath it, the memory-safety commits it includes: an ORION out-of-range cell id is rejected rather than clamped (a corrupted id must neither overwrite a real cell nor inflate the detected-cell count), and explicit zero-init where a user-provided constructor defeats value-initialisation.

---

**Shunt: three values that corrupt instead of going missing**
Branch [`driver-signedness-clamps`](https://github.com/ekholm/Battery-Emulator/tree/driver-signedness-clamps) @ `42e96464` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-signedness-clamps) · stacked under `sbox-average-divisor`
Three driver defects with the same shape: a signedness or width error that turns a real measurement into a plausible wrong number. The main one: `datalayer.shunt.measured_amperage_dA` was `uint16_t`, so every discharge current wrapped - a -50 A discharge read as ~65,486 dA. Now `int16_t`, matching `battery.status.current_dA`, the same quantity and unit already signed in-tree. The audit behind it found the field has two writers and zero in-tree readers, which is what makes the root fix safe to take first. Also: BMW-SBOX's rolling-average members go signed (`avg_mA_array`, `avg_sum` - the division then signs itself), and a review commit zero-initialises them and pins that.

---

**BMW-SBOX: the "1 second average" divides by 10 before 10 samples exist**
Branch [`sbox-average-divisor`](https://github.com/ekholm/Battery-Emulator/tree/sbox-average-divisor) @ `450b6dc7` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sbox-average-divisor) · includes the entry above
`BMW-SBOX` fills one average slot per 100 ms and unconditionally publishes `avg_sum / 10`, so for the first second - and after any gap in `0x200` frames - the average reads a tenth to nine tenths of the true current. Live, not theoretical: Kostal transmits `measured_avg1S_amperage_mA` to the inverter whenever an S-BOX is configured. The divisor becomes the count of samples actually taken, capped at the window. The judgement is stated rather than hidden: publish the average over the samples that exist, because the field carries no validity flag - "publish nothing" means the consumer keeps reading the initial 0 A, which is the same defect class in a quieter coat. An average over real samples converges inside the second.

---

**Kostal: a silent S-BOX must stop deciding the current the inverter is told**
Branch [`shunt-staleness-gate`](https://github.com/ekholm/Battery-Emulator/tree/shunt-staleness-gate) @ `dd19a170` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:shunt-staleness-gate)
`datalayer.shunt.available` is cleared 1000 ms after the last S-BOX frame - and nothing read it. Kostal kept transmitting the last shunt current forever after the shunt went silent. The gate makes Kostal check, and the interesting half is the fallback: when the S-BOX is stale, the inverter gets `battery.status.reported_current_dA` - not a value invented for an error path, but exactly what the same function's `else` branch already sends into the same two byte offsets for every installation without an S-BOX. It is the mapping the protocol already uses when nothing is measuring at the shunt, which is precisely the condition; the shunt reclaims the fields the moment frames resume. `0.0 A` was the alternative and is worse: equally untrue, and it reads as healthy idle.

---

**Native CAN: a transmit to an interface that never started is a silent success**
Branch [`native-can-transmit-guard`](https://github.com/ekholm/Battery-Emulator/tree/native-can-transmit-guard) @ `d455bcb7` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:native-can-transmit-guard)
The native TWAI interface is the only one whose init failure raises no event - the MCP2515 and CAN-FD paths both do - and a transmit to it after a failed or absent init simply disappears. On boards that log nothing unless USB logging is enabled, that is a dead peripheral presenting as a working one. This refuses the transmit and raises a new `EVENT_CAN_NATIVE_NOT_INITIALIZED`, APPENDED at the end of the event enum: event ordinals go out on the wire (ESP-NOW publishes the enum value as a u16), so a mid-enum insertion would renumber every event after it for any peer on a different build. A review commit closes the second path to the dead peripheral, and a test pins the enum layout so the next event cannot un-append it.

---

**CAN: a missing add-on chip is reported as a full buffer**
Branch [`uninitialized-interface-diagnosis`](https://github.com/ekholm/Battery-Emulator/tree/uninitialized-interface-diagnosis) @ `f21c59cb` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:uninitialized-interface-diagnosis) · pairs with the entry above
When an SPI add-on CAN chip is absent or failed to init, transmits to it surface as `CAN_BUFFER_FULL` - a message that sends the reader towards traffic load when the truth is "this chip never existed". Diagnosed on the bench, where a board with no 2515 populated produced exactly that. Three per-interface not-initialized events replace the misdiagnosis, appended to the enum for the same wire-ordinal reason as the entry above; a review commit tightens the replacement message so it does not promise an error state that need not exist.

---

**NeoPixel: `pin` is read before it is ever written - a silent boot loop on the boards where the leftover byte matters**
Branch [`neopixel-uninitialised-pin`](https://github.com/ekholm/Battery-Emulator/tree/neopixel-uninitialised-pin) @ `e80c5b7d` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:neopixel-uninitialised-pin)
`Adafruit_NeoPixel::setPin()` reads `pin` before it writes it, but `pin` has no initial value and no constructor gives it one, so the heap-allocated LED hands leftover bytes to `pinMode()` once per boot. Usually just an `Invalid IO` line; on one LilyGo T-2CAN FD the leftover byte was the SPI flash hold line and the board sat in a silent watchdog reset loop. One initialiser, +108/-2 with two host tests.

<details>
<summary>PR body it would ship with</summary>

`Adafruit_NeoPixel::setPin()` reads `pin` before it writes it, to stop driving a pad it was previously using:

```cpp
void Adafruit_NeoPixel::setPin(int16_t p) {
  if (pin >= 0)
    pinMode(pin, INPUT);
  pin = p;
  ...
```

Upstream Adafruit guards that read with a `begun` flag initialised to `false`. In this repo ce7547f7 ("Optimize Neopixel library for maximum performance") dropped the flag and kept the bare `if (pin >= 0)` - but `pin` has no initial value and no constructor gives it one. A heap-allocated LED object therefore starts life with whatever bytes were in the block and hands them straight to `pinMode()`.

The object is heap-allocated - `led_handler.cpp` does `led = new LED(...)`, and `LED` holds the `Adafruit_NeoPixel` by value - so this happens once per boot, in `led_init()`, with whatever the allocator hands back.

**Most of the time it is only noise.** An

```
[E][esp32-hal-gpio.c:118] __pinMode(): Invalid IO 232 selected
```

line around LED init, whose number moves when something unrelated changes (a different battery selection - anything that changes what was in the heap before `led_init()`), is this bug: the leftover byte was simply out of range, so `pinMode()` rejected it and nothing happened. The number itself is leftover garbage, so it differs from board to board and build to build - a different number than 232 is still this defect. We dismissed exactly that line as unrelated noise three times before it turned out to be the fault announcing itself on every boot.

**It is not always noise.** On a LilyGo T-2CAN FD the leftover byte was 27, which on an ESP32-S3 is `MSPI_IOMUX_PIN_NUM_HD` - the SPI flash hold line. Reconfiguring it as an input cut the flash bus in the middle of `setup()`: the next instruction fetch faulted, the panic handler needed the flash it no longer had, and the board sat in a double exception until the interrupt watchdog reset it. From outside that is a silent `TG1WDT_SYS_RST` loop, once every 1.36 s, with no log line and no coredump. Whether a given board dies or just logs depends on what was previously in that heap block, so it reproduces on one board and not on its identical neighbour - one of ours boots the same image fine.

The fix is the initialiser that the dropped guard used to stand in for.

Two host tests pin both halves, so the bug cannot come back and cannot be "fixed" the wrong way: construction must not configure a pad it was never given (the object's storage is poisoned with 27, the value recovered from the failing board, so the test reproduces the device condition deterministically rather than hoping the allocator hands back dirt), and a later `setPin()` must still release the previous pad - which is why deleting the read is not the fix.

Verified on hardware: the board that would not boot boots, and the `Invalid IO` line is gone.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Contactors: a faulted system must not arm the closing ladder at boot**
Branch [`contactor-fault-boot-race`](https://github.com/ekholm/Battery-Emulator/tree/contactor-fault-boot-race) @ `b6ef575d` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:contactor-fault-boot-race)
`handle_contactors()` latches a fault by counting CALLS but opens its startup gate on `millis()`, so the two agree only at an exactly 10 ms loop. Slower than that, a faulted system energises the negative contactor for the difference - 990 ms at 11 ms, 10 s at 20 ms. One term fixes it. Not verified on hardware.

<details>
<summary>PR body it would ship with</summary>

`handle_contactors()` decides two things with two different clocks, and they only agree at one loop period.

The fault latch counts **calls**: `timeSpentInFaultedMode > MAX_ALLOWED_FAULT_TICKS`, whose comment reads `1000 = 10 seconds` - true only if the function is called exactly every 10 ms. The startup inhibit below it reads **`millis()`**. So on any board whose loop is slower than 10 ms, the fault latch fires *later in real time* than the startup gate opens, and the `DISCONNECTED -> START_PRECHARGE` transition sits **above** that gate with no system-status term. The ladder is therefore already armed when the gate opens, and a faulted system energises the negative contactor for the difference:

| loop period | negative contactor energised |
|---|---|
| 10 ms | 0 (the assumed case) |
| 11 ms | 990 ms |
| 12 ms | 1,992 ms |
| 15 ms | 4,995 ms |
| 20 ms | 10,000 ms |

The fix is one term: require the system to be `ACTIVE` to leave `DISCONNECTED`.

```cpp
if (datalayer.system.status.system_status == ACTIVE &&
    datalayer.system.status.inverter_allows_contactor_closing &&
    !datalayer.system.info.equipment_stop_active) {
```

This is a state gate rather than a retiming, deliberately: making the two clocks agree would leave the transition trusting a call-count to mean a duration, which is the actual defect. It also aligns this gate with `precharge_conditions_ok()`, which has required `system_status == ACTIVE` all along - the two should never have disagreed.

**The risk that matters here is not the bug, it is the fix.** Requiring `ACTIVE` means any path reaching a different status now refuses to arm, so the question is whether a legitimate startup ever transits one. `system_status` has exactly five writers in the tree and all five were checked. It defaults to `ACTIVE`, so a healthy boot is unaffected. `update_bms_status()` derives it from the highest active event level: `ACTIVE` for INFO/WARNING/DEBUG and under the forced-charging-recovery override, `FAULT` for ERROR, `UPDATING` for an update in progress. The only other writer sets `STANDBY` while BMW i3 balancing is executing. Refusing to arm during an update is intended. Refusing during balancing agrees with that block's own code, which already forces `contactors_engaged = 0` and clears `dc_bus_live` there - and the state is reliably left, because `stop_balancing()` raises an INFO event and every event path recomputes the status back to `ACTIVE`. So there is no healthy path that this gate blocks.

Three regression tests cover it: the latch racing the startup gate at t=0, the slower-than-10 ms case above, and a guard that the term is `== ACTIVE` and not merely `!= FAULT` - weakening it that way fails exactly that one test. 209 host tests pass, with shuffled ordering, and `lilygo_330` and `stark_330` build clean.

**Not verified on hardware.** This is a host-suite change to a contactor ladder; the natural check is a faulted board with the negative contactor line under observation, and it has not been run. The window was reproduced three times independently in analysis, most recently against current main, but that is not the same as watching the contactor stay open.

Note: drafted with AI assistance, reviewed by me.

</details>

---


**Allocate the SPI controller, not just the pins**
Branch [`spi-bus-guard`](https://github.com/ekholm/Battery-Emulator/tree/spi-bus-guard) @ `eb483d9c` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:spi-bus-guard)
`alloc_pins()` allocates GPIO numbers, so two devices sharing one SPI controller with different pins pass it silently - then the second `begin()` re-points the controller's single MISO source and the first device goes deaf, unreported. Adds `claim_spi_bus()` and an event naming both devices. **Pairs with [`sd-spi-bus-hspi`](https://github.com/ekholm/Battery-Emulator/tree/sd-spi-bus-hspi)**: alone, this correctly warns on every boot of a T-CAN485 with SD logging plus an MCP2515.

<details>
<summary>PR body it would ship with</summary>

`alloc_pins()` allocates GPIO **numbers**, so two devices that land on the same SPI controller with different pins pass it without complaint. Then both call `SPIClass::begin()` - and an ESP32 SPI controller sources its MISO input from exactly one GPIO, so the second `begin()` re-points it and the first device stops receiving. Nothing is returned, nothing is logged, and the device is simply deaf from then on.

That is not hypothetical: it is shipping on the T-CAN485, where the SD card and the MCP2515 add-on are both on VSPI. Measured on hardware, the card mounts, and a second later `init_CAN()` takes the controller away from it. Every subsequent log write fails silently. The board-level fix is a separate change (`sd-spi-bus-hspi` on this fork); this one is about the class.

This adds `claim_spi_bus(name, bus, sck, miso, mosi)` - the sibling allocator for the controller that `alloc_pins()` is for the pins. It records who holds a bus and with which pin triple, and raises `EVENT_SPI_BUS_CONFLICT` when a second device claims the same controller with a *different* triple. It is wired into all four `SPIClass::begin()` call sites: the MCP2515, both MCP2518FD instances, and the SD card.

Three design calls, each of which could have gone the other way:

- **Identical wiring is allowed, silently.** Several chip selects on one bus is how SPI is meant to be used. Reporting that would fire on every correct multi-device bus and bury the real collisions.
- **It warns, it does not veto.** On the boards where this fires today it is the *second* `begin()` that ends up working, so refusing the second claim would trade a deaf SD card for a dead CAN interface. A warning that names both devices is more useful than either failure.
- **WARNING, not ERROR, and this one is behavioural rather than taste.** `update_bms_status()` turns any active error-level event into `system_status = FAULT`. A logging device losing its SPI routing must not fault an emulator that is otherwise running. There is a test pinning it.

**The event owns its own name storage rather than borrowing the GPIO allocator's.** Event message strings are rendered lazily - every time the event is published, by the events page, MQTT and ESP-NOW alike - so a message built from shared "who failed last" globals will name whoever failed an allocation most recently, not the two devices that actually collided. That bites this event particularly hard because it is persistent: it is raised on every boot of an affected board, so it is re-rendered for the life of that board with every opportunity for something unrelated to fail an allocation in between. Worth knowing before anyone simplifies it back onto the shared slots.

**What the hardware run turned up, which is worth more than the guard.** With the guard compiled in, the order of the two `begin()` calls on a real T-CAN485 **inverts** - the CAN controller now initialises first and the SD card ends up owning the bus, the opposite of what the same board does without it, reproduced three times against a control built alongside. So **which of the two devices goes deaf is not stable across builds**, and the apparent second of margin between them is a consequence of who won rather than a reason it is safe: whoever calls `begin()` first holds the SPI lock through its own initialisation and pushes the other out. A field report of either shape - a silent SD card, or a CAN interface that stopped receiving - fits this defect.

Cost is **+1,372 B of flash and +16 B of RAM**, measured baseline-against-subject in one build-lock hold from wiped build directories, on `lilygo_330` as the tightest board. Four envs build green, including the S3 FD board because it is the only one exercising the second FD claim, and dfrobot_edge101 because it is the only other board with an SD card.

Eight host tests, each mutation caught by exactly its own: the claim never recorded, the wiring not compared, MISO left out of the comparison, the bus number ignored, an unconfigured device reserving a bus, the event raised at error level, the message dropping the device that loses the bus, and the event never raised at all.

Note: drafted with AI assistance, reviewed by me.

</details>

---


**ACAN_ESP32: IRAM-safe interrupt chain + RX overrun recovery**
Branch [`acan-iram-overrun`](https://github.com/ekholm/Battery-Emulator/tree/acan-iram-overrun) @ `e840bb4a` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:acan-iram-overrun)
Two stacked defects in the vendored driver behind the "bogus native CAN data during OTA" class: the TWAI ISR is masked through every flash-cache-off window, and a detected RX overrun is never recovered, leaving the FIFO read pointer desynced.

<details>
<summary>PR body it would ship with</summary>

Two stacked defects in the vendored ACAN_ESP32 driver, both in the class behind "bogus Native CAN data during OTA" (#2813's territory):

1. **The TWAI ISR is not IRAM-safe.** `esp_intr_alloc` is called without `ESP_INTR_FLAG_IRAM` (the `IRAM_ATTR` on the handler is decorative without it), so the ISR is masked through every flash-cache-off window - NVS commits, OTA writes. At 500 kbit/s the 64-byte RX FIFO overruns in about 5 ms, and OTA is back-to-back windows. Fix: the flag, plus `IRAM_ATTR` on the whole reachable chain - the out-of-line `Buffer16::append/remove` (weak symbols) were the live trap: the flag alone would have moved the crash to "cache-off dispatch into flash-resident code".

2. **RX overrun detected, never recovered.** `TWAI_CLR_OVERRUN` (0x08) is defined in the header and never issued anywhere. After an overrun the SJA1000-family FIFO read pointer desyncs and the driver reads interleaved garbage with plausible IDs. Fix mirrors IDF's `twai_hal_clear_rx_fifo_overrun()` semantics (release buffered messages until RMC reads 0, then the clear-data-overrun command); drained frames are discarded - past the overrun point the frame boundaries are unreliable, which is exactly what `CONFIG_TWAI_ERRATA_FIX_RX_FIFO_CORRUPT` (enabled in the shipped framework) is about. The drain loop is bounded (a misbehaving counter degrades to a missed drain, never a hung ISR). New counters `hardwareRxOverrunCount` / `hardwareRxOverrunDroppedFrameCount` make the behaviour observable.

Verified: lilygo_330 / esp32devkit_330 / stark_330 / BECom_330 build green; ISR-chain placement checked by disassembly on both architectures - every call target sits in IRAM, the one indirect call is the compiler's memset in ROM (always mapped). Host suite unchanged (the register/ISR path is not host-mockable without a TWAI register emulation - stated rather than pretended). Before/after overrun numbers from a two-board bench under forced NVS/OTA windows are being collected.

Positioning: this is a bridge for today's users of the vendored driver - superseded by design the day the IDF-twai swap (#2813, waiting on IDF 6.1) lands.

Note: drafted with AI assistance, reviewed by me.

</details>

---



*More is in preparation. Ask if it would help to know what is coming.*
