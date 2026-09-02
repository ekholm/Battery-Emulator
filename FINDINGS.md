# Findings on hardware we do not run

*See also [FIXES.md](FIXES.md) for defect repairs ready to merge, and [FEATURES.md](FEATURES.md) for larger changes.*

*This is a short list on purpose. It holds only things we have found, cannot fix ourselves, and
cannot settle without someone who has the hardware in front of them. Anything we have actually
fixed belongs on the patch shelf instead, and anything we are still arguing about internally is not
your problem.*

*Everything below was read from `upstream/main` at `8e63222e` and names the file and line so it can
be checked in a minute. **Corrections are more useful to us than agreement** - if an entry is wrong,
saying so costs you a sentence and saves us a wrong fix.*

*Our own testing covers the MEB battery and the GoodWe inverter. Everything here is outside that,
which is exactly why it is a question rather than a patch.*

---

## Which is byte 0 of frame `0x151`?

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

---

## Where does a Kia/Hyundai hybrid report its 12 V battery?

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

## Does a legacy Tesla 100 kWh pack really report hwID 79 or 89?

In the `battery_hwID` switch at `TESLA-LEGACY-BATTERY.cpp:43-93`, five of six groups set a capacity
matching their own comment: `//60kWh` sets 60000, `//70kWh` 70000, `//75kWh` 75000, `//85kWh` 85000,
`//90kWh` 90000. The sixth, `//100kWh` at `:88-90` covering hwIDs 79 and 89, sets **70000** - which
is exactly the value of the `//70kWh` group four cases above. The whole switch arrived in one commit
and the line has never been touched since, so this looks like a copy-paste.

**We have not changed it, and the reason is the interesting part.** The current value errs in the
conservative direction: under-reporting capacity makes a pack look like it holds less than it does,
whereas over-reporting would make SOC read low and invite over-discharge. So if hwIDs 79 and 89 ever
turn out to cover a 70 kWh variant as well, "fixing" this would be the more dangerous choice.

**What would settle it:** anyone running a legacy 100 kWh pack (P100D, within this driver's
2012-2020 range) telling us what hwID their BMS reports. One number closes it.

---

*Note: this page is maintained with AI assistance and reviewed before publishing.*
