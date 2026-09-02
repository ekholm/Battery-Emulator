# Patch shelf

*Prepared, not filed. Offered pull-style per maintainer preference: nothing here is a pull
request. Point at whichever entry is useful, ignore the rest.*

*Each entry links the branch, the pinned commit and the full diff against upstream `main`, and
carries the PR body it would ship with. An entry becomes a PR only on request.*

*Ordered so that changes applying directly to current `main` come first; entries further down
depend on other work or have not been placed yet. Every entry is independent unless it says so.*

See also [FINDINGS.md](FINDINGS.md), for things we have found but cannot fix ourselves.

---

**NeoPixel: `pin` is read before it is ever written - a silent boot loop on the boards where the leftover byte matters**
Branch [`neopixel-uninitialised-pin`](https://github.com/ekholm/Battery-Emulator/tree/neopixel-uninitialised-pin) @ `066dd987` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:neopixel-uninitialised-pin)
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

**OTA: confirm a new image only after the system has actually run**
Branch [`ota-confirm-after-run`](https://github.com/ekholm/Battery-Emulator/tree/ota-confirm-after-run) @ `e338c48f` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:ota-confirm-after-run)
A firmware update that cannot run is permanent today, because arduino-esp32 confirms the image before `setup()` runs - so the bootloader's rollback, which is already enabled, can never fire. This defers confirmation via the framework's own `verifyRollbackLater()` hook until the running system has held together for 42 s. +1,072 B.

<details>
<summary>PR body it would ship with</summary>

A firmware update that cannot run is permanent today. The board boot-loops, and the only way out is USB - which is why the standing advice for "bricked by update" has become "erase the flash and reconfigure", losing every setting.

That is not for want of a rollback mechanism. `CONFIG_APP_ROLLBACK_ENABLE` is already on, and the bootloader already runs a new image as `PENDING_VERIFY` and restores the previous one if that boot resets before the app confirms itself. What closes the window is arduino-esp32: `initArduino()` calls `esp_ota_mark_app_valid_cancel_rollback()` **before** `setup()` runs, so an update is confirmed before a single line of this firmware has executed. Every crash worth rolling back for happens after that point.

This takes the framework's own opt-out - the weak `verifyRollbackLater()` hook - and moves the confirmation to where it can mean something: **the image is confirmed once the running system has held together for 42 seconds.**

**Why a running system and not the end of `setup()`.** Reaching the end of `setup()` only says the drivers were constructed. The failures that justify a rollback happen after that - the first frames parsed, the first MQTT or HTTP exchange, a watchdog trip under real load. 42 s also clears STA association, TLS-MQTT and autodiscovery bring-up, so first-contact crashes still revert.

The check is one guarded compare at an unconditional point in the 1 ms core task - "both sub-tasks have been alternating for 42 s" is the strongest liveness witness the firmware has, since it means CAN is being serviced and the task watchdog fed every pass. The check only sets a flag; the write happens in ordinary main-task context from `loop()`, because confirming is itself a flash write and belongs on the same path as every other runtime flash write rather than being initiated from the core tick.

**Criteria deliberately rejected**, since they look reasonable: *"all RTOS tasks started"* - starting is cheap, surviving under load is the signal. *Anything externally dependent* (CAN frames seen, WiFi up) - a bench board with no bus, or an offline install, must still be able to confirm, or every reboot reverts a perfectly good image forever.

**A deliberate restart inside the window keeps the update.** The web-UI restart path confirms first if the system is healthy, then reboots. Panics, watchdog resets and brownouts still revert.

**The honest cost to a user, stated plainly:** an image that dies at t = 30 s, or a power cut inside the window, now rolls back where previously it would have been kept. That is the intent - an image that cannot survive its first 42 seconds is not one to keep - but it is a real behaviour change and not only an improvement.

**Evidence.** On a T-CAN485, with a deliberately faulted image OTA'd over a good one: before, OTA -> panic -> reboot -> still dead 200 s later, USB recovery required. After, the board is back on the previous firmware on its own, `otadata` reads `ABORTED` for the failed slot and `VALID` for the running one, and the event log names which version failed. Three further cases pass on hardware: a good update survives 42 s and a reboot; a good update survives a restart issued from the web UI *inside* the window; and a board sitting in first-boot configuration mode confirms there too, so no healthy state exists in which an update is never confirmed.

On the host, 217 tests pass and the policy is now testable in a way it was not before - the 42 s boundary, the once-only latch and the restart arming all run on the host; only the `otadata` mechanics remain bench-only.

Cost is **+1,072 B** of flash (`esp32devkit_330`, baseline and subject built back-to-back from wiped build directories in one lock hold). No sdkconfig change - the flag was never off.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Contactors: a faulted system must not arm the closing ladder at boot**
Branch [`contactor-fault-boot-race`](https://github.com/ekholm/Battery-Emulator/tree/contactor-fault-boot-race) @ `1c168f8b` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:contactor-fault-boot-race)
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

**T-CAN485: give the SD card its own SPI controller, and check SD writes**
Branch [`sd-spi-bus-hspi`](https://github.com/ekholm/Battery-Emulator/tree/sd-spi-bus-hspi) @ `996ff954` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sd-spi-bus-hspi)
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

**Allocate the SPI controller, not just the pins**
Branch [`spi-bus-guard`](https://github.com/ekholm/Battery-Emulator/tree/spi-bus-guard) @ `7ab3343a` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:spi-bus-guard)
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

**Silent assertions: drop the assert message strings, keep every check (−55 KB flash per board)**
Branch [`assertions-silent`](https://github.com/ekholm/Battery-Emulator/tree/assertions-silent) @ `496b628a` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:assertions-silent)
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

**ACAN_ESP32: IRAM-safe interrupt chain + RX overrun recovery**
Branch [`acan-iram-overrun`](https://github.com/ekholm/Battery-Emulator/tree/acan-iram-overrun) @ `a321f073` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:acan-iram-overrun)
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

**Ford Mach-E: hand the UDS transport to the shared superclass, and fix two latent superclass bugs it exposed**
Branch [`mache-uds-superclass`](https://github.com/ekholm/Battery-Emulator/tree/mache-uds-superclass) @ `7fe98e29` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:mache-uds-superclass)
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
Branch [`mg5-uds-superclass`](https://github.com/ekholm/Battery-Emulator/tree/mg5-uds-superclass) @ `eb709d0c` · stacked on `mache-uds-superclass` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:mg5-uds-superclass)
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

*More is in preparation. Ask if it would help to know what is coming.*
