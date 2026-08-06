# Golden CAN traces for the contactor equivalence tests

These files record the frames the **pre-refactor** contactor implementations put
on the wire for each scenario in `test/contactor_equivalence_tests.cpp`. The
tests replay the same scenarios against the current code and assert the frames
are byte-identical, which is what turns "behaviour-identical" from a claim into
a check.

They exist because neither the BMW i3 nor the BMW S-BOX has hardware in this
project or a CAN log in `test/can_log_based/can_logs/`, so nothing else covers
those drivers. A capture-compare trace needs neither.

Format: one frame per line, `ID b0 b1 ...` in hex, filtered to the single CAN ID
the contactor logic controls (`0x10B` for the i3 command, `0x100` for the S-BOX
relay command). Alive counters and CRCs are included deliberately - a changed
transmit cadence should fail these tests, not slip through.

## Regenerating

Only needed when a scenario is added or changed. Take the generator from the
commit that introduced these traces (`test/contactor_trace_gen.cpp`), apply it
to a worktree at the reference revision, build and run:

```bash
git worktree add --detach ../be-wt/goldens <reference-revision>
cp test/contactor_trace_gen.cpp ../be-wt/goldens/test/
# register it in that worktree's test/CMakeLists.txt, then:
cd ../be-wt/goldens/test && cmake -B build . && cmake --build build -j
cd build && ./tests --gtest_filter='ContactorTraceGen.*'
cp contactor_traces/*.trace <this-repo>/test/contactor_traces/
```

The reference revision for the current traces is upstream `9e1cdd2d`.

**Do not regenerate to make a failing test pass.** A diff means the refactor
changed what the hardware sees. Either fix the code, or - if the change is
intended - assert the divergence explicitly, the way
`I3EquipmentStopReleasesTheCloseCommand` does.

## Known intentional divergence

`i3_equipment_stop` is the one trace the current code does **not** reproduce
exactly, on purpose. Pre-refactor, the i3 had no equipment-stop handling in its
contactor command at all: it kept commanding close for the whole stop and relied
on the wakeup pin dropping to put the pack to sleep. The shared FSM gates every
topology on `equipment_stop_active`, so the command is now released - one tick
(20 ms) after the stop, because the COMPLETED -> DISCONNECTED transition returns
before the actuator opens. The test pins both halves: everything before the stop
is identical, and after it exactly one nibble changes.
