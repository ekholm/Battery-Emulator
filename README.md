# Battery-Emulator ⚡🔋
![GitHub release (with filter)](https://img.shields.io/github/v/release/dalathegreat/BYD-Battery-Emulator-For-Gen24?color=%23008000)
![GitHub Repo stars](https://img.shields.io/github/stars/dalathegreat/Battery-Emulator?style=flat&color=%23128512)
![GitHub forks](https://img.shields.io/github/forks/dalathegreat/Battery-Emulator?style=flat&color=%23128512)
![GitHub actions](https://img.shields.io/github/actions/workflow/status/dalathegreat/BYD-Battery-Emulator-For-Gen24/compile-common-image-lilygo-TCAN.yml?color=0E810E)
![Static Badge](https://img.shields.io/badge/made-with_love-blue?color=%23008000)

## 🧰 Patch shelf — prepared, not filed

*Offered pull-style per maintainer preference: nothing here is filed as a PR. Point at whichever is
useful, ignore the rest — each entry links the branch, the pinned commit, and the full diff against
upstream `main`, and carries the PR body it would ship with. An entry becomes a PR only on request.*

---

**Silent assertions: drop the assert message strings, keep every check (−55 KB flash per board)**
Branch [`assertions-silent`](https://github.com/ekholm/Battery-Emulator/tree/assertions-silent) @ `496b628a` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:assertions-silent)
One config line in the shared size defaults; every check still compiled in and still aborts — only the per-assert message strings go. Measured −55,680 B (lilygo) / −55,432 B (devkit) from wiped, flag-verified builds.

<details>
<summary>PR body it would ship with</summary>

This enables `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT` in the shared `sdkconfig.be_size.defaults`, so it applies to every board env identically.

**What it does:** every `assert()`/IDF invariant check is compiled in and still aborts on failure, exactly as today. Only the per-assert message strings (file/expression text) are dropped — and the strings are where the flash mass is.

**Measured** (wiped build dirs, `sdkconfig.<env>` deleted before each build and its regeneration with the expected flag verified — PlatformIO can silently skip a config change under some cache states; the lilygo pair below was built baseline-then-SILENT inside one continuous build-lock hold):

| env | baseline | SILENT | delta | full-DISABLE (reference) |
|---|---|---|---|---|
| lilygo_330 | 1,866,720 B | 1,811,040 B | **−55,680 B (−2.98 %)** | −65,040 B |
| esp32devkit_330 | 1,821,232 B | 1,765,800 B | **−55,432 B (−3.05 %)** | −64,704 B |

That is ~85.6 % of the full `ASSERTIONS_DISABLE` saving with none of its safety cost — and a word of caution on the other half: **`_DISABLE` is the wrong move on a safety-related system.** It compiles the checks OUT, so a broken invariant no longer aborts — the firmware keeps running on falsified assumptions, on hardware that drives contactors. Tests cover the cases we thought of; asserts exist for the ones we didn't. The remaining ~9.4 KB is, in effect, the price of every runtime invariant in the image — the right 9.4 KB to keep. SILENT needs no stability gate at all, which is why it could merge today.

**The honest cost:** an assert failure now aborts with the program counter instead of the message string. The backtrace still prints and decodes against the ELF of that build:

    xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <PC>

**Cost / pace / breakage:** abstraction cost — none, one config line, no code changes; pace — nothing changes in anyone's workflow; breakage plan — revert is the same one line, and any assert that fires still aborts loudly, so a problem presents exactly as today, minus one string.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**ACAN_ESP32: IRAM-safe interrupt chain + RX overrun recovery**
Branch [`acan-iram-overrun`](https://github.com/ekholm/Battery-Emulator/tree/acan-iram-overrun) @ `a321f073` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:acan-iram-overrun)
Two stacked defects in the vendored driver behind the "bogus native CAN data during OTA" class: the TWAI ISR is masked through every flash-cache-off window, and a detected RX overrun is never recovered, leaving the FIFO read pointer desynced.

<details>
<summary>PR body it would ship with</summary>

Two stacked defects in the vendored ACAN_ESP32 driver, both in the class behind "bogus Native CAN data during OTA" (#2813's territory):

1. **The TWAI ISR is not IRAM-safe.** `esp_intr_alloc` is called without `ESP_INTR_FLAG_IRAM` (the `IRAM_ATTR` on the handler is decorative without it), so the ISR is masked through every flash-cache-off window — NVS commits, OTA writes. At 500 kbit/s the 64-byte RX FIFO overruns in about 5 ms, and OTA is back-to-back windows. Fix: the flag, plus `IRAM_ATTR` on the whole reachable chain — the out-of-line `Buffer16::append/remove` (weak symbols) were the live trap: the flag alone would have moved the crash to "cache-off dispatch into flash-resident code".

2. **RX overrun detected, never recovered.** `TWAI_CLR_OVERRUN` (0x08) is defined in the header and never issued anywhere. After an overrun the SJA1000-family FIFO read pointer desyncs and the driver reads interleaved garbage with plausible IDs. Fix mirrors IDF's `twai_hal_clear_rx_fifo_overrun()` semantics (release buffered messages until RMC reads 0, then the clear-data-overrun command); drained frames are discarded — past the overrun point the frame boundaries are unreliable, which is exactly what `CONFIG_TWAI_ERRATA_FIX_RX_FIFO_CORRUPT` (enabled in the shipped framework) is about. The drain loop is bounded (a misbehaving counter degrades to a missed drain, never a hung ISR). New counters `hardwareRxOverrunCount` / `hardwareRxOverrunDroppedFrameCount` make the behaviour observable.

Verified: lilygo_330 / esp32devkit_330 / stark_330 / BECom_330 build green; ISR-chain placement checked by disassembly on both architectures — every call target sits in IRAM, the one indirect call is the compiler's memset in ROM (always mapped). Host suite unchanged (the register/ISR path is not host-mockable without a TWAI register emulation — stated rather than pretended). Before/after overrun numbers from a two-board bench under forced NVS/OTA windows are being collected.

Positioning: this is a bridge for today's users of the vendored driver — superseded by design the day the IDF-twai swap (#2813, waiting on IDF 6.1) lands.

Note: drafted with AI assistance, reviewed by me.

</details>

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

## What is Battery Emulator?

Many manufacturers sell home battery systems to enable homeowners to store power collected from the grid, or renewable sources, to use at times when electricity demand is higher. However almost all of these home battery systems charge a high cost for every kilowatt hour (kWh) of capacity you buy.

At the same time, EV manufacturers have been putting high capacity battery packs into their cars, with no firm plan for what should happen to those batteries if the car is damaged in a crash, or reaches the end of its life as a vehicle.

**Battery Emulator** enables EV battery packs to be repurposed for stationary storage. It acts as a translation layer between the EV battery and the home inverter. This makes it extremely cheap and easy to use large EV batteries in a true plug'n'play fashion!

> [!CAUTION]
> Working with high voltage is dangerous. Always follow local laws and regulations regarding high voltage work. If you are unsure about the rules in your country, consult a licensed electrician for more information.


## Quickstart guide 📜
- Pick a [supported inverter](https://github.com/dalathegreat/Battery-Emulator/wiki#supported-inverters-list) (solar panels optional) :sun_with_face: 
- Pick a [supported battery](https://github.com/dalathegreat/Battery-Emulator/wiki#supported-batteries-list) :battery: 
- Order the Battery-Emulator [compatible hardware](https://github.com/dalathegreat/Battery-Emulator/wiki#where-do-i-get-the-hardware-needed) :robot: 
- Follow the [installation guidelines](https://github.com/dalathegreat/Battery-Emulator/wiki/Installation-guidelines) section for how to install and commission your battery properly :notebook: 

## Installation basics 🪛
1. Connect your Battery Emulator hardware to your EV battery
2. Connect your Battery Emulator hardware to your inverter
3. Wire up high voltage cable between the inverter and the battery
4. Add a low voltage power supply to your Battery Emulator hardware
5. Configure any additional requirements to allow Battery Emulator to switch on your EV battery (also referred to as 'closing contactors')
6. Enjoy a big cheap grid connected battery!

For examples showing wiring, see each battery type's own Wiki page. For instance the [Nissan LEAF page](https://github.com/dalathegreat/Battery-Emulator/wiki/Battery:-Nissan-LEAF---e%E2%80%90NV200)

## How to install the software 💻

Start by watching this [quickstart guide](https://www.youtube.com/watch?v=sR3t7j0R9Z0)

[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/sR3t7j0R9Z0/0.jpg)](https://www.youtube.com/watch?v=sR3t7j0R9Z0)

1. Open the [webinstaller page](https://dalathegreat.github.io/BE-Web-Installer/)
2. Follow the instructions on that page to install the software
3. After successful installation, connect to the wireless network (Battery-Emulator , password: 123456789)
4. Go to setup page and configure component selection
5. (OPTIONAL, connect the board to your home Wifi)
6. Connect your battery and inverter to the board and you are done! 🔋⚡

## Dependencies 📖
This code uses the following excellent libraries: 
- [adafruit/Adafruit_NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) LGPL-3.0 license
- [ayushsharma82/ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) AGPL-3.0 license 
- [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) MIT-License
- [eModbus/eModbus](https://github.com/eModbus/eModbus) MIT-License
- [ESP32Async/AsyncTCP](https://github.com/ESP32Async/AsyncTCP) LGPL-3.0 license
- [ESP32Async/ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) LGPL-3.0 license
- [pierremolinaro/acan-esp32](https://github.com/pierremolinaro/acan-esp32) MIT-License
- [pierremolinaro/acan2517FD](https://github.com/pierremolinaro/acan2517FD) MIT-License

It is also based on the information found in the following excellent repositories/websites:
- https://gitlab.com/pelle8/inverter_resources //new url
- https://github.com/burra/byd_battery
- https://github.com/flodorn/TeslaBMSV2
- https://github.com/SunshadeCorp/can-service
- https://github.com/openvehicles/Open-Vehicle-Monitoring-System-3
- https://github.com/dalathegreat/leaf_can_bus_messages
- https://github.com/rand12345/solax_can_bus
- https://github.com/Tom-evnut/BMWI3BMS/ SMA-CAN
- https://github.com/FozzieUK/FoxESS-Canbus-Protocol FoxESS-CAN
- https://github.com/maciek16c/hyundai-santa-fe-phev-battery
- https://github.com/ljames28/Renault-Zoe-PH2-ZE50-Canbus-LBC-Information
- Renault Zoe CAN Matrix https://docs.google.com/spreadsheets/u/0/d/1Qnk-yzzcPiMArO-QDzO4a8ptAS2Sa4HhVu441zBzlpM/edit?pli=1#gid=0
- Pylon hacking https://www.eevblog.com/forum/programming/pylontech-sc0500-protocol-hacking/

## Like this project? 💖
Leave a ⭐ If you think this project is useful. Consider hopping onto my Patreon to encourage more open-source projects! As a bonus, you will get access to the Discord server, where we hangout, develop, support, share, discuss etc. all things related to DIY EV storage solutions. See you on the server? ;)

<a href="https://www.patreon.com/dala">
	<img src="https://c5.patreon.com/external/logo/become_a_patron_button@2x.png" width="160">
</a> <------ Click here to learn more!


![image](https://github.com/user-attachments/assets/66b8e967-7f5e-409d-91ec-d012489a86d2)
