# Findings on hardware we do not run

*See also [PATCHES.md](PATCHES.md), for changes that are prepared and ready to use.*

*This is a standing list, not a set of pull requests. Nothing here is filed, and most of it will not
be filed by us: these are integrations we do not own, cannot test, and in several cases cannot even
decide about without someone who has the hardware in front of them.*

*Everything below was read from `upstream/main` at `8e63222e` and each entry names the file and line
so it can be checked in a minute. Where we are confident, the entry says so and what the fix is.
Where two readings are possible, the entry says that instead and asks the question. **Point at
whatever is useful, correct whatever is wrong, ignore the rest.** If an entry turns out to be
mistaken, saying so here is worth more to us than politeness.*

*Our own testing covers the MEB battery and the GoodWe inverter. Everything on this page is outside
that, which is exactly why it is a list of findings rather than a list of fixes.*

---

## Questions we cannot answer without a trace

### Which is byte 0 of frame `0x151`?

Two drivers here read the same byte in incompatible ways.

`SUNGROW-CAN.cpp:457` treats it as a **multiplexer**: `mux == 0` carries manufacturer name
characters 0 to 6 in bytes 1 to 7, `mux == 1` carries characters 7 to 13, so a name longer than
seven characters spans two frames. Its comment says the frame is only sent by the SH15T, an inverter
trying to speak BYD CAN.

`BYD-CAN.cpp` treats bit 0 of that byte as an **identification request flag**: on `u8[0] & 0x01` it
replies with `send_initial_data()`, otherwise it reads the frame as a name.

If the mux reading is the right one, then a `mux == 1` continuation satisfies `1 & 0x01`, so BYD
answers a frame that was not asking and never reads characters 7 to 13 - and a name longer than
seven characters can never be read at all.

**What would settle it:** a capture of `0x151` from a BYD-compatible or Sungrow inverter. Two
specifics: does byte 0 ever take the value `1`, and if so does the next frame continue the same
name? And is your inverter's reported manufacturer name longer than seven characters?

### Where does a Kia/Hyundai hybrid report its 12 V battery?

Every other driver in the Korean family decodes the 12 V lead acid voltage and raises a low-voltage
event. `KIA-HYUNDAI-64-BATTERY.cpp:109-111` reads it from frame `0x596` byte 1 and raises
`EVENT_12V_LOW` below 11.0 V; `HYUNDAI-IONIQ-28-BATTERY.cpp` does the same.

`KIA-HYUNDAI-HYBRID-BATTERY.cpp` decodes no such value and raises no such event. Its frame set is
`0x5F1`, `0x51E`, `0x588`, `0x5AE`, `0x5AF`, `0x5AD`, `0x670`, plus polled rows on `0x7EC`. There is
no `0x596` and no commented out candidate anywhere in the file, so this reads as missing data rather
than an oversight in the code.

**What would settle it:** a CAN capture from a real Kia or Hyundai **hybrid** pack, ideally with the
12 V battery weak, or Hyundai service/PID documentation naming the frame or `0x7EC` PID that carries
the auxiliary battery voltage. Model and year would help.

We have deliberately not guessed at an offset. A plausible looking byte would produce an event that
fires on the wrong data, which is worse than the current silence.

---

## Things we are confident are wrong

### SOLAX: the contactor permission flag is never cleared when the inverter says open

`SOLAX-CAN.cpp:222` sets `inverter_allows_contactor_closing = true` on entry to `CONTACTOR_CLOSED`.
When the inverter then commands the contactor open, the branch below sets `STATE = BATTERY_ANNOUNCE`
and raises `EVENT_INVERTER_OPEN_CONTACTOR` **but does not clear the flag**. The only clear on that
path is `:182`, in `BATTERY_ANNOUNCE`, which runs on the next received frame.

The whole state machine is driven from `map_can_frame_to_variable(rx_frame)`, so "the next frame"
means the next inverter transmission - and an inverter that has just commanded its contactor open is
exactly the one that may stop transmitting. There is no timeout: the only writers are `:19`, `:159`,
`:182`, `:222` and `:290`, and nothing ties the flag to `CAN_inverter_still_alive`. The periodic
refresh at `comm_contactorcontrol.cpp:178` does not apply, because it is gated on
`inverter->controls_contactor()`, which only the three SMA protocols override.

That flag gates precharge (`precharge_control.cpp:56`), contactor open/close
(`comm_contactorcontrol.cpp:237`, `:244`) and roughly fourteen battery drivers.

**Fix:** clear the flag in the open-command branch, before the state change.

### SMA-SBS transmits the raw measured current where every other driver transmits the reported one

`SMA-SBS-BYD-CAN.cpp:51-52` fills `0x4D8` bytes 2 and 3 from
`datalayer.battery.status.current_dA`. Its two siblings fill the same bytes of the same frame from
`reported_current_dA`: `SMA-BYD-H-CAN.cpp:51-52` and `SMA-BYD-HVS-CAN.cpp:55-56`.

It is not only the SMA family. Across the inverter directory roughly two dozen drivers use
`reported_current_dA` for a transmitted current - Pylon, Fox ESS, Sofar, Solax, Growatt, Schneider,
Ferroamp, Sol-Ark, SolXpow, Afore, BYD, Kostal and others. **SMA-SBS is the only one using the raw
field.** The line also carries its own `//Current (TODO: signed OK?)`.

We cannot say from here what an SBS does with the difference, which is why this is on this page and
not in a patch.

### BYD-CAN cannot read any inverter name, and answers frames that did not ask

In the `0x151` case of `BYD-CAN.cpp`:

```c
if ((rx_frame.data.u8[i] > 0x40) && (rx_frame.data.u8[i] > 0x7B)) {  //Filter out invalid chars
  datalayer.system.info.inverter_brand[i] = rx_frame.data.u8[i + 1];
```

Both comparisons are `>`, so the condition reduces to `> 0x7B` - it admits exactly the bytes the
comment says it rejects, and rejects the printable range it was written to accept. The second test
was presumably meant as `< 0x7B`.

Independently, the guard tests `u8[i]` while the body copies `u8[i + 1]`, so the check never governs
the byte actually stored.

Composed, `inverter_brand` is populated only from bytes above `0x7B`, shifted by one - it is never
correct on any input, which is presumably why nobody has reported it. The right fix depends on the
`0x151` question above, which is why the two are on the same page.

### Tesla: `HVP_currentSenseMia` uses a two-bit mask on a one-bit field

`TESLA-BATTERY.cpp:1655` reads

```c
HVP_currentSenseMia = ((rx_frame.data.u8[7] >> 2) & (0x03U));
```

under its own DBC comment describing start bit 58, length 1. The mask covers bits 58 **and** 59, and
bit 59 is `HVP_shuntRefVoltageMismatch`, parsed four lines below at `:1656`. Both are published and
both render on the advanced page (`TESLA-HTML.h:426` and `:428`), so `HVP_currentSenseMia` reads
`Yes` whenever the neighbouring ref-voltage bit is set on its own.

Every sibling MIA field in the same block masks a single bit correctly - `packCurrentMia`,
`auxCurrentMia`, `shuntThermistorMia`, `shuntHwMia`. This one is alone in being wrong. It is a
display defect only: both the local and the datalayer field are `bool`, so nothing is overrun.

**Fix:** `0x03U` becomes `0x01U`, plus a parser test.

### Tesla: four PCS retry counters render through a two-entry table

`TESLA-HTML.h:349`, `:351`, `:353` and `:356` index a two-element `falseTrue[]` with
`PCS_dcdcPrechargeRtyCnt` (3-bit), `PCS_dcdc12VSupportRtyCnt` (4-bit), `PCS_dcdcDischargeRtyCnt`
(4-bit) and `PCS_dcdcPrechargeRestartCnt` (3-bit). These are counts, so one retry has always
displayed as `True` and higher counts had no meaning at all.

The honest render is the number, and the labels already say "Rty Cnt". We have not changed it,
because it also changes what 0 and 1 display and a Tesla owner should decide that.

### TESLA-LEGACY reports a 100 kWh pack as 70 kWh

In the `battery_hwID` switch at `TESLA-LEGACY-BATTERY.cpp:43-93`, five of six groups have a comment
that matches the value they set: `//60kWh` sets 60000, `//70kWh` 70000, `//75kWh` 75000, `//85kWh`
85000, `//90kWh` 90000. The sixth, `//100kWh` at `:88-90` covering hwIDs 79 and 89, sets **70000** -
which is exactly the value of the `//70kWh` group four cases above.

The whole switch arrived in one commit and the line has never been touched since.

**We have not changed it, and the reason is worth stating:** the current value errs in the
conservative direction. Under-reporting capacity makes a pack look like it holds less than it does;
over-reporting would make SOC read low and invite over-discharge. So we would rather hear from
someone with a legacy 100 kWh pack (P100D, within this driver's range) confirming what hwID their
BMS reports, than flip it on a pattern argument alone.

### A double Tesla setup corrupts the first battery's page

`TESLA-BATTERY.cpp` writes `datalayer_extended.tesla.*` at 483 sites with no per-instance pointer,
and both constructors run that code. With a second Tesla battery configured, the two instances
interleave into one struct and the first battery's advanced page shows a mixture of both.

The other shared-datalayer integrations avoid this by construction: `ECMP-BATTERY.h:15` sets
`datalayer_ecmp = NULL` in the second-instance constructor and the renderer guards on it. Renault
Zoe Gen2 does the same. Tesla has no such mechanism.

### `CmpSmartCar` has an unreachable case in the triple-battery switch

`battery_supports_triple()` lists five types; the battery3 construction switch has six cases. The
extra is `CmpSmartCar`, and its case cannot be reached because the guard above rejects the type first
and logs "User tried enabling triple battery on non-supported integration!".

The file states the invariant itself, on both predicates: *"Must match the switch in setup_battery()
below."* The double pair does match, 18 to 18. The triple pair does not.

Harmless today - the user gets a correct refusal - but the comment claims a property the code does
not have, and nothing tests it. A host test extracting both label sets and asserting equality would
catch it and any future drift; it needs to handle case fallthrough, which is what defeated two
hand-reads on our side.

### The shunt's deci-amp field is dead, and two consumers re-derive it

`datalayer.shunt.measured_amperage_dA` has two writers (`BMW-SBOX.cpp:41`, `BYD-CAN.cpp:134`) and no
readers. Meanwhile `CHARGEBYTE-CCS.cpp:123` computes `measured_amperage_mA / 100` and
`KOSTAL-RS485.cpp:133` computes `(measured_amperage_mA / 100) / 10` - both want the deci-amp value
and each re-derives the conversion the shunt driver already performed one field over.

Two things travel with it. The narrowing at `BMW-SBOX.cpp:41` has no range check, from a frame
carrying 24 signed bits of milliamps; the field is `uint16_t` (`datalayer.h:285`), so it currently
wraps for every discharge current. And `BYD-CAN.cpp:128-130`, in inverter-as-shunt mode, sets
`available`, `precharging` and `contactors_engaged` and never clears any of them, where `BMW-SBOX`
runs a full lifecycle with a 1000 ms last-seen timeout. Nothing reachable pairs them today, because
BYD-CAN and Kostal are both inverters and mutually exclusive - but `contactors_engaged` and
`precharging` already have readers at `KOSTAL-RS485.cpp:136` and `:142`, so the protection is the
pairing, not the absence of readers.

---

## Reported to us, not re-verified here

Listed so they are not lost, and marked so nobody treats them as checked. These came from a
characterization-test pass and were confirmed at the source by its reviewer, but the desk has not
re-derived them and two rest on vendor documentation we do not hold.

- **GROWATT-LV** Ah capacity scaling: `GROWATT-LV-CAN.cpp:18-22` and `:72-76` multiply by 100 with a
  worked comment; the report says the field's documented unit wants a factor of 10. We cannot check
  the unit without the Growatt protocol document.
- **SOFAR** "100 % SoC means discharge only" branch appears unreachable, because the spoofed SoC is
  capped at 9900 first (`SOFAR-CAN.cpp:29-85`).
- **SOFAR** `0x35A` last-payload statics are function-local and survive driver re-instantiation
  (`SOFAR-CAN.cpp:219-247`).

---

*Note: this page is maintained with AI assistance and reviewed by me before publishing.*
