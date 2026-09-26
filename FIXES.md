# Fixes

*Prepared, not filed. Offered pull-style per maintainer preference: nothing here is a pull
request. Point at whichever entry is useful, ignore the rest.*

*Each entry links the branch, the pinned commit and the full diff against the upstream release
`v12.6.0`, and carries the PR body it would ship with. An entry becomes a PR only on request.*

*Grouped by area: [CAN](#can), [Battery drivers](#battery-drivers), [Inverters](#inverters), [Contactors and safety](#contactors-and-safety), [Settings and web UI](#settings-and-web-ui), [Platform, build and storage](#platform-build-and-storage). Within each group, changes applying directly to
the current release `v12.6.0` come first; entries further down depend on other work or have not
been placed yet. Every entry is independent unless it says so.*

Defect repairs only - small, self-contained, each ready to merge. See also [FEATURES.md](FEATURES.md)
for larger changes and conversions, and [FINDINGS.md](FINDINGS.md) for things we have found but
cannot fix ourselves.

**Kept current.** Every entry is re-checked against upstream `main`, and one whose defect `main`
has since fixed is removed rather than left to waste your time.

*Each entry's branch line names the upstream commit its branch is rebased onto. Since the
2026-09-21 base ruling that is the latest upstream RELEASE, not whatever `main` reads today, and a
new release rather than a merge is what triggers the next uplift cycle. The branch was rebuilt
there and the host test suite run on the result, all green, unless its entry says otherwise. The
pinned commits are those rebased, tested ones.*

*One entry below, `can-speed-conflict`, still names an older `main` instead: its branch sits
outside this shelf and has not been lifted, and saying it was would be false.*

## CAN

---

**CAN replay: a malformed log line writes past the end of a global frame buffer**
Branch [`can-replay-dlc-bound`](https://github.com/ekholm/Battery-Emulator/tree/can-replay-dlc-bound) @ `28f4bb3e` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-replay-dlc-bound)
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
Branch [`can-replay-null-data`](https://github.com/ekholm/Battery-Emulator/tree/can-replay-null-data) @ `d1becc24` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-replay-null-data) · stacked on `can-replay-dlc-bound` (includes it)
When a log line ends at the DLC, `strtok(NULL, " ")` is called with no live tokenisation state - the prior `strtok` chain ran on a String's internal buffer that has gone out of scope by then. The parse now goes through a host-tested helper that owns its buffer for the whole parse, and the test reproduces the use-after-free rather than asserting around it.

<details>
<summary>PR body it would ship with</summary>

`canReplayTask` tokenises a log line's data bytes with a `strtok` chain that starts on the internal buffer of a `String` produced earlier in the loop. When a line carries a DLC but no data bytes, the loop's next iteration calls `strtok(NULL, " ")` - continuing a tokenisation whose backing buffer has already gone out of scope. Use-after-free on user-supplied input.

The data-byte parse moves into a small helper that owns its buffer for the whole parse and is host-testable in isolation. The accompanying test reproduces the use-after-free shape (it fails against the old code for the defect's actual reason, not a proxy), and the helper is exercised by the DLC-bound tests as well.

Stacked on `can-replay-dlc-bound`: this branch contains that fix, and the two together make the replay parser refuse malformed lines and parse well-formed ones without touching freed memory.
</details>

---

**CAN: two drivers that disagree on an interface's bitrate fail closed at boot, and say so**
Branch [`can-speed-conflict`](https://github.com/ekholm/Battery-Emulator/tree/can-speed-conflict) @ `a0b582d0` · on upstream `main` @ `72516786` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-speed-conflict)
An interface's bitrate is owned by whichever driver registered first, and setup runs charger, inverter, battery, shunt - so a 250 kbit/s battery sharing an interface with a charger or a 500 kbit/s inverter comes up on a 500 kbit/s bus, deaf, with nothing logged and no event. It presents as "battery not detected", and it is reachable from the settings page alone. The registrations for an interface are now compared before it starts: a disagreement leaves that interface stopped and raises `EVENT_CAN_SPEED_CONFLICT` naming both drivers and both speeds, while the other interfaces still come up. Raised in [discussion #2871](https://github.com/dalathegreat/Battery-Emulator/discussions/2871).

<details>
<summary>PR body it would ship with</summary>

The bitrate a CAN interface comes up at is decided by whichever driver registered first. Setup runs charger, inverter, battery, shunt, so a charger or a 500 kbit/s inverter sharing an interface with a 250 kbit/s battery brings the bus up at 500 kbit/s. The battery never hears a frame, nothing is logged, and no event fires. It presents as "battery not detected".

Six upstream drivers need 250 kbit/s (Akasol, CellPower, Relion LV, RJXZS, Pylon at its 250 setting, Sungrow inverter), and the charger and shunt can only ask for 500.

This change makes the interface own its bitrate. Each registration carries the driver's name; before an interface is started, its registrations are compared. If they agree, the interface starts at that speed. If they disagree, that interface is not started and `EVENT_CAN_SPEED_CONFLICT` names both drivers and both speeds, in the same shape `EVENT_GPIO_CONFLICT` already uses for pins. Other interfaces still start. A runtime speed change on a shared interface is refused the same way when the peer disagrees; a driver alone on its interface is unaffected.

No driver file changes: the name is supplied by the four base classes that register.

The two CAN-FD interface keys share one controller, so they are also compared with each other. Bus-off recovery re-initialises at the speed already in force and is not treated as a speed change.

Tests: eighteen host cases in `test/can_speed_conflict_tests.cpp` - agreement, disagreement in second and third position, the charger-first shape, per-interface isolation, the runtime refusal and its sole-registrant exemption, the CAN-FD key pair, and recovery after a runtime speed switch. Four of them scan `comm_can.cpp` source, because that file cannot be linked into the host binary: three check that `init_CAN()` and `change_can_speed()` actually consult the policy, and one that bus-off recovery does not. Builds on lilygo_330, stark_330 and lilygo_2CAN_330. Not run on hardware: the decision is a comparison of registered bitrates.
</details>

---

**Native CAN: a transmit to an interface that never started is a silent success**
Branch [`native-can-transmit-guard`](https://github.com/ekholm/Battery-Emulator/tree/native-can-transmit-guard) @ `f59f3050` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:native-can-transmit-guard)
The native TWAI interface is the only one whose init failure raises no event - the MCP2515 and CAN-FD paths both do - and a transmit to it after a failed or absent init simply disappears. On boards that log nothing unless USB logging is enabled, that is a dead peripheral presenting as a working one. This refuses the transmit and raises a new `EVENT_CAN_NATIVE_NOT_INITIALIZED`, APPENDED at the end of the event enum: event ordinals go out on the wire (ESP-NOW publishes the enum value as a u16), so a mid-enum insertion would renumber every event after it for any peer on a different build. Review closed the second path to the dead peripheral, and a test pins the enum layout so the next event cannot un-append it.

<details>
<summary>PR body it would ship with</summary>

`transmit_can_frame_to_interface()` routed a `CAN_NATIVE` frame to `ACAN_ESP32::tryToSend()` with no check that the interface came up. `tryToSend()` writes TWAI registers from inside `portENTER_CRITICAL`. If the peripheral was never taken out of reset, that write raises a hardware exception with interrupts already masked - a double exception - and the watchdog resets the board straight back into the same transmit. Measured on silicon with a battery on the MCP2515 add-on and native unconfigured: roughly 45 resets a minute, indefinitely. On S3 boards each boot is another chance to lose the USB port; two boards here needed physical replugs after exactly this.

Every other interface in the same switch already refuses this way - they hold a driver pointer and short-circuit on null. The native path has a `native_can_initialized` flag instead, and that flag was only ever read by `receive_can()`, so the interface was inert inbound and lethal outbound.

The fix reads the flag before reaching `tryToSend()`, drops the frame, sets a datalayer flag, and the safety pass raises `EVENT_CAN_NATIVE_NOT_INITIALIZED` (WARNING). Trading a boot loop for an unexplained dead interface would not be an improvement on boards that log nothing unless `USBENABLED` is set.

Review found the same hole in two further entry points: `stop_can()` and `restart_can()` wrote TWAI registers with no init check. `restart_can()` additionally dereferences `settingsespcan`, which is only assigned when native init succeeds - on a pin-conflicted or unconfigured native interface that dereference is null.

The event is appended to the enum rather than inserted beside related events. ESP-NOW publishes the enum value as a `uint16_t`, so a mid-enum insertion renumbers every event after it for any peer on a different build.

`comm_can.cpp` is not part of the host binary, so the guard's properties are read from source. The reporting path is tested for real.

233 host tests (1 pre-existing skip); lilygo_330, lilygo_2CAN_330 and stark_330 build clean.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**CAN: a missing add-on chip is reported as a full buffer**
Branch [`uninitialized-interface-diagnosis`](https://github.com/ekholm/Battery-Emulator/tree/uninitialized-interface-diagnosis) @ `9ac81590` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:uninitialized-interface-diagnosis) · pairs with the entry above
When an SPI add-on CAN chip is absent or failed to init, transmits to it surface as `CAN_BUFFER_FULL` - a message that sends the reader towards traffic load when the truth is "this chip never existed". Seen on the bench: a board with nothing registered on its MCP2515 interface, so no driver object, raised it from the first replayed frame. Three per-interface not-initialized events replace the misdiagnosis, appended to the enum for the same wire-ordinal reason as the entry above; review tightened the replacement message so it does not promise an error state that need not exist.

<details>
<summary>PR body it would ship with</summary>

When an SPI add-on CAN chip is absent or failed to initialize, `init_CAN()` leaves its driver pointer null. `transmit_can_frame_to_interface()` already short-circuited on null and never dereferenced it - that was not the defect. The defect was what happened next: the short-circuit set the same flag a genuine failed send sets, so what reached the user was `EVENT_CANMCP2515_BUFFER_FULL`, `EVENT_CANFD_BUFFER_FULL` or `EVENT_CANFD_2_BUFFER_FULL`: "CAN failed to send. Buffer full or no one on the bus to ACK the message!"

A chip that is not there is neither of those. The message sends someone to check bus wiring and ACK counts for a peripheral that was never running. Seen on the bench: a board with nothing registered on its MCP2515 interface, so no driver object, raised `EVENT_CANMCP2515_BUFFER_FULL` from the first replayed frame (179 within seconds) with nothing on the wire.

Each add-on case now tests its driver pointer before building the frame, sets a per-chip flag in `datalayer.system.info`, and the safety pass raises `EVENT_CANMCP2515_NOT_INITIALIZED`, `EVENT_CANFD_NOT_INITIALIZED` or `EVENT_CANFD_2_NOT_INITIALIZED` (all WARNING). The old null short-circuit in the send check is removed rather than left alongside the new guard - leaving it in place would keep the old buffer-full path reachable for the same condition.

All three events share one message string the way the buffer-full family does, since the event name itself says which interface. The message deliberately does not promise that a boot-time initialization error was raised: three early-exit paths in `init_CAN()`'s native branch run before `can2515` is ever created, so a pin conflict on the native pins leaves the MCP2515 null with no prior error event.

Review tightened the replacement text so it names where to look next rather than implying an error state that may not exist.

All three events are appended to the enum for the same wire-ordinal reason as the native entry this pairs with.

`comm_can.cpp` has no host build; the three guards are read from source. The reporting path is tested for real.

235 host tests (1 pre-existing skip); 8 mutations each biting. lilygo_2CAN_330, lilygo_330 and esp32devkit_330 build clean.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Allocate the SPI controller, not just the pins**
Branch [`spi-bus-guard`](https://github.com/ekholm/Battery-Emulator/tree/spi-bus-guard) @ `f0f1d036` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:spi-bus-guard)
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
Branch [`acan-iram-overrun`](https://github.com/ekholm/Battery-Emulator/tree/acan-iram-overrun) @ `bf5785fe` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:acan-iram-overrun)
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

**CAN: a frame too long for classic CAN is reported as a full buffer**
Branch [`can-frame-too-long-diagnosis`](https://github.com/ekholm/Battery-Emulator/tree/can-frame-too-long-diagnosis) @ `fbf10251` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:can-frame-too-long-diagnosis)
The native and MCP2515 transmit paths refuse a frame whose DLC exceeds the 8 bytes classic CAN carries - correctly, copying one would overrun the driver frame - but report it by setting the same flag a failed send sets. The user is told "Buffer full or no one on the bus to ACK the message!" and sent to check wiring. It is none of those, and above all it is not transient: it is a CAN-FD battery configured on a classic interface, and nothing refuses that pairing.

<details>
<summary>PR body it would ship with</summary>

The `CAN_NATIVE` and `CAN_ADDON_MCP2515` transmit cases refuse a frame whose DLC exceeds 8 - copying one into the driver frame would overrun it on the stack - and report the refusal by setting the same flag a failed send sets. What reaches the user is `EVENT_CAN_NATIVE_BUFFER_FULL` or `EVENT_CANMCP2515_BUFFER_FULL`: *"CAN failed to send. Buffer full or no one on the bus to ACK the message!"*

It is none of those. Not a buffer, not the bus, and not transient. It is a CAN-FD battery configured on a classic interface, and nothing anywhere refuses that pairing: `comm_nvm.cpp`'s `readIf()` maps the stored `BATTCOMM` to an interface without consulting the battery, and no driver declares that it needs FD. The setting drops every frame that driver emits, for as long as it stands, while the events page sends the user to check wiring.

Four drivers construct frames this refuses - BMW-IX, KIA-64FD, KIA-E-GMP and MEB, at DLC 12 to 48 - and KIA-E-GMP transmits its DLC 32 frames unconditionally from its message table. It is reachable a second way that needs no battery at all: CAN replay parses a DLC out of an uploaded log and hands the frame to whichever interface the user picked.

Each case now reports the refusal as itself - `EVENT_CAN_NATIVE_FRAME_TOO_LONG` and `EVENT_CANMCP2515_FRAME_TOO_LONG` - latched through `datalayer.system.info` beside the send-fail and bus-error flags the safety pass already drains.

**On the level, because the first reason given for it was wrong.** WARNING, not ERROR - but not because `EVENT_CAN_BATTERY_MISSING` delivers the same shutdown 60 s later. That is only true of a purely request/response pack. `check_can_component_alive()` refreshes its counter from RECEIVED frames, so losing our TRANSMIT frames does not silence a pack that broadcasts - and the driver this is most reachable through is exactly such a pack. KIA-E-GMP refreshes on 0x055, 0x150, 0x1F5, 0x215, 0x21A and 0x235 and transmits none of them, so with all 63 of its DLC 32 frames dropped there is no backstop at all.

WARNING is still right, for a better reason: the dropped frames are the ones that would bring the pack online, so the contactors never close and there is nothing to shut down, while ERROR would put every inverter protocol that reads `system_status` into FAULT behaviour over a pack that is already inert. That property is now tested rather than asserted - 120 passes of `update_machineryprotection()` with the counter refreshed as a broadcasting pack refreshes it.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**CAN: the native path took one frame per loop and dropped three-quarters of a busy bus**
Branch [`native-can-drain-batch`](https://github.com/ekholm/Battery-Emulator/tree/native-can-drain-batch) @ `583ffe45` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:native-can-drain-batch)
`receive_frame_can_native()` takes one frame per call, once per iteration of the ~1 kHz core loop, while every other CAN interface drains a batch. That caps the native path at about 1000 frames per second: on a busy 500 kbit bus the rest is dropped inside the driver's ring, with no counter, no log and no event. Measured before and after, one commit apart: 74.4 % lost at wire rate becomes 0.000 % over 700 859 frames. The drain is now bounded by the ring's own depth.

<details>
<summary>PR body it would ship with</summary>

**The defect.** `receive_frame_can_native()` takes **one frame per call**:

```cpp
if (ACAN_ESP32::can.available()) {      // one frame, then return
  if (ACAN_ESP32::can.receive(frame)) {
```

Every other interface in the same file drains a batch:

```cpp
while (count++ < 16 && can2515->receiveFrame(rx_frame))   // MCP2515
while (canfd->available() && count++ < 16)                 // both MCP2518FD paths
```

`receive_can()` runs once per iteration of the ~1 kHz core loop, so one frame per call is one frame per millisecond. That makes the **application** the ceiling - not the bus, not the driver, and not the interrupt handler. A 500 kbit bus carries about 3900 frames per second. Everything above ~1000 f/s is dropped inside the driver's ring. **A pack streaming faster than about one frame per millisecond is silently three-quarters unheard.**

Silently is the word that matters. The frames are lost in the driver's ring before any application code sees them, so nothing counts them, nothing logs them, and no event fires. A user whose battery talks fast enough sees stale values and no indication why.

**The fix.** Drain until the ring is empty, bounded by the ring's own depth:

```cpp
const uint16_t drain_limit = ACAN_ESP32::can.driverReceiveBufferSize();
uint16_t drained = 0;
while (drained++ < drain_limit && ACAN_ESP32::can.receive(frame)) {
```

**Where the bound comes from, and why it is not 16.** The cap is asked of the driver (`driverReceiveBufferSize()`, backed by `mDriverReceiveBufferSize`, 32) rather than copied from the add-on paths' unexplained 16. The ring is the most the ISR can have queued since the last call, so a cap at its depth always empties whatever accumulated and no backlog carries over. **Any cap below the depth can leave frames behind on every iteration** - which is exactly how one-per-call failed, only less severely. Above the depth there is nothing left to take. Asking the driver also keeps the cap correct if the ring is ever deepened.

This is a catch-up cap, not a rate limit. The loop exits as soon as the ring is empty, so at normal traffic it costs one extra `receive()` call per iteration. It binds only after a stall.

The `available()` guard is gone: `receive()` reports an empty ring itself, so the guard cost a second critical section per call to answer the question the next line asks again.

**Measured before and after, one commit apart.** Two LilyGo T-2CAN boards (ESP32-S3) on a native CAN pair, one flooding a 500 kbit bus with sequence-numbered frames, the other running the firmware under test. The two builds differ by exactly this commit: the "before" build is its parent.

| build | offered | received | lost | frames |
|---|---|---|---|---|
| before, wire rate | 3880.5 f/s | 993.6 f/s | **74.395 %** | 116 913 |
| **after, wire rate** | 3891.1 f/s | 3891.1 f/s | **0.000 %** | **700 859** in 180 s |
| before, paced | 399.9 f/s | 399.9 f/s | 0.000 % | 12 040 |
| after, paced | 400.0 f/s | 400.0 f/s | 0.000 % | 48 046 |

Before the fix the receiver takes 993.6 f/s - one frame per millisecond of a 1 kHz loop, exactly the ceiling the source predicts. After it, the receiver takes the whole wire: none missing and none reordered. The zero rests on about 1.1 million frames across three separate boots, not on one window, and the frame count is cross-checked against the last sequence number the receiver reports.

**Loss is counted from the flood's own sequence numbers, not from a receive counter**, and that matters. A plain counter counts every native frame dispatched, unfiltered by CAN ID, and both boards run their own battery and inverter emulation on the same wire: foreign traffic inflates "received" and masks loss. Counting gaps in the flood's own sequence is immune to who else is talking.

The defect itself has been measured independently three more times, on an older base: 74.7 %, 74.112 % and 74.827 % lost at wire rate.

**The driver ring says the same thing.** Before the fix its high-water mark reads 33 in a 32-deep ring. That is not a depth: on overflow `ACAN_ESP32_Buffer16::append()` overwrites the peak with `size + 1` as a flag, and it stays there until reset. After the fix it peaks at 5 at wire rate - the ring is barely used, because the drain empties it every iteration.

**What the tick pays, and what is not measured.** The loss is not a loaded-CPU symptom: in the original measurement the board was idle while it dropped three-quarters of the bus, with the CAN RX function at 52 µs and the core task at 375 µs over 10 s. After the fix the core task's rolling 10 s maximum (398-596 µs) overlaps the before figure (311-531 µs), and neither approaches the 1 ms tick. That is an absence of evidence of harm, not proof of its absence: the perf page keeps one sticky worst case since boot, so whether a full 32-frame catch-up drain fits the tick needs an instrument that samples every iteration, and that has not been run. The drain binds only after a stall.

The host tests pin the source shape, not the throughput: they assert that the native path drains a batch, that its bound is asked of the ring rather than copied, and that no interface is left draining one frame at a time. They cannot observe frames on a wire; the table above is what does.

**Not claimed here.** This says nothing about the flash-cache window. Keeping the controllers draining while the cache is off is a different change, and no per-iteration bound can touch it, because the loop is not running during that window.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**CAN FD: a failed MCP2517FD init left its interrupt attached, then cleared the pointer the interrupt calls through**
Branch [`canfd-init-failure-isr`](https://github.com/ekholm/Battery-Emulator/tree/canfd-init-failure-isr) @ `20cd0bf5` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:canfd-init-failure-isr)
`begin_canfd()` hands the MCP2517FD driver an interrupt handler that reads the global `canfd`, then sets that global to `nullptr` when `begin()` fails. One of the library's failure codes is raised after the handler is attached and its task started, so the next interrupt on that path calls through a null pointer in interrupt context. The failure path now tears the driver down with the library's own `end()` before clearing the pointer, for both chips, and the build refuses the one library setting that would make that teardown run with interrupts masked.

<details>
<summary>PR body it would ship with</summary>

**The defect.** `begin_canfd()` in `Software/src/communication/can/comm_can.cpp`:

    static bool begin_canfd() {
      const uint32_t errorCode2517 = canfd->begin(*settings2517, [] { canfd->isr(); });
      canfd->poll();
      if (errorCode2517 != 0) {
        ...
        set_event(EVENT_CANMCP2518FD_INIT_FAILURE, (uint8_t)errorCode2517);
        // This will leak, but we have failed and won't try to reinit.
        canfd = nullptr;
        return false;
      }
      return true;
    }

The interrupt handler handed to `begin()` is a **captureless lambda that reads the global** `canfd`. It has to be: `ACAN2517FD::begin()` takes a plain `void (*)()`, and a capturing lambda does not convert to one. So `canfd = nullptr` does not disarm the handler - it removes the object the handler dereferences, and the next falling edge on nINT runs `nullptr->isr()` in interrupt context. `begin_canfd_2()` is the same code for the second chip.

**A non-zero return from `begin()` does not mean the driver is inert.** Reading `Software/src/lib/pierremolinaro-ACAN2517FD/ACAN2517FD.cpp`: twenty of the twenty-one error bits are set at lines 255-405, before the `if (errorCode == 0)` guard at line 409 - those paths never reach the install block, and for them the failure path is harmless. **One bit is set inside that block.** The requested-mode wait ORs in `kRequestedModeTimeOut` at line 548 when the chip does not reach `NormalFD` within 10 ms, and then falls through to `xTaskCreate(myESP32Task, "ACAN2517Handler", ...)` at 553 and `attachInterrupt(itPin, inInterruptServiceRoutine, FALLING)` at 557. `begin()` returns `1 << 16` with the handler live and a driver task running.

**Precondition:** a board whose HAL returns a real `MCP2517_INT()`. With the pin at 255 the library never attaches and none of this can fire. The T-2CAN's FD variant returns GPIO 8 (`hw_lilygo2can.h:63`), so it is a real configuration, not a hypothetical one.

**The change.** One line per function, before the pointer is cleared:

    canfd->end();

`ACAN2517FD::end()` is the library's own teardown and is the only option that drops all three pieces of live state: it detaches the interrupt on the same pin (line 582), deletes the driver task (604), and resets the chip (599) so nINT stops being asserted at the source. **It is already the established call in this file** - `stop_can()` calls `canfd->end()` on the OTA path - so the failure path is not inventing a teardown, it is using the one that exists.

It is safe on a driver that failed before anything was constructed: `end()` touches only members with in-class initialisers - `mCallBackFunctionArray = NULL` (`ACAN2517FD.h:99`), so the `delete []` is a no-op; `mESP32TaskHandle = nullptr` (141), so the `vTaskDelete` is skipped; both driver buffers default-constructed, so `initWithSize(0)` is well-defined - and `detachInterrupt` on a pin that was never attached is a no-op. Its two `millis()` wait loops are bounded at 2 ms.

**One qualifier on that last point, because it is the one member whose `end()`-side use is not unconditionally inert.** `end()` recomputes `digitalPinToInterrupt(mINT)` and detaches it whenever `mINT != 255`. On the `kINTPinIsNotAnInterrupt` path that expression is `-1` by definition - it is what set the code - and `detachInterrupt(uint8_t)` in the Arduino-ESP32 core takes that as `255` and indexes `__pinInterruptHandlers[]`, a `SOC_GPIO_PIN_COUNT`-entry table (40 on ESP32, 49 on S3), **without the bounds check its `__pinMode`/`__digitalWrite` neighbours have**. So on that one code the teardown would read and write past the table. **It is not reachable here:** all eight HALs return either a valid GPIO (8, 10, 14, 34, 35, 39) or `GPIO_NUM_NC`, and `Esp32Hal::alloc_pins()` rejects the negative one before the driver is ever constructed, so `kINTPinIsNotAnInterrupt` cannot be produced by any board in the tree. It is stated because the argument above is "safe on all twenty early-exit codes", and that is true only given a HAL pin in range - `alloc_pins()` checks `pin < 0` and has no upper bound.

The two alternatives were considered and are weaker. A bare `detachInterrupt(digitalPinToInterrupt(esp32hal->MCP2517_INT()))` closes the null dereference and leaves the `ACAN2517Handler` task alive forever, waking every 500 ms to run SPI transactions against a chip that just failed to configure - on a bus the second FD chip shares whenever `MCP2517_BUS() == MCP2517_BUS2()`. Gating the top half on a "driver usable" flag puts a branch in the ISR and still leaves the task and the asserted nINT alone.

**One of the premises above is now made to fail loudly rather than silently.** `end()` runs its whole body - including `vTaskDelete(mESP32TaskHandle)` - between the library's `turnOffInterrupts()` and `turnOnInterrupts()`. On ESP32 those are `taskDISABLE_INTERRUPTS()` / `taskENABLE_INTERRUPTS()` **unless** `DISABLEMCP2517FDCOMPAT` is defined, and `ACAN2517FD.h:26` defines it unconditionally, so today the bracket is empty and the task deletion runs with interrupts on. But that macro is the library's own MCP2517FD compatibility switch, described in its comment as a performance option for the MCP2518FD - exactly the kind of thing someone turns back on the day a board with a real MCP2517FD appears, at which point every CAN-FD init failure deletes a FreeRTOS task with interrupts masked. `comm_can.cpp` therefore carries an `#ifndef DISABLEMCP2517FDCOMPAT` / `#error` immediately after the library include, so that change stops the build with the reason attached instead of quietly re-arming the hazard. The define is not only about `end()`: it empties the library's interrupt bracket everywhere the library uses it, on the transmit, receive and register paths as well, and the error message says so - whoever removes the define answers for all of them. The guard is in the consumer and not in the library header deliberately: the define is the vendored library's to make, and this file is the one relying on it.

The vendored library is deliberately **not** touched. Its contract - return a code, leave the caller to tear down - is defensible; the caller's assumption that a non-zero code means nothing was started is what is wrong.

**Scope.** The MCP2515 path in the same file has the same *shape* - `MCP2515_Lite::begin()` attaches at `mcp2515_lite.cpp:145` before a `reset()` that can return false at 149, and `comm_can.cpp` then sets `can2515 = nullptr` - but it is **not** the same defect and is not changed here. It attaches with `attachInterruptArg(..., mcp2515_isr_handler, this, FALLING)`, so the ISR receives the object pointer as its argument rather than reading the global; the object is leaked, not freed, and the handler guards on `instance->_can_task_handle`, which has an in-class `= nullptr`. A live handler on a dead chip, but nothing to dereference through.

**The tests read source rather than run it.** `comm_can.cpp` is outside the host suite: `test/CMakeLists.txt` keeps a deliberately curated firmware-source list whose own comment says it "is not every file under `Software/src`", and reaching this function means bringing in ACAN2517FD, ACAN_ESP32, mcp2515_lite, the SD card and the CAN streaming webserver against an `emul/` layer whose FreeRTOS shim is 35 lines. That remains its own piece of work. What the fix *is*, however, is a placement, and a placement can be pinned as text - which is the pattern this tree already uses for the OTA confirmation marks in `Software.cpp`. `test/canfd_init_teardown_tests.cpp` asserts, for each chip, that the failure path clears the pointer, tears down first, and names the driver it is abandoning rather than its sibling. Over the vendored library, it asserts that a code is still set inside the block which attaches the handler, and that the polling task is still started without regard to it. For the guard, it asserts that `comm_can.cpp` refuses to build without the compatibility switch, that the library's interrupt wrappers still honour the switch, and that `end()` still deletes its task inside that bracket. The library-side assertions are the premises this change depends on and cannot see: a library bump that makes one of them false turns the teardown into dead code, or re-arms the masked-interrupt hazard, silently - and these fail loudly instead. The shared scanning helpers move to `test/source_scan.h`; they strip comments before matching, without which the comment introduced above - which names `end()` - would satisfy a search for a call that had been commented out.

**Reachability is argued from the source, not measured.** Nothing here has watched an MCP2517FD miss its mode change on a bench board; the mode-request timeout is a real code path with a real 10 ms budget, but how often it fires in the field is unknown. A forcing run - make the requested-mode wait always time out, flash before and after - has not been done.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**CAN replay: a replay aimed at an interface that cannot reach any wire looked exactly like one that works**
Branch [`replay-unreachable-interface`](https://github.com/ekholm/Battery-Emulator/tree/replay-unreachable-interface) @ `163f2c2e` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:replay-unreachable-interface)
A CAN replay pointed at a nonexistent or dead interface reported exactly what a working replay reports: every page said the file was playing, and nothing said the frames went nowhere. Three holes let it through. `/setCANInterface` stored any integer it was given - and the settings page and the replay number the MCP2515 add-on differently, so a reasonable "5" selects no interface at all. `/startReplay` trusted the stored value. And the transmit path's `default:` branch dropped frames without the event its own comment asked for. Both routes now validate through one tested helper, and an unreachable interface raises `EVENT_CAN_INTERFACE_UNAVAILABLE`.

<details>
<summary>PR body it would ship with</summary>

A CAN replay pointed at a nonexistent or dead interface reports exactly what a working replay reports. Every page says the file is playing. Nothing says the frames went nowhere. On a bench that is the worst possible failure: the probe runs, the numbers come back, and they are the numbers of a test that never touched a wire.

Three separate holes let it happen, and each is small enough to have been missed on its own.

**1. `/setCANInterface` stored whatever integer it was given.** The route wrote the value straight to settings. That is bad by itself, and there is a specific trap waiting behind it: **the two enumerations disagree.**

    settings dialect:  5 = MCP2515 add-on
    replay enum:       5 = NO_CAN_INTERFACE,  2 = MCP2515 add-on

So "5" is a perfectly reasonable thing for someone reading the settings page to type, and it selects *no interface at all*. The route now validates through a pure helper, and the rejection text names the dialect clash rather than saying "invalid" - whoever typed 5 finds out which numbering they were reading.

**2. `/startReplay` trusted the stored value.** Even with the route fixed, the stored interface can be wrong: it may predate the validation, and a registered receiver whose `begin()` failed leaves a null driver behind. So `/startReplay` re-validates what it is about to use. Without that, frames land in nothing and the only eventual symptom is a buffer-full count.

**3. `transmit_can_frame_to_interface()`'s `default:` branch was a silent no-op.** Its own comment asked for an event and none was raised. It now raises `EVENT_CAN_INTERFACE_UNAVAILABLE` with the interface number as the event data.

**The change** is one commit, nine files:

- `Software/src/devboard/webserver/can_replay_validation.{h,cpp}` - a new pure helper (46 lines) that answers "can this interface reach a wire?"
- `Software/src/devboard/webserver/webserver.cpp` - both routes call it.
- `Software/src/communication/can/comm_can.{cpp,h}` - the `default:` branch raises the event, and `can_interface_ready()` answers the readiness question in one place.
- `Software/src/devboard/utils/events.{h,cpp}` - the new event.
- `test/can_replay_validation_tests.cpp` + `test/CMakeLists.txt` - the helper's tests.

**The new event is appended, and that is deliberate.** `EVENT_CAN_INTERFACE_UNAVAILABLE` sits immediately before the `EVENT_NOF_EVENTS` sentinel rather than beside the CAN events it belongs with. **Event ordinals are a wire format**: `espnow.cpp` publishes the enum value as a u16 field and `espnow.h` documents it as such. An event inserted mid-enum renumbers every event after it, and a peer running a different build then decodes the wrong one. The sentinel is a count, not a wire value, so it may move. Appending costs nothing here and avoids the skew entirely. It is worth stating explicitly, because the natural instinct is to file a new event with its relatives - and doing that once would silently break every ESP-NOW peer on an older build.

**A failed native re-init now clears the flag readiness is read from.** `change_can_speed(CAN_NATIVE)` re-runs the native init and used to leave the readiness flag standing when that init failed. Readiness then kept answering "yes" for a driver that had just died - which is precisely the answer the mid-replay validation above depends on. The native bus-error flags still fired, so the board was not silent; the readiness answer was simply wrong.

**Testing, and what it does not cover.** `test/can_replay_validation_tests.cpp` exercises the helper's decision table directly, including the 5-vs-2 dialect case in both dialects. **No host test reaches the driver stack**, and that is why the helper takes readiness as an **injected seam** rather than calling into it: the decision is testable, the wiring from that decision to a live driver is not, and making it so would mean pulling the whole CAN stack into `test/CMakeLists.txt`'s curated source list. Stated here rather than left for a reader to discover.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**CAN: the BECom's second CAN-FD interface does not work unless the first one is also configured**
Branch [`fix/becom-fd2-standalone`](https://github.com/ekholm/Battery-Emulator/tree/fix/becom-fd2-standalone) @ `2433a59b` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:fix/becom-fd2-standalone)
The BECom's second MCP2518 is clocked from the first chip's CLKO output, and that divider is only programmed inside the first interface's `begin()`; the chip powers up dividing by 10, so the second interface alone ran at 4 MHz instead of 40 and failed to start. It now programs the first chip's oscillator register itself when it starts alone. A reviewer suggested reusing the first interface's init; that would start an interface nobody configured just for its clock output, so the one register write is done on its own. Host-tested only - no BECom was available on a bench.

<details>
<summary>PR body it would ship with</summary>

The BECom's second MCP2518 is clocked from the first chip's CLKO output
(hw_becom.h declares CLKODIV divide-by-1). That divider is programmed by
the first FD interface's driver during begin(), and the chip powers up
with CLKO at divide-by-10 - so configuring only the second interface
left the second chip running at 4 MHz while the driver assumed 40 MHz,
and it failed to initialize.

When the second FD interface starts without the first one, program the
first chip's OSC register directly (reset, then one register write) so
its CLKO carries the frequency the board declares. Starting the first
driver just to get its clock output would bring up an interface nobody
configured, so the one register write is done on its own. Chip 1 runs
from its own crystal - CLKODIV divides only the output - so the write is
not constrained by the second chip's divided clock; it uses a slow,
universally legal SPI speed because a one-off transaction needs no
throughput. Boards with the default divider (stark, lilygo2can second
chips have their own clocks) are unaffected.

Host tests pin the gate condition and the exact SPI command sequence.

Note: drafted with AI assistance, reviewed by me.

</details>

## Battery drivers

---
**Triple battery: the predicate and the switch agree again, and the invariant is now a test**
Branch [`battery-instance-support-parity`](https://github.com/ekholm/Battery-Emulator/tree/battery-instance-support-parity) @ `2fc664ff` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:battery-instance-support-parity)
`battery_supports_triple()` listed five types while the battery3 construction switch had six cases - `CmpSmartCar`'s case was unreachable, because the guard rejected the type before the switch could reach it. The resolution declares the CMP Smart Car triple-capable rather than deleting the dead case, and - the durable half - adds the test the file's own comment has always asked for: both label sets extracted and asserted equal, fallthrough-aware, so the "must match the switch in setup_battery() below" invariant fails a build instead of relying on a comment.

<details>
<summary>PR body it would ship with</summary>

`battery_supports_triple()` and the battery3 construction switch in `setup_battery()` are two lists of battery types that must agree - the file says so in a comment, "Must match the switch in setup_battery() below." They had drifted: `CmpSmartCar` has a complete, well-formed battery3 construction case and was absent from the predicate. The guard above the switch rejects the configuration before the switch can reach it, so the case is dead code and the UI never offers the option. The predicate was the side that was wrong: the driver takes a datalayer pointer in its multi-battery constructor, writes nothing to file-scope or `datalayer_extended` state, and is already declared and built for a second battery.

`CmpSmartCar` is added to `battery_supports_triple()`, making the two lists agree.

The durable half is the test that the file's comment has always asked for but that a comment cannot enforce. The invariant has two directions and they need different instruments, which is worth stating because it is not obvious:

- Predicate true, switch cannot build: the guard passes and `setup_battery()` silently does nothing. Caught by running `setup_battery()` over every battery type and comparing the predicate against whether an instance appeared.
- Switch can build, predicate false: dead code behind the predicate's own guard - not reachable by running anything. That is the direction that had drifted. The source is the only instrument, so the third test reads BATTERIES.cpp and compares the case labels in the predicate switch against the case labels in the construction switch.

Both directions are mutation-tested: removing `CmpSmartCar` from the predicate fails the source-reading test; declaring a type with no construction case fails the runtime test. Three test cases cover the double predicate, the triple predicate, and the dead-code direction.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Tesla: two advanced-page fields that report the wrong thing**
Branch [`tesla-page-meaning`](https://github.com/ekholm/Battery-Emulator/tree/tesla-page-meaning) @ `dab355e3` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:tesla-page-meaning) · includes the lookup-bounds fix beneath it
`BMS_hvacPowerBudget` is a 10-bit field whose top nibble sits in byte 7, but byte 7 was read unmasked, so the neighbouring `BMS_inverterTQF` bits were folded into the power budget and it could report far more than its 1023 maximum. (The other field this entry used to name, `HVP_currentSenseMia`'s two-bit mask, was fixed upstream before `v12.6.0` and is no longer part of the branch.) Four PCS retry counters (3- and 4-bit) were rendered through a two-entry False/True table, so one retry read `True` and higher counts had no meaning; they now render as the numbers their labels ("Rty Cnt") always promised. Beneath it, the contained bounds fix: every lookup table on the page is bounded, an out-of-range value is named (`UNKNOWN(n)`) instead of dereferenced, and the emul String is null-guarded where Arduino's guards.

<details>
<summary>PR body it would ship with</summary>

Two fields on the advanced page reported the wrong thing, and the bounds fix beneath them stops the class from recurring.

**BMS_hvacPowerBudget.** The signal is 10 bits starting at bit 50: byte 6 supplies bits 50-55, and byte 7 only its low nibble (bits 56-59). The code read byte 7 unmasked. Byte 7's bits 4-5 are `BMS_inverterTQF`, a different signal parsed four lines below, so the power budget absorbed that signal and a 10-bit field could report far more than 1023. The display line for this field was already commented out with "Not giving useable data" - the value was seen to be wrong and the line removed rather than the cause found. The parse is now corrected; the display stays commented, since re-enabling it is a judgement call for someone with the car.

**PCS retry counters.** Four retry counter fields (3-bit and 4-bit, lifted straight from `0x224`) were rendered through a two-entry `falseTrue[]` table. One retry read "True"; counts above 1 indexed past the end of the table. The fields' own labels already said "Rty Cnt". They now render the number. This deliberately changes what 0 and 1 display - "0" and "1" rather than "False" and "True" - because a count is not a boolean, and the only reading that was ever true is how many retries there were. `falseTrue[]` has no remaining users and is removed.

**Lookup bounds.** All 14 static tables in the renderer are indexed by raw bit-slice values from CAN frames, and 10 of those slices are wider than the table they select. The widest gap was `PCS_dcdcSubState`: a 5-bit field indexing an 18-entry table, so any sub-state code from 18 upward dereferenced past the array. A `lookupName()` template takes each table by reference (bound from the array's own type, not a constant) and renders `UNKNOWN(n)` for an out-of-range code. All 40 lookups, including the commented-out ones, route through it.

The host `String` emul is also null-guarded in its `const char*` constructor and `operator+=`, matching Arduino's behavior. Without this the host renderer crashed on char* fields that stay null until a CAN frame fills them; the firmware renders them as empty strings.

19 tests in `test/tesla_html_bounds_tests.cpp` and `test/emul_string_null_tests.cpp`: the power-budget parser case (neighbouring flag set alone, field's own maximum pinned at 1023), one case per table with a reachable overrun, the exactly-covered tables still rendering their labels at the top code, the retry counters rendering numbers including 0 and 1, and the null-guard emul behavior. Not run on hardware.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Tesla: a second battery stops corrupting the first battery's page**
Branch [`tesla-instance-parity`](https://github.com/ekholm/Battery-Emulator/tree/tesla-instance-parity) @ `7241dc10` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:tesla-instance-parity)
`TESLA-BATTERY.cpp` wrote the shared `datalayer_extended.tesla` struct from every instance - 478 sites, both constructors - so a double-Tesla setup interleaved two packs into one advanced page. Each instance now carries its own extended-struct pointer, set at construction and null for the second battery: the pattern ECMP and Renault Zoe Gen2 already use. The hoisted UDS part-number trigger is covered, and both ends of the guarded extended block are pinned by test.

<details>
<summary>PR body it would ship with</summary>

There is exactly one `datalayer_extended.tesla`, and both `TeslaBattery` constructors published into it from every instance. On a double-Tesla setup that means two packs interleaving their values into the first pack's advanced page - with nothing on the page to indicate whose numbers they are.

`TeslaBattery` now carries a `DATALAYER_INFO_TESLA*` set at construction: the struct's address for the main instance, null for any additional instance. This is the pattern ECMP and Renault Zoe Gen2 already use. The 478 extended writes in `update_values()` occupy one contiguous region; a single `if (datalayer_tesla)` guards them all. Two things that live inside that region are hoisted above the guard so they keep running for every instance: the UDS part-number query trigger, which drives a request on the wire, and the pack's own energy counters, which are addressed through the per-instance `datalayer_battery` pointer rather than through the extended struct.

`TeslaHtmlRenderer` takes the same pointer. An instance with no extended struct renders a brief "no extended data" section rather than the other pack's numbers. `renders_own_battery_data()` stays false for pack 2, keeping its existing "limited to the Main Battery" notice; the null path makes that a deliberate choice rather than the only safe option.

The host `String` emulation now null-guards its `const char*` constructor and `operator+=`, matching Arduino's behavior. Some Tesla char* fields remain null until a CAN frame fills them; the firmware renders them as empty strings, but the previous `std::string` path threw. This was needed to run the Tesla HTML renderer in the host test binary at all.

Eight tests in `test/tesla_instance_isolation_tests.cpp`: a positive control confirming the main instance still publishes (so "second instance wrote nothing" cannot pass vacuously), and cases pinning that a second instance leaves the struct untouched - including the UDS handshake path, which must still fire for every instance even though the extended struct is not written.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**TESLA-LEGACY: state of health is measured against an 85 kWh pack, so a larger pack reports over 100 %**
Branch [`tesla-legacy-soh-clamp`](https://github.com/ekholm/Battery-Emulator/tree/tesla-legacy-soh-clamp) @ `ff6496fd` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:tesla-legacy-soh-clamp)
`TESLA-LEGACY-BATTERY.cpp` computes state of health as the measured minimum CAC over 231.6 Ah, the CAC-at-new of an 85 kWh pack and the only reference in the driver, so every other pack size reports a wrong SOH and a larger one reports more than 100 %, which the inverter protocols publish. An owner of a 100 kWh legacy pack on #2673 has a page reading SOH 113.94 %. This clamps the published value to 100 %. It is deliberately half a fix: a smaller pack still under-reports, because the per-size CAC-at-new figures are not in the tree and are not guessed here. Pairs with the 100 kWh capacity entry: that group is both mis-sized and over-reported.

<details>
<summary>PR body it would ship with</summary>

State of health is computed as

```cpp
datalayer.battery.status.soh_pptt = BMS_CAC_min / 2316;  // uitgelezen minimale CAC / CAC bij nieuw (231,6 Ah)
```

231.6 Ah is the CAC-at-new of an 85 kWh pack and the only CAC reference in the driver. The hardware-ID switch just above it selects a capacity per pack group but no CAC, so every other size reports SOH scaled by its own CAC-at-new over 231.6 Ah: a healthy 60 kWh pack reads well below 100 %, and a 100 kWh pack reads above it.

CACmin arrives on `0x7E2` as a 12-bit field, so the largest value the bus can produce is 4095 × 10000 / 2316 = 17681 pptt, 176.81 %. That still fits `uint16_t`, which is why nothing downstream catches it, and the value leaves the box: PYLON packs `soh_pptt / 100` into one byte of a percent field, and BYD-CAN and SMA-BYD-H-CAN send the raw 16-bit value. The owner of a 100 kWh legacy pack on #2673 has a status page reading **SOH: 113.94 %**.

This was raised when the constant went in. On #1946 a reviewer asked whether 231 Ah should be hard-coded; the contributor answered that it was their own 85 kWh pack's figure and that the hardware ID ought to select the CAC, and the maintainer's reply on that thread, "But I think we need some CAC variable also?", is still open on this line.

The change clamps the quotient to 10000 pptt at the assignment, so no pack reports more than full health. It is half a fix and says so in the code: a smaller pack under-reports for the same missing table, and no clamp can help it. The per-size CAC-at-new figures are not in the tree and are not invented here.

Host tests drive the real receive path (`0x7E2` frame, `update_values()`, datalayer): the reference pack at exactly 100 %, a worn pack passed through unclamped, a larger pack and the largest encodable reading clamped, one count above the reference already clamped, a smaller pack still under-reporting (so the limitation is asserted, not only written down), the `0x7E2` byte-0 gate, the driver's starting reading, and the clamp acting on a decoded reading rather than on that starting value.
</details>

---

**BYD-CAN: the brand filter tested one byte and stored another**
Branch [`byd-brand-filter`](https://github.com/ekholm/Battery-Emulator/tree/byd-brand-filter) @ `e9fa7e92` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:byd-brand-filter)
The inverter-name filter had two independent defects that composed into "never correct on any input": both comparisons were `>` (so the printable range it was written to accept was exactly what it rejected), and the guard tested `u8[i]` while the body stored `u8[i + 1]`. Both fixed, and the review added the half a fix alone would have missed: a rejected byte clears its slot rather than leaving the previous scan's character behind. This deliberately does not decide the byte-0 mux question - see [FINDINGS.md](FINDINGS.md) - it makes the current reading self-consistent.

<details>
<summary>PR body it would ship with</summary>

The 0x151 identification branch that populates `inverter_brand` had two independent defects:

```cpp
if ((rx_frame.data.u8[i] > 0x40) && (rx_frame.data.u8[i] > 0x7B))  //Filter out invalid chars
    inverter_brand[i] = rx_frame.data.u8[i + 1];
```

**Both comparisons are `>`.** The condition collapses to `> 0x7B`, admitting exactly the bytes the comment calls invalid (above `z`) and rejecting the entire printable `A`..`z` range it was written to accept.

**The guard reads `u8[i]` while the copy takes `u8[i + 1]`.** Byte 0 carries the identification-request flag, so the brand string lives in bytes 1..7 and the copy's offset is right. It was the test that needed to move with the copy, not the other way around.

Together they meant `inverter_brand` was never once correct for a printable brand name: the string was populated only from bytes above 0x7B, shifted by one - almost never anything real.

The review found the issue the first two fixes alone would have missed: a rejected byte left its slot untouched. The destination is never zeroed between frames, so "GoodWes" followed by "SMA" produced "SMAdWes" - a brand string that was never on the wire. That was unreachable while the broken filter accepted almost nothing, and became reachable the moment the filter started working, so it is fixed in the same change.

The fix reads the byte once and writes either the character or `'\0'`:

```cpp
const uint8_t c = rx_frame.data.u8[i + 1];
inverter_brand[i] = ((c > 0x40) && (c < 0x7B)) ? c : '\0';
```

Characterization tests cover: the name stored in order, both range bounds and the bytes immediately outside them, the high bytes the broken filter uniquely accepted, an invalid byte truncating rather than shifting the rest, a request frame not populating the name, and a shorter name following a longer one not inheriting its tail.

The inherited range `A`..`z` still rejects digits and spaces. That is pre-existing and not changed here - widening it is a separate decision - but it is now pinned by a test rather than waiting to be discovered against a real inverter.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Safety events that could never fire**
Branch [`driver-dead-safety-events`](https://github.com/ekholm/Battery-Emulator/tree/driver-dead-safety-events) @ `ced4d411` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-dead-safety-events)
Two drivers raised safety events on conditions that could not occur: the Kia/Hyundai HYBRID's interlock decode had a cast-precedence error, so `EVENT_HVIL_FAILURE` never fired (now fires and clears on `0x5AE`), and CHARGEBYTE's error ladder was ordered so an error while charging never reported `BMS_FAULT` (reordered). One decision stated openly: the E-GMP water-sensor check was dead - the member is initialised to 164 ("no water") and no E-GMP RX path ever writes it, so the event could never fire and the page rendered a constant. The sibling KIA-64 driver decodes the same sensor for real (`u8[3]` of its poll response, 164 = dry), so this was a copied pattern that never got its decode wired. Removed rather than guessed at; one E-GMP trace naming the byte restores it with the KIA-64 decode as the template.

<details>
<summary>PR body it would ship with</summary>

Three drivers raised safety events on conditions that could not occur, and one dead check is recorded rather than guessed at.

**KIA/Hyundai HYBRID - HVIL interlock** (`KIA-HYUNDAI-HYBRID-BATTERY.cpp`): the interlock decode read

```cpp
interlock_missing = (bool)(rx_frame.data.u8[1] & 0x02) >> 1;
```

The `(bool)` cast bound before the shift: `(bool)(u8[1] & 0x02)` is 0 or 1, and `1 >> 1` is 0, so `interlock_missing` was always 0 on every `0x5AE` frame and `EVENT_HVIL_FAILURE` was unreachable. Removing the cast lets the shift happen on the raw masked byte, so bit 1 is tested correctly. The event now fires and clears through `0x5AE`.

**CHARGEBYTE-CCS - BMS fault while charging** (`CHARGEBYTE-CCS.cpp`): the status ladder put `inCharge` before the error flags, so an error raised while charging reported `BMS_ACTIVE` and `BMS_FAULT` was unreachable in exactly the state where a fault matters most. Errors now rank first:

```cpp
if (hasLowLevelError || hasChargebyteError)
  datalayer.battery.status.real_bms_status = BMS_FAULT;
else if (inPrecharge)
  ...
```

**KIA E-GMP - water ingress sensor** (`KIA-E-GMP-BATTERY.cpp`): `waterleakageSensor` was initialised to 164 ("no water") and no E-GMP RX path ever wrote it. The ingress check compared a constant, and the advanced page rendered that constant as if it were a live reading. The member, its getter, the dead check, and the misleading page line are removed. The decision is stated openly: the KIA-64 sibling decodes the same sensor from `u8[3]` of its `0x5D5` poll response (164 = dry), so the E-GMP decode has a template when a real CAN trace names the byte. Nothing is invented here.

The fourth family gap - `EVENT_12V_LOW` on HYBRID - is not added and the reason is on record: no HYBRID frame set decodes a 12 V byte, and the sibling's `0x596` frame does not exist there. A trace or PID documentation is needed first.

Five tests in `test/battery/dead_safety_events_tests.cpp`, each confirmed to fail against its defect before the fix: the HVIL event firing on a set interlock bit; the HVIL event clearing on a cleared bit; `BMS_FAULT` when charging with an error flag set; `BMS_ACTIVE` when charging cleanly; and a text check that the waterleakage line stays absent from the E-GMP status page.

Not reproduced on hardware. These are host tests over the receive paths and status logic.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Decode arithmetic: four values fixed, three pinned**
Branch [`driver-decode-arithmetic`](https://github.com/ekholm/Battery-Emulator/tree/driver-decode-arithmetic) @ `aae8fc10` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-decode-arithmetic)
Range Rover PHEV's 24-bit current read against the driver's own declared range; IMIEV had swapped channels, mV rounding loss, and - found during testing - uninitialised 88-entry instance arrays publishing heap reads until every sensor reports; RELION-LV's minimum temperature was decoded but never wired. ENNOID-BMS put its only temperature in the minimum field, one degree above the maximum, and in whole degrees against deci-degree fields. **TESLA-LEGACY's wrapped subzero brick temperatures are no longer part of this: you fixed them independently in `a0687ce98`, with the same change this branch carried - `battery_BrickModelTMax/Min` widened to `int16_t`, the decode's own range being -40..+87.5 C.** What is left here for that driver is the reason, as a comment beside the declaration, and a test - which no longer fails before, and is kept only because nothing else pins the sign of that decode. Two further suspicions are pinned as correct-as-is by characterization tests with the evidence named, so the next reader does not re-litigate them. A third pin, TESLA-LEGACY's 100 kWh hardware-ID group, was removed once an owner's page settled it; that one you have since fixed too, and what remains of it is a test-only entry in [FEATURES.md](FEATURES.md).

<details>
<summary>PR body it would ship with</summary>

Four decode fixes, each with named evidence, and three pins where the evidence was not strong enough to change anything. The principle is stated once and applied throughout: a withdrawn BMW-PHEV DTC claim is the cautionary case for changing something you cannot settle.

**RANGE-ROVER-PHEV - 24-bit current assembly** (`RANGE-ROVER-PHEV-BATTERY.cpp`): the driver's own declaration says the field spans 0 - 16,777,215 with offset -209,715.175 - a range whose midpoint only a 24-bit assembly reaches. The old code shifted both high bytes by 8:

```cpp
CurrentExt = ((rx_frame.data.u8[5] << 8) | (rx_frame.data.u8[6] << 8) | rx_frame.data.u8[7]);
```

Bytes 5 and 6 collided in one lane, leaving the value as a 16-bit number. Fixed to `<< 16` for byte 5, `<< 8` for byte 6.

**IMIEV/C-Zero/Ion - temperature channels and voltage rounding** (`IMIEV-CZERO-ION-BATTERY.cpp`): temperature channels 2 and 3 guarded on `u8[2]` and `u8[3]` but read `u8[1]`, mirroring channel 1 on every frame - the guards themselves name the intended bytes. Cell voltages were truncated: `3.7f * 1000` is `3699.99...` in binary, and the cast published 3,699 mV for a cell the decode meant as 3,700. Changed to `lroundf`. The two 88-entry instance arrays (`cell_voltages`, `cell_temperatures`) were also uninitialised heap reads until every sensor reported; zero-initialised here. (The uninit fix also appears in `driver-uninit-sweep`; on this branch it travels with the decode corrections.)

**RELION-LV - minimum temperature never wired** (`RELION-LV-BATTERY.cpp`): `min_cell_temperature` was decoded and then never used - both datalayer fields carried the maximum. Evidence: the case's own captured frame `47 01 01 47 01 01 00 00` reads as two (value, id, id) triplets, the same shape the driver uses for cell voltages. `u8[2]` is an id byte (0x01 = -49 C constant); `u8[3]` is the second value byte (0x47 = 21 C). Min is now wired from `u8[3]`.

**ENNOID-BMS - temperature unit and ordering** (`ENNOID-BMS.cpp`): the single reported temperature landed in the MIN field one degree colder than the max, both in raw degrees Celsius against deci-Celsius fields. A 21 C pack displayed as 2.1 C with max < min. The author's synthetic 1-degree spread is kept, correctly ordered and scaled by 10.

**TESLA-LEGACY brick temperatures - overtaken by upstream**: subzero brick temperatures wrapped through `uint8_t` intermediates while the decode range is -40 to +87.5 C. Upstream reached the identical fix independently (a0687ce98: `battery_BrickModelTMax/Min` widened to `int16_t`), so there is nothing left to fix. What remains is a comment beside the declaration explaining why the sign matters, and one test that pins the subzero case - not because it fails, but because nothing else pins the sign of that decode.

**Pinned as correct-as-is** (characterisation tests with the evidence named): VOLVO-SPA-HYBRID `0x369` low-byte source is self-referential, and the sibling driver has the identical expression - no sibling settles the layout, and the field is display-only; RELION-LV `0x264` discharge current reads the regen bytes - its only consumer is commented out, and no trace names the discharge bytes.

Nine tests in `test/battery/DecodeArithmeticTests.cpp`. Each fix test is confirmed to fail against its defect before the fix; each pin test reflects the evidence stated at the site.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Family consistency: four fixes where siblings already agree**
Branch [`driver-family-consistency`](https://github.com/ekholm/Battery-Emulator/tree/driver-family-consistency) @ `6c59fefc` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-family-consistency)
FERROAMP now honours user voltage limits like PYLON and SOLXPOW already do (the siblings' 2.0 V offset deliberately not imported); GROWATT-WIT's capacity guard goes `> 0` to `> 10`, ending a 50,000 dAh fiction from a 0.5 V startup reading; MG-5's `MG5_USE_FULL_CAPACITY` branch - defined nowhere - is deleted; SOFAR's consent reads `reported_soc` instead of `spoofed_soc`; FERROAMP's swapped charge/discharge byte labels corrected, values unchanged.

<details>
<summary>PR body it would ship with</summary>

Four drivers brought back to what their family or their own comments promise. Each was verified at the source before changing anything.

**FERROAMP-CAN - user voltage limits not honoured** (`inverter/FERROAMP-CAN.cpp`): the `0x4221` frame always sent the raw design limits for charge and discharge cutoff voltages. The `0x4200` siblings PYLON and SOLXPOW already check `user_set_voltage_limits_active` and substitute the user-tightened window when it is active; FERROAMP did not, so a user-configured narrow window reached the emulator's own logic but not the inverter. Now follows the same pattern. The siblings carry a +/-2.0 V offset that this driver never had; that offset is deliberately not imported - only the voltage selection is aligned, not the offset convention.

The same file had swapped byte comments on `0x4221`: charge and discharge labels were exchanged. The values were always correct; only the comments were wrong. Corrected to match the slot semantics the family uses.

**GROWATT-WIT - capacity guard** (`inverter/GROWATT-WIT-CAN.cpp`): the rated-capacity calculation guarded `voltage_dV > 0`, which let a 0.5 V startup reading turn 30 kWh into a clamped 50,000 dAh fiction that went out on the wire. The HV and LV siblings both guard at `> 10` (1.0 V). Changed to match. The accompanying comment claimed the clamp allowed up to 65,535 while the code clamped to 50,000; the comment now says what the code does.

**MG-5 - dead `MG5_USE_FULL_CAPACITY` branch** (`battery/MG-5-BATTERY.cpp`): the macro was defined nowhere, so the voltage-extended SoC rescale was compiled out of every build that ever shipped. Deleted rather than activated - its 4.1 V / 369 V / 92 % constants need a real-pack trace to validate. Deleting the branch orphaned a `cellVoltageValidTime` timer that was set and decremented but never read; removed with it, which also resolves the per-call-vs-per-second question the triage had noted about that timer.

**SOFAR-CAN - charge consent follows the display spoof** (`inverter/SOFAR-CAN.cpp`): the charge/discharge consent gate used `spoofed_soc`, whose 99 % display cap made the discharge-only branch unreachable and its claimed hysteresis fiction. A truly full pack now revokes charge consent while the display still reads 99 %. Changed to `datalayer.battery.status.reported_soc`.

Eight tests in `test/family_consistency_tests.cpp` pin the four behaviours. Each fix-reverted mutation fails its named test.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Uninitialised driver arrays: the four that are live**
Branch [`driver-uninit-sweep`](https://github.com/ekholm/Battery-Emulator/tree/driver-uninit-sweep) @ `51ef941a` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-uninit-sweep) · includes the memory-safety fixes beneath it
A sweep sorted ten suspect arrays by liveness: four are whole-array memcpys into the datalayer reachable before frames fill them - BOLT-AMPERA, HYUNDAI-IONIQ-28, KIA-HYUNDAI-64 (whose `<300` filter passes high garbage), SANTA-FE-PHEV. All four get `= {0}` plus a poisoned default-init test through their own publish path; the six that are written-before-read are commented at the declaration instead of churned. Beneath it, the memory-safety commits it includes: an ORION out-of-range cell id is rejected rather than clamped (a corrupted id must neither overwrite a real cell nor inflate the detected-cell count), and explicit zero-init where a user-provided constructor defeats value-initialisation.

<details>
<summary>PR body it would ship with</summary>

This branch stacks two commits: the base commit fixes an out-of-bounds cell write and three uninitialised cell arrays (ORION, ECMP, IMIEV); the top commit zero-initialises the four additional live uninitialised arrays the sweep found when it sorted all ten suspects by liveness.

**Base commit - out-of-bounds write and three arrays**

ORION-BMS (`ORION-BMS.cpp`): an over-range cell id was clamped to `MAX_AMOUNT_CELLS` and then used as a write index - one past the end of `cellvoltages[MAX_AMOUNT_CELLS]`. The recorded decision is to reject the frame rather than clamp to the last cell: a corrupted or mis-configured id must neither overwrite a real cell's reading nor drive the detected-cell count to a full pack.

ECMP, IMIEV, and ORION cell arrays were uninitialised instance members that were memcpy'd or min/max-scanned into the datalayer before any frame filled them. ECMP had this live - its user-provided constructors defeat the value-initialisation that `new T()` would otherwise perform for types without them. IMIEV and ORION happened to be safe today for the same reason, and that protection is exactly one added constructor away from vanishing. All four arrays get explicit `= {0}` initialisers.

**Top commit - four more live arrays**

A sweep of eight further drivers with the same pattern (user-provided constructor + uninitialised member array) sorted them by whether the array is a whole-array memcpy destination reachable before frames fill it:

Live, fixed with `= {0}` and a poisoned-init test each: BOLT-AMPERA `cellblock_voltage[96]` (`update_values()` memcpys all 96 unconditionally), HYUNDAI-IONIQ-28 `cellvoltages_mv[96]` (same unconditional memcpy), KIA-HYUNDAI-64 `cellvoltages_mv[98]` (the POLL_GROUP_5 row memcpys all 98, and its `<300` filter passes high garbage - 0x4242 = 16,962 mV), SANTA-FE-PHEV `cellvoltages_mv[96]` (the poll-complete memcpy publishes all 96).

Written-before-read or read-nowhere, commented at the declaration rather than initialised: BMW-I3 `message_data`, BMW-iX `UDS_buffer`, BOLT-AMPERA `battery_cell_voltages`, MEB `cellvoltages_polled` and `battery_serialnumber`, UdsCanBattery `seq_msg.data`.

**Why the mutation evidence is stated rather than hidden**: removing the BOLT initialiser fails its test in a Release host build. Removing the other three does not - GCC's store-merging turns the neighbouring zero NSDMIs into a block clear that zeroes the gap array as collateral. At -O0 all three removals fail (0x42 poison surfaces verbatim), so the hazard is real wherever an optimizer or flag set stops covering it. The initialisers convert both accidents into a guarantee.

The poisoned-init tests use placement-new over a buffer filled with 0x42 (not 0xAB: the obvious poison reads back as a float that truncates to zero, which is the value the test is trying to prove is not an accident).

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Shunt: three values that corrupt instead of going missing**
Branch [`driver-signedness-clamps`](https://github.com/ekholm/Battery-Emulator/tree/driver-signedness-clamps) @ `7a1a945d` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:driver-signedness-clamps) · stacked under `sbox-average-divisor`
Three driver defects with the same shape: a signedness or width error that turns a real measurement into a plausible wrong number. The main one: `datalayer.shunt.measured_amperage_dA` was `uint16_t`, so every discharge current wrapped - a -50 A discharge read as ~65,036 dA. Now `int16_t`, matching `battery.status.current_dA`, the same quantity and unit already signed in-tree. The audit behind it found the field has two writers and zero in-tree readers, which is what makes the root fix safe to take first. Also: BMW-SBOX's rolling-average members go signed (`avg_mA_array`, `avg_sum` - the division then signs itself), and the same change zero-initialises them and pins that.

<details>
<summary>PR body it would ship with</summary>

Three defects that corrupt a value instead of losing it - a signed quantity carried unsigned in two places, and a clamp that assigns its limit to the wrong variable. All three are silent: nothing is logged, nothing is refused, the wrong number simply goes out on the wire.

**`datalayer.shunt.measured_amperage_dA` was `uint16_t`** (`datalayer/datalayer.h`, `shunt/BMW-SBOX.cpp`): every discharge current wrapped. The field is derived from `measured_amperage_mA`, which is `int32_t`; dividing a negative milliamp value by 100 and storing it in `uint16_t` turns -50 A (= -500 dA) into roughly 65,036. The field becomes `int16_t`, matching `battery.status.current_dA` - the same physical quantity in the same unit, already signed in the tree.

The audit behind this change matters: the field has exactly two writers (BMW-SBOX and BYD-CAN in shunt mode) and zero in-tree readers. Widening the type cannot change any comparison anywhere; that is the reason this fix is safe to take independently, before the field has a reader.

**BMW-SBOX rolling average signed wrong** (`shunt/BMW-SBOX.h`): `avg_mA_array` and `avg_sum` were `uint32_t`, and the division was unsigned. One -500 mA discharge sample summed across nine zero slots as `(2^32 - 500) / 10 = 429,496,679` mA and landed in the `int32_t` datalayer field as roughly +429 kA - a discharge reported as a colossal charge. Both declarations become `int32_t`, matching the samples they hold.

The same pass zeroes these members and `k`: `avg_mA_array[10] = {0}`, `avg_sum = 0`, `k = 0`. Today `new BmwSbox()` happens to value-initialise them because the class declares no user-provided constructor, which is exactly the accident-of-allocation-expression that made ECMP's cell array live garbage the moment a constructor was added. One initialiser makes that impossible to repeat. A test pins it: `SboxMemoryInitTest.RollingAverageStartsZeroedNotHeapGarbage` default-init placement-news over a 0x42-poisoned buffer and confirms the first-sample average is not influenced by that poison.

**CHEVY-VOLT-CHARGER over-current clamp** (`charger/CHEVY-VOLT-CHARGER.cpp`): the over-current branch assigned the max-amp constant to `setpoint_HV_VDC` (the voltage setpoint) instead of `setpoint_HV_IDC`. That transmitted a nonsense ~11 V voltage setpoint and left the current unclamped - the second consequence being the worse one. The neighbouring voltage clamps show the intended shape.

Tests cover all three defects, both writers of the shared deci-amp field, and the charge direction that must not regress. The over-current test drives 250 V deliberately: at 300 V the power clamp reaches the same current by itself, so the assertion would hold even with the current clamp deleted.

Not reproduced on hardware - no SBOX, no BYD shunt and no Volt charger on this bench. Host tests over the decode and clamp arithmetic.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**BMW-SBOX: the "1 second average" divides by 10 before 10 samples exist**
Branch [`sbox-average-divisor`](https://github.com/ekholm/Battery-Emulator/tree/sbox-average-divisor) @ `be280452` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sbox-average-divisor) · includes the entry above
`BMW-SBOX` fills one average slot per 100 ms and unconditionally publishes `avg_sum / 10`, so for the first second - and after any gap in `0x200` frames - the average reads a tenth to nine tenths of the true current. Live, not theoretical: Kostal transmits `measured_avg1S_amperage_mA` to the inverter whenever an S-BOX is configured. The divisor becomes the count of samples actually taken, capped at the window. The judgement is stated rather than hidden: publish the average over the samples that exist, because the field carries no validity flag - "publish nothing" means the consumer keeps reading the initial 0 A, which is the same defect class in a quieter coat. An average over real samples converges inside the second.

<details>
<summary>PR body it would ship with</summary>

This branch includes the `driver-signedness-clamps` fixes and adds the average-divisor fix on top.

`BMW-SBOX` accumulates one current sample per 100 ms in a ten-slot ring, then publishes `avg_sum / 10` as a one-second average. The divisor is always 10, whether the ring has filled or not. For the first second after the shunt starts reporting - and after any gap in `0x200` frames that lets the window go empty - the published average is a fraction of the real current: one real sample and nine zero slots averaged in as though they were zero-current readings, so the first sample is reported as one tenth of the real current.

This is not an internal number. KOSTAL-RS485 transmits `measured_avg1S_amperage_mA` to the inverter whenever the S-BOX shunt is selected, so the understated reading goes out on the wire during that window.

The divisor becomes the count of samples actually taken, capped at the window size (`avg_samples`, initialised to 0 and incremented each time the ring advances, capped at `AVG_SAMPLE_COUNT`). The sum still runs over the whole array: the unfilled slots are zero and contribute nothing, so the two are arithmetically equivalent, and the bound on the whole array is the one that cannot read past its end.

The judgement is stated rather than hidden: the alternative considered was to publish nothing until the window fills. That is worse, because the field carries no validity flag - "publish nothing" means the consumer keeps reading the initial 0 A, which is a plausible-looking wrong number in a quieter coat. An average over the samples that exist is never a number no measurement supports, and it converges to the full-window average inside the second.

The two window constants (`AVG_SAMPLE_COUNT = 10`, `AVG_SAMPLE_INTERVAL_MS = 100`) are named while the lines are open. They are one fact stated twice, and `measured_avg1S_amperage_mA` is named for it. A `static_assert(AVG_SAMPLE_COUNT * AVG_SAMPLE_INTERVAL_MS == 1000)` makes the relation a build-time constraint rather than a comment - the two cannot drift apart silently.

Six new cases and a re-cut of two existing ones (those pins working: `OneDischargeSampleAveragesToItsOwnTenth` becomes `OneDischargeSampleAveragesToItself`). Seven mutations, each caught by a named test: divisor back to the window, cap removed, cap boundary `<=`, count starting at 1, sum dropping a slot, sampling guard removed, and the mathematically equivalent bound-to-count swap which is the whole fix's proof that the divisor is the only thing changing.

Not reproduced on hardware - no SBOX on this bench. Host tests over the averaging arithmetic.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Kostal: a silent S-BOX must stop deciding the current the inverter is told**
Branch [`shunt-staleness-gate`](https://github.com/ekholm/Battery-Emulator/tree/shunt-staleness-gate) @ `199c9045` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:shunt-staleness-gate)
`datalayer.shunt.available` is cleared 1000 ms after the last S-BOX frame - and nothing read it. Kostal kept transmitting the last shunt current forever after the shunt went silent. The gate makes Kostal check, and the interesting half is the fallback: when the S-BOX is stale, the inverter gets `battery.status.reported_current_dA` - not a value invented for an error path, but exactly what the same function's `else` branch already sends into the same two byte offsets for every installation without an S-BOX. It is the mapping the protocol already uses when nothing is measuring at the shunt, which is precisely the condition; the shunt reclaims the fields the moment frames resume. `0.0 A` was the alternative and is worse: equally untrue, and it reads as healthy idle.

<details>
<summary>PR body it would ship with</summary>

`datalayer.shunt.available` had three writers and no readers anywhere in the tree. BMW-SBOX clears it 1,000 ms after the last `0x200`/`0x210`/`0x220` frame, so the staleness the driver carefully tracks was recorded and then ignored.

KOSTAL-RS485 is the live consumer: whenever an S-BOX is selected it writes `datalayer.shunt.measured_amperage_mA` and `measured_avg1S_amperage_mA` into the cyclic frame at byte offsets 18 and 22. Those fields keep their last value once the shunt goes quiet, so a dead shunt and a steady one look identical on the wire, for as long as the outage lasts.

The gate makes KOSTAL check `datalayer.shunt.available` before writing those two offsets. When the shunt is stale, the inverter gets `datalayer.battery.status.reported_current_dA` into both positions instead. This is not a value invented for an error path: the `else` branch directly below in the same function already writes that same field into these exact two byte offsets for every installation without an S-BOX. It is the mapping the protocol already uses when nothing is measuring at the shunt, which is precisely the situation. The shunt reclaims the fields the moment frames resume.

`0.0 A` was the alternative and is worse: equally untrue, and it reads as a healthy idle battery - the one state that invites the inverter to act.

What is deliberately not gated: `CYCLIC_DATA[56]` and `[59]`, the precharge and contactor bytes. They also come off `datalayer.shunt`, but BMW-SBOX writes them from the emulator's own contactor state machine in `transmit_can()`, not from received frames, so they are not stale when the shunt goes quiet.

Tests drive the real request/response path rather than peeking at the internal buffer: the test-harness `Serial2` grew a read queue and a write capture, and the tests feed the actual battery-info and cyclic-data request frames, then read the two floats out of the 64-byte frame the firmware actually sent, with null stuffing undone. That is what makes "the average field is quoted from the same dead shunt" a checkable claim rather than a second copy of the first assertion.

Not reproduced on hardware - no SBOX on this bench.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**DALY: both power derates release completely one step past the limit they exist to enforce**
Branch [`daly-derate-underflow-clamps`](https://github.com/ekholm/Battery-Emulator/tree/daly-derate-underflow-clamps) @ `c1d75db5` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:daly-derate-underflow-clamps)
`DalyBms::update_values()` computes both of its derates as unsigned subtractions of values that legitimately go negative. One decivolt below the minimum discharge voltage - a normal end-of-discharge state - `voltage_dV - min_voltage` wraps to about four billion and the discharge derate stops applying; one count above 100 % SOC, `10000 - SOC` wraps and the charge derate stops applying. Each derate therefore disengages exactly at the boundary it exists to protect, and stays off however far past it the pack goes. Both headrooms are clamped at zero before the multiply.

<details>
<summary>PR body it would ship with</summary>

The discharge derate tightens toward zero as the pack approaches `min_voltage` - the design minimum, or the user's own discharge floor when one is set:

```cpp
if (voltage_dV - min_voltage < user_selected_daly_power_per_dV_start)
  voltage_power_limit = (uint32_t)(voltage_dV - min_voltage) * user_selected_daly_power_per_dV;
```

Crossing that voltage is normal, not an error. One decivolt below it the difference is `-1`, the cast makes it `4 294 967 295`, and the product is larger than any real limit - so the `<` below applies nothing. Measured against unfixed source with a 3000 dV minimum and a 50 W/dV derate: 29 990 W at 299.9 V and 20 000 W at 200.0 V, where 0 W was owed. Against a *user* discharge limit of 3400 dV it reads 33 990 W at 339.9 V, which is the pack's ordinary current limit standing entirely unclamped - and that is the case a real installation meets first, since a conservative user floor is crossed in ordinary operation.

The charge derate has the same shape: `10000 - (uint32_t)SOC` with `SOC` in hundredths of a percent, so a BMS reporting 100.1 % (one wire count past full - the field arrives in tenths of a percent) releases it. Measured 35 000 W where 0 was owed.

Both headrooms are now clamped at zero before the unsigned multiply. Held at zero rather than skipped: past the boundary the derate's answer is zero power, not "no opinion", and skipping it would leave whatever limit another arm had set.

Host tests walk each boundary rather than sampling one side of it - in band, exactly at the limit, one step past, far past - and cover the user-limit path separately from the design floor, since a refactor reading the design floor twice would restore the release for exactly the users who asked for the tighter limit. Testing the driver at all needs an RX queue on the host `HardwareSerial` emulation: the pack state is in file statics reachable only through the driver's own RS485 parser. The queue is empty by default, so existing tests see no change. The test helper also asserts that each pack actually reached the parser before any power figure is checked: if the queue is ever lost, the cases would otherwise still fail, but on the in-band arm, which reads as a derate defect when nothing was fed.

Not fixed here: `SOC` is still unbounded on the way in, where `decode_uint16be(...) * 10` can wrap above 655.35 %. Separate defect; the clamp above is correct for every value that reaches it either way.

Note: drafted with AI assistance, reviewed by me.

</details>

## Inverters

---

**SOLAX: the contactor-close permission no longer outlives the inverter's open request**
Branch [`solax-contactor-permission-uplift`](https://github.com/ekholm/Battery-Emulator/tree/solax-contactor-permission-uplift) @ `a69c78ef` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:solax-contactor-permission-uplift)
When the inverter commanded the contactor open, the state machine reset but `inverter_allows_contactor_closing` stayed true until the next received frame. The revocation now happens in the open-command branch itself. The exposure, stated precisely: the flag was never unbounded - a 2-second silence timeout already clears it - so the window was the inverter's own next transmission or about 2-3 s, whichever came first. The tests pin both the revocation and the timeout backstop, including its AlwaysClosed gate, so neither safety layer can regress silently.

<details>
<summary>PR body it would ship with</summary>

`SolaxInverter::map_can_frame_to_variable()` grants and revokes `inverter_allows_contactor_closing` from the same state machine, but not at the same time.

The `CONTACTOR_CLOSED` case sets the flag near its top, then tests the payload for the inverter's open command. That branch raises `EVENT_INVERTER_OPEN_CONTACTOR` and resets `STATE = BATTERY_ANNOUNCE` - but does not touch the flag. The `BATTERY_ANNOUNCE` case is what clears it, and that runs on the next received frame.

So between the open request and the next frame, the permission stays true. If the inverter goes quiet after asking to disconnect, the clearing frame never comes, and the permission stands until `update_values()`'s RX-timeout backstop fires. That backstop bounds it: `INTERVAL_2_S` measured from `LastFrameTime`, which the open frame itself refreshed, checked on the 1 s core-loop cadence - so roughly 2-3 s of stale permission. The flag is read by `precharge_control.cpp`, `comm_contactorcontrol.cpp`, and eleven battery drivers.

The fix is one line in the block that already handles the open request: revoke the permission there, so the frame carrying the request also carries the revocation.

`LockAfterFirstClose` is deliberately unaffected. The revocation sits inside the same `NoWorkaround` gate as the `set_event` call, so the lock still holds. `AlwaysClosed` reaches the permission by a different path entirely - an early return that bypasses the state machine - and the timeout backstop is also gated on `NoWorkaround`, so that mode is unaffected by the backstop as well.

Seven tests in `test/solax_contactor_permission_tests.cpp` drive the real state machine over RX frames: the open request leaves the permission false with no further frame; it stays false while `BATTERY_ANNOUNCE` runs; the backstop threshold, pinned at 1999 ms and 2000 ms of silence; `LockAfterFirstClose` survives the timeout and ignores the open payload; the backstop measures silence from the latest frame, not from the first; `AlwaysClosed` survives the timeout.

Builds on lilygo_330. Not reproduced on hardware - no SOLAX inverter available. The state walk is from the source and from the host tests.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**BYD-MODBUS: the static-data write cursor survives the call**
Branch [`byd-modbus-static-cursor`](https://github.com/ekholm/Battery-Emulator/tree/byd-modbus-static-cursor) @ `782a2376` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:byd-modbus-static-cursor)
`handle_static_data()` tracks its write position through the identity and manufacturer strings with `static uint16_t i = 100`, so any instantiation after the first resumes where the previous call stopped. `setup()` runs once per boot today, but the drivers-from-store replace-live path re-instantiates inverter drivers at runtime, and the second instance then presents a garbled identity block. One word: the cursor is a plain local.

<details>
<summary>PR body it would ship with</summary>

`handle_static_data()` tracks its write position through the identity and manufacturer string arrays with `static uint16_t i = 100`, so any instantiation after the first resumes where the previous call stopped.

Dumping the poisoned map with three instances gives the real layout: the six arrays total 68 words, so the second call resumes at 168 - registers 100-167 absent, `battery_data[0]` landing at register 186, and a garbled window from 168 to 235 that the `p201`/`p301` init loops only partially repair. Because `mbPV` is a per-instance map, the new instance's registers 100-181 stay absent entirely.

`setup()` runs once per boot today, so this is latent on a shipping build. It stops being latent on the drivers-from-store replace-live path, which re-instantiates inverter drivers at runtime.

The fix is one word - the cursor becomes a plain local. The regression test constructs two instances in one process and requires the second to write `si_data[0]=21321` at register 100 with nothing past the `p201` repair window; both of its assertions fail with the static restored.

**A note on the test, because the first version of it did not bite.** The original second assertion checked register 214, which holds `volt_data[12]=0` in the shifted layout too - so it passed against the unfixed code and guarded nothing. It now checks register 186, which carries `16985` before the fix.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**GROWATT LV: capacity is sent 10x too high, truncated, and wraps above ~65 Ah**
Branch [`growatt-lv-capacity`](https://github.com/ekholm/Battery-Emulator/tree/growatt-lv-capacity) @ `165e1957` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:growatt-lv-capacity)
Three defects in one expression. The 0x314 capacity fields are computed as `(Wh / voltage_dV) * 100` and packed `* 100` into a field documented as 10 mAh units: `Wh / dV` is Ah/10, so the transmitted value is Ah x 1000 where the field wants Ah x 100. The integer division truncates before the scaling multiply - the code's own worked example yields 20.0 Ah instead of 27.7 Ah - and the `uint16_t` intermediate silently wraps above ~65 Ah. Worth noting: the HV sibling has since been given exactly this arithmetic upstream, and the LV file was left behind.

<details>
<summary>PR body it would ship with</summary>

The 0x314 capacity fields are computed as `(Wh / voltage_dV) * 100` and packed `* 100` into a field documented as 10 mAh units. Three separate defects sit in that one expression:

1. **Scale.** `Wh / dV` is Ah/10, so the transmitted value is Ah x 1000 where the field wants Ah x 100 - 10x too high.
2. **Truncation.** The integer division truncates before the scaling multiply. The code's own worked example, 10000 Wh at 360.0 V, yields 20.0 Ah instead of 27.7 Ah.
3. **Overflow.** The `uint16_t` intermediate silently wraps the field above ~65 Ah.

Compute 0.1 Ah as `(Wh * 100) / voltage_dV` - multiply before dividing, so nothing truncates until the last digit - and pack `* 10`. The intermediates are `uint32_t` and the 16-bit field saturates at 655.35 Ah instead of wrapping. The `Wh * 100` product fits `uint32_t` for packs up to about 42 MWh.

Tests cover each unit boundary: an exact Wh to 10 mAh conversion at 100.00 Ah, the divide-after-multiply order, saturation above 655.35 Ah, and the `voltage_dV > 10` update guard. A fifth test gives the two capacities DISTINCT values, because every existing test in the file sets them equal - so a swap or aliasing of the 0x314 byte pairs survived the whole suite, verified by mutating the full-capacity bytes to pack the remaining value.

**Context that may be useful:** current `main` computes the HV capacity as `Wh * 1000 / dV` with a 64-bit intermediate, which is the same correction this makes. The LV file did not get it.

Note: drafted with AI assistance, reviewed by me.

</details>

## Contactors and safety

---

**Parallel batteries: the 1.5 V join gate becomes symmetric, and its state survives instances**
Branch [`parallel-join-symmetry`](https://github.com/ekholm/Battery-Emulator/tree/parallel-join-symmetry) @ `46913556` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:parallel-join-symmetry)
The voltage-difference gate that keeps a battery from closing onto a live parallel link was one-directional: battery 2 was checked against battery 1, but battery 1 could (re-)close onto the link unchecked. The gate is now symmetric, its state is gathered into a resettable struct instead of file-scope statics, and the safety and SOH sentinel checks latch. Introduces `reported_contactor_state()` on the battery interface - a battery reports what its contactors are doing rather than the safety layer inferring it. Includes a review-raised bound (credited in-source to jonny5532): 3700 dV means "no voltage decoded yet" and must not satisfy the gate. Fake-triple tests pin the drift grace at both ends and the sentinel case.

**On the 370 V startup case, this branch now adopts your own fix rather than competing with it.** You solved the same ambiguity in `a5bd6eb37` ("Make it possible to startup with 370V") by corroborating the 3700 dV reading against `cell_max_voltage_mV`, which carries its own 3700 mV default; this branch had solved it by latching once both packs are seen off the sentinel. The two cover different failures - the latch catches a joined pair drifting apart while one pack happens to read exactly 3700, and the corroboration catches a pack that BOOTS at 370.0 V and would otherwise sit in the startup grace forever - so the branch now carries both, using your corroboration verbatim. **And the shape of how you fixed it is itself the argument for this refactor.** `a5bd6eb37` corroborated battery 2 only; the battery-3 copy kept returning on a bare `== 3700` until `6355c7bec` ("Let the third battery join the DC link at 370.0 V") applied the same check there a day later - one fix, written twice, because the rule lives in two copy-pasted blocks. `main` now carries four corroboration sites for one rule. This branch carries one, in a shared helper both joiners call, so a change to the rule cannot reach one pack and miss the other. Offered as an argument for the refactor, not as a criticism of the fix: the duplication is what made the second fix necessary.

<details>
<summary>PR body it would ship with</summary>

The 1.5 V parallel-join rule in `check_parallel_battery_safety()` was enforced only for battery 2/3 closing toward battery 1. Battery 1's own close was never checked. After a main-pack dropout (BMS reset, wakeup glitch, fault recovery) while battery 2 remains engaged, battery 1 re-closes through its normal contactor path with no voltage check against the live link. The GPIO precharge only equalizes dead-link capacitance - against a link held stiff by another closed pack it still closes after the timer with whatever difference remains. Pack-internal windows (MEB ~20 V) are far looser than 1.5 V.

`check_parallel_battery_safety()` now computes `battery1_allowed_contactor_closing`: false while another engaged pack differs by more than 1.5 V. Close-gating only - opening the main battery under load is its own hazard and stays out of scope. `handle_contactors()` consumes it in the `START_PRECHARGE` transition. The main-instance constructors of the three drivers that already took the gating pointer for their battery-2 instances (BMW i3, Kia/Hyundai 64, Pylon) now wire it too. Tesla reports its contactor state through the new API but the main instance's `contactor_closing_allowed` pointer is not wired - Tesla has no consumption path without touching its TX command logic, and that is stated as an open item rather than smuggled around.

Whether another pack is closed comes from a new Battery virtual: `ContactorState reported_contactor_state()`, tri-state (Unknown/Open/Closed). BMW i3, Tesla and MEB each override it with a mapping of an already-parsed member; SNA and error conditions return Unknown, never a definite state. The gate keys on reported Closed; Unknown falls back to BE-commanded state, so a commanded-but-failed close cannot deadlock the main pack.

The two battery 2/3 check blocks were verbatim copy-paste. They collapse into one `check_parallel_join()` helper both joiners call, so the rule lives in one place. The 3700 dV sentinel is now a startup grace rather than a continuous skip condition: once both packs have been seen off it, the check runs regardless of transient 3700 readings. Cell voltages corroborate an ambiguous 370.0 V reading, adopting upstream's approach from #2958 verbatim. The latch and the corroboration cover different failures: the latch catches a joined pair drifting while one pack reads exactly 3700; the corroboration catches a pack that boots at 370.0 V and would otherwise wait in the startup grace indefinitely.

Upstream applied the 370.0 V corroboration to battery 2 in #2958 and then to battery 3 in a separate commit a day later - one rule, written twice, because it lived in two copy-pasted blocks. This branch carries one instance, in the shared helper both joiners call. That is the argument for the refactor, made from the fix's own history rather than from a standalone defect.

15 host tests: five fake-triple cases (normal mirror operation raises no event, single-tick lag absorbed by the 3 s grace), seven symmetry cases (block on large diff, allow within window, disengaged pack does not block, existing battery-2 gating as regression guard, unknown-fallback path, gate blocks START_PRECHARGE, a pack at the sentinel voltage still engages the gate), and three voltage-sync cases (existing disengage while main reads sentinel, boot-at-sentinel, cell-voltage grace). Builds on lilygo_330 and stark_330. Not run on hardware.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Contactors: a faulted system must not arm the closing ladder at boot**
Branch [`contactor-fault-boot-race`](https://github.com/ekholm/Battery-Emulator/tree/contactor-fault-boot-race) @ `e169724c` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:contactor-fault-boot-race)
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

**BMW-SBOX: contactor transitions decided from a dead shunt's last reading**
Branch [`sbox-precharge-stale-shunt`](https://github.com/ekholm/Battery-Emulator/tree/sbox-precharge-stale-shunt) @ `3985bb35` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sbox-precharge-stale-shunt)
The precharge sequence reads the shunt's last stored values with no check that the shunt is still talking. `datalayer.shunt.available` already exists and already goes false after a second of silence; the sequence never consults it, so a shunt that dies mid-precharge leaves the state machine advancing on a frozen number. The abort now raises an event - and, in the same branch, clears it on recovery, because an ERROR that nothing clears latches a shutdown twenty seconds later.

<details>
<summary>PR body it would ship with</summary>

The S-BOX precharge sequence decides contactor transitions from the shunt's last stored reading without checking that the shunt is still alive. `datalayer.shunt.available` is already maintained - it goes false one second after the last frame - and the sequence never reads it.

The sequence now aborts when the shunt goes away, and that abort is `DISCONNECTED` rather than `SHUTDOWN_REQUESTED`: a shunt that comes back should let the sequence run again.

**Three things about the abort that are worth reading before the diff, because the first version of it introduced a failure mode the code did not have:**

1. **A raised ERROR that nothing clears is not recoverable.** `EVENT_LEVEL_ERROR` drives `system_status` to FAULT; this driver counts FAULT ticks; past 2000 of them (20 s) it requests `SHUTDOWN_REQUESTED`, which this same file documents as *"not possible to recover without a powercycle"*. With nothing clearing the event, a single momentary dropout guaranteed a power-cycle-required shutdown mid-operation with a shunt that never faulted again. The event is now cleared on the recovery path, and with that the sequence resumes, the relays stay closed, and a shunt that stays dead still latches after 20 s - the flap protection is kept, not traded away.

2. **A start gate, for the same reason one level down.** Booting with a dead shunt and waiting 25 s gives, with the gate, status 3 and relays 0x55 - it sits benignly. Without it, status 4 and relays 0x00: latched. A board that merely booted with no shunt connected would have needed a power cycle.

3. **A lost shunt on a live bus is reported, at WARNING.** `shunt.available` had exactly two consumers and nothing anywhere said when the shunt died, so a live bus silently lost current measurement. It must NOT be reported with the precharge event: raising that on a COMPLETED bus would drive FAULT and open contactors under load twenty seconds later, which is the hazard the exclusion exists to prevent. It gets its own warning-level event, and that event is cleared on recovery too - a test pins the clearing, because removing it left the whole suite green.

**One test-harness note, since it changes what the suite can see.** The listener now calls `init_events()`, so event LEVELS are production levels for every test. Before, two fixtures called it in their own `SetUp` and levels persist - `reset_all_events()` clears state, never levels - so whichever of those ran first handed real levels to everything after it, and part of every run was level-blind in a way that moved with the shuffle seed.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Contactors: every battery's veto on closing is ignored, so no driver can hold its contactors open**
Branch [`fix/restore-battery-contactor-veto`](https://github.com/ekholm/Battery-Emulator/tree/fix/restore-battery-contactor-veto) @ `1b739986` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:fix/restore-battery-contactor-veto)
`battery_allows_contactor_closing` was half of the contactor state machine's closing gate until `1645c5b3` dropped it; since then contactors close on the inverter's say-so alone. Every driver that withholds the flag until it has seen its BMS or finished a handshake (MEB/MQB, BMW iX, Atto 3, MG Gen1, LEAF, the Volvos, Growatt LV, CHAdeMO) is overruled, and nothing reads the flag. The visible case is CHAdeMO, whose contactors close at boot with nothing plugged in (#1863, #1662).

<details>
<summary>PR body it would ship with</summary>

`battery_allows_contactor_closing` was half of the contactor FSM's DISCONNECTED gate until `1645c5b3` ("Further improve dialogue boxes and texts", #1433, first shipped in v9.0.RC3) dropped it from the condition. Since then contactors close on inverter say-so alone: the gate reads `inverter_allows_contactor_closing && !equipment_stop_active`, and the battery's flag is written by the drivers but read by nothing.

That matters for every driver that withholds the flag until it has seen the BMS or completed a handshake: MEB/MQB, BMW iX, Atto 3, MG Gen1, LEAF, the Volvos, Growatt LV. None of them can keep contactors open any more. The most visible case is CHAdeMO, which holds the flag false from setup and only grants it in `CHADEMO_EVSE_START`, after the CAN session, the pin-4 permission and a valid target voltage. With the gate gone the whole EVSE sequence is bypassed and contactors close at boot with nothing plugged in - the symptom in #1863 and #1662.

This restores the flag to the gate and fixes the drivers the restoration would otherwise leave unable to close. The flag defaults to false, so a driver that never writes it would never close contactors: **CHARGEBYTE-CCS**, **GEELY-SEA** and **AKASOL** now grant at setup with the vacuous allow stated in a comment, the same contract as the other setup-granting drivers. No driver's behaviour changes otherwise; the diff to the gate is one added condition.

**Tests.** `contactor_veto_contract_tests` runs every constructible battery driver through setup plus benign update ticks and asserts the flag lands where that driver's declared behaviour says: setup-granting drivers must have granted, handshake- and CAN-gated drivers must still be withholding. A driver that never writes the flag fails, and a new driver fails until its author declares which kind it is - which is how AKASOL and Growatt LV, added after this was first written, were caught. Two scenario tests in `contactor_sequence_tests`: `ChademoWithNothingPluggedInNeverClosesAtBoot` (the #1863 report, through the real driver: five seconds of contactor ticks, DISCONNECTED throughout) and `CompletedIsNotReopenedByBatteryRevocation`, which pins the existing semantics on purpose - the veto gates *closing* only; a battery revoking mid-session does not open contactors from COMPLETED (opening under load is the e-stop path's sequenced job).

**What this does not claim.** No hardware leg was possible for CHAdeMO itself; the gate change is exercised on the host through the real drivers. The CHAdeMO driver's other regressions are separate.

Refs #1863, #1662.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Safety: with two or three packs, a healthy pack clears another pack's cell-deviation or SOH-difference warning**
Branch [`fix/shared-pack-warnings`](https://github.com/ekholm/Battery-Emulator/tree/fix/shared-pack-warnings) @ `53f78944` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:fix/shared-pack-warnings)
The cell-deviation and SOH-difference checks run per pack and each clears the SHARED event, so whichever pack is evaluated last decides whether a real warning is shown: a healthy second pack silently clears the first pack's deviation warning every cycle. The two checks now aggregate across the packs before deciding, and a placeholder SOH reading can no longer clear a genuine SOH warning. One file, `safety.cpp`. Still present on upstream `main`.

<details>
<summary>PR body it would ship with</summary>

With two or three packs, the cell-deviation and SOH-difference checks in
update_machineryprotection() each ran per pack and called clear_event()
on the shared event, so whichever pack was evaluated last decided the
outcome: a healthy pack cleared a warning another pack had just raised.

Both checks now aggregate across the configured packs before deciding.
EVENT_CELL_DEVIATION_HIGH is raised when any pack's spread exceeds its
limit, with the first offending pack's deviation as the event data (same
/20 scaling), and cleared only when no pack exceeds it. EVENT_SOH_DIFFERENCE
compares battery 1 against each extra pack and is raised when any pair
differs by more than MAX_SOH_DEVIATION_PPTT.

The SOH-difference warning is left untouched when no pack pair has two
real readings (a pack reporting the 9900 placeholder), exactly as the
per-pair checks behaved for a single pair, so a transient placeholder
cannot clear a genuine warning.

The per-pack event scheme is a separate change: it renames published
events, which is a question of its own and does not belong in this one.

Note: drafted with AI assistance, reviewed by me.
</details>

## Settings and web UI

---

**Settings: only the full form may treat an absent checkbox as unchecked**
Branch [`partial-form-bools`](https://github.com/ekholm/Battery-Emulator/tree/partial-form-bools) @ `2e959ad6` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:partial-form-bools)
HTML forms omit unchecked checkboxes, so the server treats "absent" as "false" - correct for the full settings form, destructive for any partial POST, which silently wiped every boolean it did not mention. The full form now carries a hidden `FULLFORM` marker and only its presence licenses the absent-means-unchecked reading; partial POSTs leave unmentioned booleans alone. Fourteen lines, and the class of accidental factory-resets-by-curl goes away.

<details>
<summary>PR body it would ship with</summary>

`POST /saveSettings` applies absent-means-false to every boolean setting in its loop. That is correct for the full settings page - an unchecked HTML checkbox is simply absent from the submission - but it is destructive for any other caller. A partial POST, whether from curl, a script or an API client setting one field, silently clears every boolean it did not mention: `WIFIAPENABLED`, `MQTTENABLED` and the rest.

The full settings form now marks itself with a hidden `FULLFORM` field. The handler reads it once before the boolean loop: with the marker present, absent-means-false applies as before; without it, a boolean is applied only when its field is actually in the POST (`on` = true, anything else = false), and a missing field is skipped entirely. A partial client can therefore set, clear or leave any boolean without coordinating with the rest. The double/triple capability clamp below the loop is unchanged - it enforces a stored invariant, not a form field, and runs in both paths.

The change is fourteen lines. The class of accidental factory-resets-by-curl goes away, and sectioned forms - where a section's POST naturally omits every boolean outside its own section - become safe without any further server-side work.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Hostname: stop copying it on every read**
Branch [`hostname-no-copy`](https://github.com/ekholm/Battery-Emulator/tree/hostname-no-copy) @ `2f67b0fb` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:hostname-no-copy)
`custom_hostname` was an `std::string` in a tree whose consumers speak Arduino `String`, so every read paid a conversion copy. It becomes a `String`, both accessors return `const String&`, and all five call sites bind for free - `MDNS.begin()` and `html_escape()` take the reference directly. The quieter win: the file is now host-testable at all, and ships with its tests.

<details>
<summary>PR body it would ship with</summary>

`active_hostname()` and `default_hostname()` returned `String` by value from storage that outlived every caller. "battery-emulator-" plus four hex digits is 21 characters, well past the 13 below which an Arduino String stays inline, so each call paid a malloc, a copy and a free. `syslog_send()` calls `active_hostname().c_str()` once per line. Measured with an operator-new probe: 100 allocations per 100 reads. Now 0.

The reason a copy was unavoidable at all: `custom_hostname` was a `std::string`, so the custom-name branch built a `String(custom.c_str())` out of bytes that already existed. `custom_hostname` becomes a `String`, and both accessors return `const String&`. Every call site still works: the two that want their own copy assign to a `String` and get one; `MDNS.begin()` and `html_escape()` take `const String&` directly; the settings page still returns a value.

`hostname.cpp` joins the host test build, with `esp_read_mac()` stubbed alongside the other IDF headers the test emulation already provides. `hostname_for_mac()` is split out as a pure function: `default_hostname()` reads the eFuse once and caches, which makes the format untestable through it. The pure function is what goes on the wire and now has tests for the cases that actually break - leading-zero handling (`%x` vs `%02x` is wrong only for some MACs), lowercase, and that only the last two bytes are used.

Seven mutations caught: format-drops-leading-zeroes, wrong-mac-bytes-used, uppercase-hex, selection-inverted, active-returns-a-copy, cache-rebuilt-every-call, and back-to-returning-by-value (caught at compile time - the test takes the address of what it gets back, and an rvalue has none).

Host suite green; lilygo_330 builds clean.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Pin roles: illegal combinations refused at selection time**
Branch [`pin-role-exclusions`](https://github.com/ekholm/Battery-Emulator/tree/pin-role-exclusions) @ `30454a6d` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:pin-role-exclusions)
A legal-combination table (`pin_exclusions.{h,cpp}`) makes enforced exclusions data entries, with the known-legal shared-pin groups documented beside them with their rationale - so the next exclusion is an entry, not an investigation. Enforcement runs at `/saveSettings`: the would-be pin assignment is computed and an excluded pair is refused before it is stored, instead of discovered at boot. The board knowledge in the table is hand-maintained today; if board capabilities ever become declarative, this table is the natural first consumer.

<details>
<summary>PR body it would ship with</summary>

Board pin tables multiplex one GPIO across several roles on purpose. Most coexisting roles are user-selectable alternatives or structurally exclusive options, so uniqueness alone cannot be asserted globally. What can be stated is which pairs of roles must never be active at the same time.

Today, a collision like that surfaces at boot as `EVENT_GPIO_CONFLICT` out of `alloc_pins()`, when the user can no longer act on it. The new `pin_exclusions.{h,cpp}` moves enforcement to `/saveSettings`, where the would-be assignment can still be refused.

The helper `pin_exclusion_conflict()` takes the candidate inverter protocol and equipment-stop setting plus the two board-specific pin numbers, and returns a user-showable sentence naming the conflict or `nullptr` for a legal combination. It is a pure function of its inputs, so every board's verdict is testable without hardware. `/saveSettings` reads the candidate state from the request's own parameters where present and from the currently stored values otherwise, calls the helper, and returns HTTP 400 with the conflict text if excluded.

The currently ruled pair is the SMA inverter's contactor-enable pin and the equipment stop button, which the Stark board wires to the same GPIO (GPIO 2). SMA LV does not drive a contactor pin and stays legal. Boards that wire these roles to separate pins are not affected. `GPIO_NUM_NC` ("not connected") is treated as never colliding, so a board that does not wire one of these roles at all does not report a conflict.

The known-legal shared-pin groups are documented in the header with their rationale, so the next exclusion lands as a table entry rather than requiring the wiring to be re-derived from the board schematics.

The host matrix test instantiates every classic-ESP32 board HAL and pins each board's verdict explicitly. S3 boards do not compile in the host GPIO emulation and are recorded as read-apart at implementation time.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**Settings page: a placeholder with no answer renders empty on a shipping build**
Branch [`settings-placeholder-pairing`](https://github.com/ekholm/Battery-Emulator/tree/settings-placeholder-pairing) @ `f5098f74` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:settings-placeholder-pairing)
The settings template and its processor are coupled only by hand-matched string literals. Nothing checks them and no compiler can see them, so a template saying `%BAL_MAX_TIME%` beside a handler testing `"BALANCING_MAX_TIME"` renders empty on a shipping build with every test green. That pair survived fourteen months and was reported from outside. Two live orphans are fixed and the pairing is now enforced.

<details>
<summary>PR body it would ship with</summary>

The settings template and its processor are coupled only by hand-matched string literals. Nothing checked them and no compiler can see them, so a template that says `%BAL_MAX_TIME%` next to a handler that tests `"BALANCING_MAX_TIME"` renders empty on a shipping build with every test green.

Two live orphans, both fixed:

- **`%BAL_MAX_TIME%`** - the reported one. The handler is renamed rather than the template, because its four neighbours are all `BAL_*`; renaming the other way would leave it the only `BALANCING_*` value placeholder in the block.
- **`%VOLTAGE_LIMITS_CLASS%`** - not reported by anyone, and it shows WRONG STYLING rather than a missing value, which is why it went unnoticed longer.

The durable half is the pairing test: every placeholder in the template must have an answering handler and every handler an emitting placeholder, so the next orphan fails a build instead of shipping.

One layer is guarded beyond that, because it is the same coupling arriving by the same route: a `*_CLASS` handler does not return a value, it returns a CSS class NAME hand-matched to the stylesheet in the same file. A handler returning `"inactiveSOC"` would compile, pass every other case, and render an unstyled span. All four names resolve today, so that one is a guard rather than a fix - mutation-checked from both ends: typing the returned literal wrong fails it, and deleting a rule from the stylesheet fails it.

The comment-skipping heuristic's boundary is pinned rather than left implicit: the skip is anchored at the start of the line, so a placeholder in a trailing comment is still collected. The numbers rather than the argument say that is the right trade - scanning both this tree and a sibling, the count of placeholders sitting in a trailing comment is zero, while a rule that stripped `//` anywhere would have to survive templates full of `https://` URLs.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Do not blank a stored SSID because the field came back empty**
Branch [`ssid-blank-guard`](https://github.com/ekholm/Battery-Emulator/tree/ssid-blank-guard) @ `afa581ad` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:ssid-blank-guard)
`/saveSettings` writes SSID unguarded while PASSWORD, in the very next branch, is guarded with "blank = keep existing". Any client that sends the field empty therefore erases the stored network - and because USB logging is off by default, the board then looks completely dead rather than merely unconfigured.

<details>
<summary>PR body it would ship with</summary>

`/saveSettings` writes `SSID` unguarded while `PASSWORD`, in the very next branch, is guarded with "blank = keep existing". Any client that sends the field empty erases the stored network, and because USB logging is off by default the board then looks completely dead rather than merely unconfigured.

The two are guarded for DIFFERENT reasons, which is worth stating rather than copying the neighbour blindly:

- `PASSWORD` is rendered empty, so blank means "unchanged".
- `SSID` is rendered WITH its value, so a blank one is either a partial client that sent the field empty or a user who cleared it - and neither should be honoured over the network the board is being configured on.

`webserver.cpp` is not part of the host build, so the accompanying test is a source guard rather than a behavioural one: it pins that the SSID branch tests the value before saving.

**The guard's own first version was worse than useless, and the reason generalises.** It asked whether `isEmpty()` appears between the SSID branch and the save. That is satisfied by `if (p->value().isEmpty()) { save }` - which saves the SSID ONLY when the submitted field is blank, keeping the original defect and making Wi-Fi configuration impossible. It passed the test. The property is the NEGATION, not the mention, so the guard now looks for `!p->value().isEmpty()` and prints what it found when it fails. Three mutations are caught by name: guard removed, guard inverted, guard weakened to always-true.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Settings: Min-SOC above 50% silently reverts on reboot, and an inverted SOC window is accepted**
Branch [`fix/soc-window-validation`](https://github.com/ekholm/Battery-Emulator/tree/fix/soc-window-validation) @ `8be61c45` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:fix/soc-window-validation)
The SOC window had three different rules at three entry points: the live routes accepted anything, the save stored whatever was live, and boot silently dropped a stored minimum above 50%. Nothing enforced min < max, so an inverted window pins the scaled SOC at 100% and wraps the scaled capacity. One validator now guards every entry point; the routes answer 400 on an invalid pair; the arbitrary 50% cap is gone.

<details>
<summary>PR body it would ship with</summary>

The SOC window (min/max percentage) was guarded by three different rules
at three entry points:

- /updateSocMin and /updateSocMax accepted any value with no validation
- store_settings() persisted whatever was live
- boot load rejected stored minimums above 50.0%, silently falling back
  to the compiled default

So setting Min-SOC above 50% worked until the next reboot, then quietly
disappeared. Worse, nothing anywhere enforced min < max: with an
inverted window CONSTRAIN() pins the scaled SOC to 100% toward the
inverter, and the scaled total capacity wraps negative into a huge
unsigned value.

This adds a single validator used by every entry point, values in pptt:
-10.00% <= min, max <= 100.00%, min at least 1.00% below max (which also
keeps the scaling divisor from getting arbitrarily small), and max at
least 1.00%. The last one matters because a stored maximum of 0 collides
with the NVS never-stored sentinel for MAXPERCENTAGE: the pair would be
accepted live, persisted as 0, and replaced by the compiled default at
boot - the same accept-then-lose class this removes.

The live routes reject invalid pairs with HTTP 400 and the settings page
shows the rejection instead of ignoring it. Boot-time load validates the
stored pair and, when it is invalid, keeps the compiled defaults for
both values. No new event is raised for that: an invalid stored pair can
only come from an older build or a hand-edited store, and an event is not
worth its flash cost for it.

The old 50% boot cap on the minimum is dropped deliberately: large
reserve floors (60-70%) are legitimate for backup-power installations,
and the cap had no counterpart on the live path. NVS encoding and keys
are unchanged.

Host tests cover the validator rules and the apply/reject behaviour.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Settings: a mistagged NVS key can never be repaired by saving, and the user's value is lost at every boot**
Branch [`fix/savex-nvs-tag`](https://github.com/ekholm/Battery-Emulator/tree/fix/savex-nvs-tag) @ `e9a01fd3` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:fix/savex-nvs-tag)
A typed NVS read of a key stored under the wrong type returns the caller's default, and every `saveX()` skipped the write when that read equalled the new value - so saving exactly the value the mistagged read reports did nothing, and the write that would have fixed the tag never happened. For `WIFIAPENABLED` (default on) that means switching the access point off is silently lost. A save is now skipped only when the stored tag matches too. The branch also makes the host build's settings-store emulation real, which turned up a second, smaller defect: a read-only store reported changes it had refused to write.

<details>
<summary>PR body it would ship with</summary>

**Make the settings-store emulation real, and stop a read-only store reporting changes**

Every setting is read and written through BatteryEmulatorSettingsStore, and
the Preferences emulation discarded every write and returned zero from every
read, so a round-trip was not observable in a host build at all.

The emulation now keeps values in memory per namespace, surviving a store
being closed and reopened the way real NVS survives a reboot, and models two
constraints that otherwise only appear on hardware: a read-only store cannot
write, and keys longer than the NVS limit of 15 characters are rejected. The
longest keys in the firmware are exactly at that limit (TARGETDISCHVOLT among
them), so a new one that exceeded it would silently never persist.

Exercising the store against it turned up one defect: the save and remove
methods set settingsUpdated even on a read-only store, where the underlying
write is refused. Nothing reaches that today - the only read-only store is the
one the settings page opens to render values, and it never saves - but it
would have told the user to reboot to apply a change that never happened. The
store now refuses the write outright when it is read-only.

**Make the emulation's typed reads honor the NVS type tag**

Real NVS tags every entry with the type it was written as and a typed
getter on a mismatched key returns the caller's default, not the stored
bits. The emulation returned whichever field the getter named, which for
a cross-typed read is a zero that real hardware would never produce. The
shadow audit and the accessor layer both exist because of exactly this
tag behaviour, so the emulation has to model it for their tests to mean
anything.

**Repair a mistagged setting instead of skipping the write that would fix it**

NVS records the type a value was written under, and a typed read returns the
caller's default when that tag does not match. So on a key stored under the
wrong type, getX() reports the default rather than what is stored - and saveX()
skipped the write whenever the current read equalled the new value.

Saving exactly the value the mistagged read reports therefore did nothing. That
skipped write is the only thing that would have repaired the tag, because
nvs_set_* finds the existing entry whatever its type, writes the new one and
deletes the old. The key stays mistagged and every later boot reads the row
default instead of the user's choice.

One value per type is affected: 0, false, and "". WIFIAPENABLED is the case that
shows why it matters - its default is TRUE, so a user switching the access point
off writes the one value that gets swallowed, and the device keeps booting with
the AP on with nothing pointing at the cause.

A save is now skipped only when the stored tag matches as well, for all four
savers: saveBool->PT_U8 (putBool forwards to putUChar), saveUInt->PT_U32,
saveInt->PT_I32, saveString->PT_STR.

Builds on the settings-store emulation in the preceding commits, which already
stores real values in real namespaces and whose typed reads already honour the
tag. What this adds to it is getType(),
which neither had and which the fix needs - the tag is exactly what a getter
cannot tell you, since it answers with the caller's default on a mismatch - and
a write counter, because the skip-identical optimisation is invisible from the
stored value and a test otherwise cannot tell "left alone" from "rewritten with
the same value".

Tests cover all four savers on their dangerous value, plus both directions of
what must NOT change: an unchanged, correctly-tagged value is still skipped,
and the first save of a falsy value into a missing key is still written, which
is what the existing isKey() guards are for. The skip is not about flash wear -
ESP-IDF's NVS already compares before writing and leaves an identical value
alone, which a bench run confirmed - but about the store's own bookkeeping:
settingsUpdated decides whether the user is told to reboot to apply a change,
and a no-op save must not set it. Verified by reverting the four tag
checks: the four repair cases fail, the two preservation cases pass.

Note: drafted with AI assistance, reviewed by me.

</details>

## Platform, build and storage

---

**Builds depend on which machine made them: 24 Arduino-core `__FILE__` strings carry the builder's PlatformIO path**
Branch [`file-prefix-map`](https://github.com/ekholm/Battery-Emulator/tree/file-prefix-map) @ `43fb9a75` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:file-prefix-map) · on upstream `main` at `c011e9767` (2026-09-11)
The Arduino core's `log_e()`/assert sites put `__FILE__` in flash with the absolute package path it was compiled from, so the same commit built by two people gives different binaries. Two `-ffile-prefix-map` lines in the shared `[env]` flags remove all 24 strings, −1,312 B of `.flash.rodata` on both measured envs (the exact figure is 24 × the builder's core-path length minus 8, so about 1.3 kB on a default install), with `.text`/`.iram0`/`.dram0` byte-identical. The map is narrowed to the Arduino package on purpose: a wider one also matches the IDF tree, outranks IDF's own `/IDF` macro map, and grows the image by 1,648 B. Build flags only, so the host test suite does not see it; built and measured on `lilygo_330` and `esp32devkit_330`.

<details>
<summary>PR body it would ship with</summary>

The Arduino core is compiled from its absolute PlatformIO package path, and its `log_e()` and
assert sites put `__FILE__` into flash. That puts **24 strings carrying the builder's own
PlatformIO core directory** into every image - so two people building the same commit get
different binaries, differing by the length of a path that has nothing to do with the firmware.

Two `build_flags` lines in the shared `[env]` section fix it:

```ini
    "-ffile-prefix-map=$PROJECT_DIR=."
    "-ffile-prefix-map=${platformio.packages_dir}/framework-arduinoespressif32=/ARDUINO"
```

**Measured** on current `main` (baseline and subject built from detached checkouts, build dirs
wiped, each env built on its own):

| env | delta | where |
|---|---|---|
| `lilygo_330` | **−1,312 B** | all `.flash.rodata` |
| `esp32devkit_330` | **−1,312 B** | all `.flash.rodata` |

`.text`, `.iram0` and `.dram0` are byte-identical across the change - it removes string bytes and
touches nothing else. A census of the built images shows the core-directory strings going
**24 → 0**, each one reappearing as `/ARDUINO/...`.

**The exact figure depends on who builds it, which is the point of the change.** Each of the 24
strings loses the length of the builder's Arduino core directory minus the 8 bytes of `/ARDUINO`,
so the saving is 24 × (that length − 8): about 1.3 kB on a default PlatformIO install, more for a
longer home path. The image tail rounds to 16, and which way it rounds follows the length of the
version string the build embeds, so the same change can show figures 16 B apart between envs or
checkouts. The map matches whether `__FILE__` spells the path with backslashes or forward slashes.

The reproducibility half is checked directly rather than argued: two worktrees whose paths differ
by 24 characters produce images identical in every code and data byte - the four allocated
sections hash the same. What still differs is the app descriptor and the image's own footer: the
descriptor's `project_name` is the project directory's basename, and the ELF sha and the appended
checksum move with it. So this removes the path from the firmware, not the directory name from the
descriptor; that field is metadata, and whether upstream wants it pinned as well is a separate
question this change does not answer.

**Two things that look like tidying and are not, so they do not get "simplified" later:**

**The map is deliberately narrowed to the Arduino package.** pioarduino already maps the IDF tree
and the project directory itself (`-fmacro-prefix-map=<IDF>=/IDF` and `<project>=.`, from
`CONFIG_COMPILER_HIDE_PATHS_MACROS`), which is why the project path was never in an image. Widening
this map to the whole packages directory makes it match the IDF tree too - and
`-ffile-prefix-map` takes precedence over `-fmacro-prefix-map` for the same path **in either
order**, so it replaces IDF's short `/IDF` with a longer prefix across ~150 IDF assert strings and
the image **grows by 1,648 B**. Measured, not predicted.

**Both lines are quoted.** PlatformIO hands `build_flags` to SCons' `shlex.split`. Unquoted, a
packages directory containing a space (`C:\Users\First Last\.platformio`) splits into two tokens
and loses its backslashes, so on such an install the map silently never matches and the stray
second token reaches the compiler as an input filename. The quoting is a verified no-op on Linux:
resolved flags byte-identical, image identical.

There is no functional change and no runtime cost. An assert or `log_e()` that fires now reports
its path as `/ARDUINO/...` instead of the builder's home directory.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**AsyncTCP: the 4 KB stack request is real - proven from the emitted code, and now made explicit**
Branch [`asynctcp-stack-claim`](https://github.com/ekholm/Battery-Emulator/tree/asynctcp-stack-claim) @ `1c792560` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:asynctcp-stack-claim)
The suspicion was that `CONFIG_ASYNC_TCP_STACK_SIZE 4096` never reached the library, leaving its 16 KB default to win. Refuted twice over: `AsyncTCP.h`'s first include is `system_settings.h` itself, and the emitted object code builds the task-creation argument as 4096 (the counterfactual was also built and read). The change makes the ask explicit - `BE_ASYNC_TCP_STACK_SIZE`, mapped onto the library's config name - and adds a text-reading regression test that reddens if either define disappears. It deliberately does not pin which include supplies the value, since two paths do today.

<details>
<summary>PR body it would ship with</summary>

The starting suspicion was that `CONFIG_ASYNC_TCP_STACK_SIZE 4096` in `system_settings.h` was not reaching `AsyncTCP.h` before its `#ifndef` fallback, leaving the task with 16384 bytes of stack instead of the 4096 asked for. That suspicion was refuted.

`AsyncTCP.h`'s very first include is `system_settings.h` itself. The emitted object code was also read: in `_start_asyncsock_task` the stack argument to `xTaskCreateUniversal` is built as `movi.n a12, 1` then `slli a12, a12, 12` = 4096. The counterfactual was also built: commenting out the define and rebuilding gives `slli a12, a12, 14` = 16384.

There are actually two independent include paths that bring `system_settings.h` in before the fallback - the direct include and the chain through `hal.h` into `datalayer.h`. Either is enough. An ordering test that pinned one of them would cry wolf at a tidy-up removing the redundant include while the 4096 stayed intact, so the test was not written that way.

The setting is already working. What was missing was anything that notices if it stops.

The change gives the project's value its own name: `BE_ASYNC_TCP_STACK_SIZE 4096` in `system_settings.h`, mapped onto `CONFIG_ASYNC_TCP_STACK_SIZE`. `AsyncTCP.h` then checks at preprocessor time whether the value it arrived at matches `BE_ASYNC_TCP_STACK_SIZE`, and fails the firmware build with a diagnostic message if not. This fires wherever the setting is lost, by any route.

The host test only checks that the guard is still present, and says in the test text why it does not pin which include supplies the value.

No functional change: the setting already worked. This is the protection it never had.

235 passed / 1 skipped (pre-existing); lilygo_2CAN_330 builds clean.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**GPIO events: each failure names its own component**
Branch [`gpio-event-names`](https://github.com/ekholm/Battery-Emulator/tree/gpio-event-names) @ `630c74e1` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:gpio-event-names)
`alloc_pins()` wrote one shared name pair for both GPIO events, and the message string is read back live on every publish - events page, MQTT, ESP-NOW. So a missing-pin failure after a pin conflict re-pointed the conflict's message at the wrong component, and vice versa. Each event now owns its names; the shared pair is gone rather than left behind.

<details>
<summary>PR body it would ship with</summary>

`EVENT_GPIO_CONFLICT` and `EVENT_GPIO_NOT_DEFINED` both drew from a single pair of name slots written by `alloc_pins()` - one for the component that claimed a pin and one for the component that later tried to claim the same one. Event messages are not stored with the event: `get_event_message_string()` is re-rendered every time any consumer reads it, and the events page, MQTT and ESP-NOW all do. The message therefore reflects whoever wrote those shared slots most recently, not the components the event is actually about.

On a misconfigured board both events are raised and neither is cleared. A pin conflict followed by a missing-pin failure overwrites the conflict's name slots with the missing-pin component, so the conflict event's message names the wrong component for the life of that board. The same happens in reverse.

Each event now has its own name storage, set when the event is raised and written by nothing else. The shared pair is gone rather than left behind - a second event cannot reclaim it.

Verified by measurement rather than by reading: restoring the shared slot turns the two cross-contamination cases red and leaves the positive cases green. Further mutations are caught: swapping the claimant and holder names (the test asserts the full "pin used by 'X' is already allocated by 'Y'" phrase, not merely that both names appear - a swap sends the user to the wrong setting), and freezing names on first write (which pins that this separated two events rather than merely latching each one's first value).

Last-write-wins within one event is a deliberate property of `set_event()`. It is also the correct one here: the events page renders the data field and the message text in adjacent cells of one row, and first-wins names would put the newest failure's pin number beside the first failure's component names in that same row.

238 host tests.

Note: drafted with AI assistance, reviewed by me.
</details>

---

**NeoPixel: `pin` is read before it is ever written - a silent boot loop on the boards where the leftover byte matters**
Branch [`neopixel-uninitialised-pin`](https://github.com/ekholm/Battery-Emulator/tree/neopixel-uninitialised-pin) @ `4f04bbcb` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:neopixel-uninitialised-pin)
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

**The per-test datalayer reset misses `datalayer_extended`, and it is a union**
Branch [`test-extended-datalayer-reset`](https://github.com/ekholm/Battery-Emulator/tree/test-extended-datalayer-reset) @ `a4052a60` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:test-extended-datalayer-reset)
`DataLayerResetListener` gives every test a fresh `datalayer`, fresh events, new driver instances and a re-inited HAL - but not `datalayer_extended`, which is a union across battery types. On a board that costs nothing, because one battery runs. The test binary runs all of them in turn, so a test rendering battery B's page after battery A's test reads A's bytes through B's struct. Found under ASan/UBSan, which is the first time anything has looked.

<details>
<summary>PR body it would ship with</summary>

`DataLayerResetListener` resets `datalayer` and the event state between tests. It does not reset `datalayer_extended`, and that one is a UNION across battery types.

On a real board this costs nothing: one battery runs, so only one union member is ever touched. This binary runs all of them, one after another.

Found by running the host suite under AddressSanitizer and UndefinedBehaviorSanitizer:

```
NISSAN-LEAF-HTML.h:67:54: runtime error: load of value 255, which is not a
valid value for type 'bool'
  #0 NissanLeafHtmlRenderer::get_status_html()
  #1 NissanLeafDtcTests_ShouldDrainReplyLargerThanStorage_Test::TestBody()
```

A gdb watchpoint names the writer: `FordMachEBattery::update_values()` at `FORD-MACH-E-BATTERY.cpp:146` writing its own `fordMachE` union member. The Nissan renderer then loads `nissanleaf.Interlock` out of the same storage.

Two things make this worth fixing rather than noting:

- It is UNDEFINED BEHAVIOUR, not merely a wrong value. A `bool` holding 255 lets the compiler assume either branch.
- **The test PASSED.** It asserts on the DTC count, not on the polluted field, so the suite was green while the run was undefined. The same pollution one field over would be a plausible wrong NUMBER that an assertion accepts.

It is order dependent, which is why nobody has hit it: of six shuffle seeds tried, 1 and 1234 reported and 7, 42, 99999 and 31337 did not.

The union's own comment already says "All zero-initialized entries should go inside this union" - value-initialising the object is exactly that, and is what the listener does for `datalayer` one line above.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Events: hand out the message as bytes, not a String nobody keeps**
Branch [`event-message-bytes`](https://github.com/ekholm/Battery-Emulator/tree/event-message-bytes) @ `6ece2027` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:event-message-bytes)
`get_event_message_string()` returns an Arduino `String` and three of its four callers take `.c_str()` off it on the next token. Event text runs well past the 13 characters below which a String stays inline, so each of those paid a malloc, a copy and a free to reach bytes it then read straight out - measured at 169 allocations across the 170 events the table held at the time. A `snprintf`-contract byte API removes them, and a stack measurement moved 272 bytes off the hot path on the way.

<details>
<summary>PR body it would ship with</summary>

`get_event_message_string()` returns an Arduino `String`, and three of its four callers take `.c_str()` off it on the next token: the MQTT publisher, the ESP-NOW frame, and `set_event()`'s own debug line. Event text runs well over the 13 characters below which a String stays inline, so each of those paid a malloc, a copy and a free to reach bytes it then read straight out. Measured with a probe over every event the table held at the time (170): **169 allocations through the String form, 0 through the new one.**

Adds `size_t get_event_message(EVENTS_ENUM_TYPE, char*, size_t)` on the `snprintf` contract - at most `len - 1` bytes plus a NUL, returning the length the message WOULD have taken, so truncation is detectable rather than silent. The three byte-wanting callers move onto it; the events page keeps the String form, now a thin wrapper rather than a second implementation.

`EVENT_MESSAGE_BUF_SIZE` is 256 because that was measured, not guessed: the longest message measured 173 bytes, the two composed GPIO messages reach 166 with the longest component name in the tree ("Equipment stop button", 21), and a pack suffix adds 12.

The two GPIO messages were the only ones that composed anything, and they did it by concatenating Strings from `Esp32Hal::failed_allocator()` / `conflicting_allocator()`, which returned two String MEMBERS by value. Their format now lives beside the other 168 in the table and the accessors return `const String&` - the names come from a string literal and from the pin map's own `std::string`, so there was never a lifetime that needed the copy.

**A stack finding that came with it.** The debug line's buffer now sits in its own `noinline` function. Left inside `set_event_internal()` it lands in that frame unconditionally, and `set_event()` is called from every driver and safety check in the firmware: measured with `-fstack-usage`, 48 bytes of frame became 320. `DEBUG_PRINTF` is gated at runtime, not compiled out, so the buffer cannot vanish in a release build. Moved out, the common path is 48 bytes again and the 288-byte frame exists only while a newly active event is being logged.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Settings: a save that never reached flash is reported as stored**
Branch [`nvs-full-save-loss`](https://github.com/ekholm/Battery-Emulator/tree/nvs-full-save-loss) @ `26aceef5` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:nvs-full-save-loss)
Every save through `BatteryEmulatorSettingsStore` discarded the result of the underlying write and set `settingsUpdated` regardless. `Preferences::putX()` returns 0 when the NVS call fails, so on a full partition the firmware told the user the setting was stored and to reboot to apply it - and the reboot brought the old value back, with nothing reported anywhere. NVS is log-structured, so an installation that has been reconfigured enough times reaches this on its own.

<details>
<summary>PR body it would ship with</summary>

Every save through `BatteryEmulatorSettingsStore` discarded the result of the underlying write and set `settingsUpdated` regardless. `Preferences::putX()` returns 0 when the NVS call fails, so on a full partition the firmware told the user the setting was stored and to reboot to apply it - and the reboot brought the old value back, with nothing reported anywhere. That is how a device reached `battery=None` across two save-and-reboot rounds with no diagnosis available afterwards.

**This is not a bench-only condition.** NVS is log-structured: an update appends a new entry and marks the old one erased, and reclaiming those erased entries needs a free page to compact into. Once none is free, every save fails the same way.

`EVENT_PERSISTENT_SAVE_INFO` did not cover it and is not overloaded here: it is raised where `settings.begin()` fails, and a full store opens perfectly well. Its message claimed "Namespace full?", which is precisely the cause it cannot detect, so it now says what it does mean. The new `EVENT_PERSISTENT_SAVE_FAILURE` is a **warning, not an error** - the emulator keeps running the battery correctly and only the persistence failed - which still colours the LED and the status page, the signal the silent version lacked.

The level is load-bearing rather than cosmetic, and there is a test that fails if it is raised: `EVENT_LEVEL_ERROR` drives `system_status` to FAULT, and contactor control requests SHUTDOWN after ten seconds of that. At error level this event would disconnect the battery because a setting could not be written.

`removeKey()` and `clearAll()` discarded `remove()`/`clear()` the same way and are fixed with them.

One case is not a simple non-zero test: `putString()` returns the number of bytes written, so a successful save of `""` returns the same 0 that every put returns on failure. Clearing a WiFi password is exactly that write, so it is settled by reading the key back - and the read-back is gated on **what is being written**, not on what is stored. Gating it the other way left the same silent loss alive in one reachable case: clear a password, fill the store, then set a new value, and the failed write reads back as the empty string that was already there and scores a success.

Note: drafted with AI assistance, reviewed by me.

</details>

---

**Build: the sdkconfig rebuild stamp is keyed on checkout time, not content**
Branch [`sdkconfig-mtime-stamp`](https://github.com/ekholm/Battery-Emulator/tree/sdkconfig-mtime-stamp) @ `9ac8da10` · on release `v12.6.0` @ `f7d65fc2` · [diff vs upstream main](https://github.com/dalathegreat/Battery-Emulator/compare/main...ekholm:Battery-Emulator:sdkconfig-mtime-stamp)
pioarduino guards its per-config framework rebuild with a hash whose input is the custom_sdkconfig file's mtime, never its bytes. Every checkout that rewrites the file therefore pays a full framework reinstall plus an IDF-libs rebuild - measured at 2 m 16 s against 27 s settled - for a configuration that has not changed.

<details>
<summary>PR body it would ship with</summary>

pioarduino guards its per-config framework rebuild with a hash whose input is the `custom_sdkconfig` file's **mtime**, never its bytes. Every checkout that rewrites the file pays a full framework reinstall plus an IDF-libs rebuild - measured at 2 m 16 s against 27 s settled - for a configuration that has not changed. Worktrees with identical configuration also carry different stamps: five stamps for two configurations across the live worktrees here.

A `pre:` script sets the file's mtime to a value derived from its content hash, before the framework builder looks. Same bytes, same mtime, same stamp - across checkouts and across worktrees. Measured: one migration rebuild when the normalization first lands, then touching the file with no content change costs a 5.6 s no-op build and zero reinstalls.

The normalizer's one property is pinned by a test: same bytes must mean same mtime, a content change must move it (so the rebuild guard stays alive), and a second run must change nothing - a drifting mtime would reintroduce the per-build reinstall. A constant-stamp mutant fails the content-change case.

**What this deliberately does not fix:** the compiled-libs package is global and unstamped, so two genuinely DIFFERENT configurations still race for it. That needs per-variant libs or a vendored builder, and is platform-side. Until then: one configuration at a time.

Note: drafted with AI assistance, reviewed by me.

</details>

---

*More is in preparation. Ask if it would help to know what is coming.*
