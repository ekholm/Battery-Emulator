#!/usr/bin/env python3
"""Prove the probe-safety checker refuses what it must refuse.

A safety checker that only ever says "safe" is worse than none, because it
launders a judgement nobody made. Each case here hands it a situation whose
right answer is known and requires that answer, including the two ways it could
be wrong in the dangerous direction: calling an actuating pin benign, and
treating a pin whose position the declaration never states as absent.

Usage: python3 tools/test_probe_plan.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import board_gen as bg      # noqa: E402
import board_probe_plan as pp  # noqa: E402

ROOT = Path(__file__).parent.parent
BOARDS = ROOT / 'Software' / 'boards'
failures = []


def load(stem):
    path = BOARDS / f'{stem}.yaml'
    return bg.parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)


def check(case, condition, detail=''):
    if not condition:
        failures.append(f'{case}{": " + detail if detail else ""}')


def main():
    # --- the role classes are the whole basis of the verdict ----------------
    check('a contactor pin is actuating', pp.role_class('contactors', 'contactors.positive') == 'ACTUATING')
    check('a labelled output is actuating', pp.role_class('output', 'output "Output 4"') == 'ACTUATING')
    check('an equipment stop is externally driven',
          pp.role_class('equipment_stop', 'equipment_stop.pin') == 'GUARDED_INPUT')
    # CHAdeMO is signalling, EXCEPT the connector lock, which is a solenoid.
    check('the chademo lock is an actuator', pp.role_class('chademo', 'chademo.lock') == 'ACTUATING')
    check('other chademo lines are inputs', pp.role_class('chademo', 'chademo.pin2') == 'GUARDED_INPUT')
    check('an SPI line is benign to drive', pp.role_class('can', 'can.bus.clk') == 'BENIGN')

    # --- a late-bound pin must not read as an absent one --------------------
    # This is the failure that would matter: lilygo's BMS_POWER is chosen by a
    # user setting, so the declaration cannot name its pad. If that registered
    # as "no such role" the checker would happily certify a probe across it.
    roles, unplaced = pp.pin_roles(load('lilygo'))
    labels = [label for _, label in unplaced]
    check('a `setting` pin is recorded as unplaced, not dropped',
          'contactors.bms_power' in labels, f'unplaced roles seen: {labels}')
    check('an unplaced actuating role blocks every probe',
          pp.unsafe_against({'driven': [1, 2, 3]}, ['lilygo'], {'lilygo': roles}, {'lilygo': unplaced}),
          'a probe on pins lilygo never mentions was certified safe anyway')

    # A board with nothing unplaced and nothing dangerous on the pins in
    # question must still come back clean, or the check above proves nothing.
    dfr_roles, dfr_unplaced = pp.pin_roles(load('dfrobot_edge101'))
    check('a genuinely harmless probe is not refused',
          not pp.unsafe_against({'driven': [5, 12, 18]}, ['dfrobot_edge101'],
                                {'dfrobot_edge101': dfr_roles}, {'dfrobot_edge101': dfr_unplaced}))

    # --- the headline finding, stated as a requirement ----------------------
    # Probing for the Stark's CAN-FD controller drives pins that carry current
    # on other boards. If this ever comes back safe, either a declaration
    # changed or the checker stopped working; both are worth a failed build.
    stark = load('stark')
    devkit_roles, devkit_unplaced = pp.pin_roles(load('devkit'))
    probes = pp.probes_for('stark', stark)
    check('stark has an SPI probe to test at all', probes, 'no SPI CAN probe derived from stark')
    for probe in probes:
        reasons = pp.unsafe_against(probe, ['devkit'], {'devkit': devkit_roles},
                                    {'devkit': devkit_unplaced})
        check('probing stark over devkit is refused', reasons,
              f'pins {probe["driven"]} were certified safe against devkit')
        check('the refusal names an actuating role',
              any(k == 'ACTUATING' for _, _, _, k in reasons),
              f'only these reasons: {reasons}')

    # --- a safe probe still has to be a USEFUL one --------------------------
    # Two boards with the same part behind the same chip select on the same bus
    # answer identically; that probe is harmless and worthless, and the two
    # must not be confused.
    twin = {'driven': [1, 2, 3], 'family': 'spi:mcp2518fd', 'what': 'x'}
    check('a probe both boards answer is rejected as useless',
          pp.indistinct_against(twin, 'a', ['b'], {'b': [dict(twin)]}))
    check('a probe only one board answers is kept',
          not pp.indistinct_against(twin, 'a', ['b'],
                                    {'b': [{'driven': [4, 5, 6], 'family': 'spi:mcp2518fd', 'what': 'y'}]}))

    # --- family placement is a constraint, not a guess ----------------------
    fam, why = pp.family_from_pins({23: [], 25: [], 5: []})
    check('classic-only GPIOs place a board as ESP32', fam == 'ESP32', f'got {fam}')
    fam, _ = pp.family_from_pins({45: [], 5: []})
    check('S3-only GPIOs place a board as ESP32-S3', fam == 'ESP32-S3', f'got {fam}')
    fam, _ = pp.family_from_pins({23: [], 45: []})
    check('contradictory pins place nothing', fam is None, f'got {fam}')
    fam, _ = pp.family_from_pins({5: [], 18: []})
    check('pins common to both place nothing', fam is None, f'got {fam}')

    if failures:
        print(f'probe plan: {len(failures)} FAILED')
        for f in failures:
            print(f'  - {f}')
        sys.exit(1)
    print('probe plan: all cases pass (dangerous probes refused, harmless ones allowed)')


if __name__ == '__main__':
    main()
