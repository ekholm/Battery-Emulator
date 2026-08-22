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
    labels = [label for _, label, _ in unplaced]
    check('a `setting` pin is recorded as unplaced, not dropped',
          'contactors.bms_power' in labels, f'unplaced roles seen: {labels}')
    check('an unplaced actuating role blocks every probe',
          pp.unsafe_against({'driven': [1, 2, 3]}, ['lilygo'], {'lilygo': roles}, {'lilygo': unplaced}),
          'a probe on pins lilygo never mentions was certified safe anyway')

    # --- virgin-config mode -------------------------------------------------
    # The mode's whole claim is that a `setting` role has no pad before anything
    # is stored. Its danger is obvious: excuse one role too many and the checker
    # certifies a probe against a live contactor. So each case below pins one
    # side of the line, and the two that matter are the refusals.
    check('a setting-bound role is vacuous under virgin config', pp.virgin_vacuous('setting'))
    check('a variant-bound role is NOT vacuous under virgin config',
          not pp.virgin_vacuous('variant'),
          'a variant pad is decided by the hardware; virgin NVS does not move it')

    # lilygo's blockers are all `setting`, so virgin config should clear them...
    check('virgin config clears lilygo\'s setting-bound unplaced roles',
          not pp.unsafe_against({'driven': [1, 2, 3]}, ['lilygo'], {'lilygo': roles},
                                {'lilygo': unplaced}, virgin=True))

    # ...but a PLACED actuating pin is a pin whatever the configuration says.
    # This is the mutation the item names: virgin mode must not touch it.
    placed_actuating = [g for g, rs in roles.items()
                        if any(pp.role_class(f, l) == 'ACTUATING' for f, l in rs)]
    check('lilygo has a placed actuating pin to test against', placed_actuating)
    check('virgin config does NOT excuse a PLACED actuating conflict',
          pp.unsafe_against({'driven': placed_actuating[:1]}, ['lilygo'], {'lilygo': roles},
                            {'lilygo': unplaced}, virgin=True),
          f'driving GPIO {placed_actuating[:1]} must still refuse under virgin config')

    # And the unsafe direction of the mode itself: a variant-bound actuating
    # role must keep blocking, or the mode would certify a probe against a
    # contactor whose pad the declaration simply never stated.
    variant_actuating = {'x': [('contactors', 'contactors.positive', 'variant')]}
    check('virgin config does NOT excuse a variant-bound actuating role',
          pp.unsafe_against({'driven': [99]}, ['x'], {'x': {}}, variant_actuating, virgin=True),
          'a variant pad is real hardware; only stored config can be absent')

    # A board with nothing unplaced and nothing dangerous on the pins in
    # question must still come back clean, or the check above proves nothing.
    dfr_roles, dfr_unplaced = pp.pin_roles(load('dfrobot_edge101'))
    check('a genuinely harmless probe is not refused',
          not pp.unsafe_against({'driven': [5, 12, 18]}, ['dfrobot_edge101'],
                                {'dfrobot_edge101': dfr_roles}, {'dfrobot_edge101': dfr_unplaced}))

    # --- the classes must refuse ALONE, and driven sets must be
    # complete ---------------------------------------------------------------
    # Every refusal in today's report happens to list an ACTUATING or UNPLACED
    # reason beside any GUARDED_INPUT one, so a regression that stopped
    # refusing on guarded inputs alone would change no verdict and fail no
    # case. Devkit's GPIO 12 is only its equipment stop: a probe driving just
    # that pin must be refused for exactly that reason.
    devkit_roles, devkit_unplaced = pp.pin_roles(load('devkit'))
    check('a guarded input alone refuses a probe',
          any(k == 'GUARDED_INPUT' for _, _, _, k in
              pp.unsafe_against({'driven': [12]}, ['devkit'], {'devkit': devkit_roles},
                                {'devkit': devkit_unplaced})),
          'driving an equipment-stop input was certified safe')

    # A probe asserts its own chip select, so a `variant` cs means the driven
    # set cannot be stated - the probe must not be emitted at all rather than
    # emitted with a pad missing.
    threelb = load('3lb')
    with_late_cs = dict(threelb)
    with_late_cs['can'] = [dict(i) for i in threelb['can']]
    for inst in with_late_cs['can']:
        if inst.get('driver') == 'mcp2515':
            inst['cs'] = 'variant'
    before = {p['what'] for p in pp.probes_for('3lb', threelb)}
    after = {p['what'] for p in pp.probes_for('3lb', with_late_cs)}
    check('a late-bound chip select suppresses the probe',
          'mcp2515 on bus SPI1' in before and 'mcp2515 on bus SPI1' not in after,
          f'probes before={before} after={after}')

    # The Edge101's PHY answers MDIO only while its power switch is asserted,
    # so the probe drives three pins, not two - an undercounted driven set is
    # how an unsafe probe gets certified.
    eth_probes = pp.probes_for('dfrobot_edge101', load('dfrobot_edge101'))
    check('the MDIO probe counts the PHY power pin as driven',
          any(p['kind'] == 'mdio' and p['driven'] == [2, 4, 13] for p in eth_probes),
          f'got {eth_probes}')

    # --- the headline finding, stated as a requirement ----------------------
    # Probing for the Stark's CAN-FD controller drives pins that carry current
    # on other boards. If this ever comes back safe, either a declaration
    # changed or the checker stopped working; both are worth a failed build.
    stark = load('stark')
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

    # --- the free facts come from the declarations, for every board ---------
    # Grouping is only as good as these. A board that no env builds still has a
    # chip and a flash size, because both are declared and board_gen checks
    # them against platformio.ini wherever an env exists to check against - so
    # nothing lands in a group by default or goes unplaced.
    data = {b: load(b) for b in ('3lb', 'stark', 'waveshare')}
    facts = pp.board_facts(list(data), data)
    check('a declared board is matched to the env that builds it',
          facts['3lb']['env'] == '3lb_330', f'got {facts["3lb"]}')

    # The property is that the declaration alone is enough to place a board, so
    # it must not be tested by pointing at whichever board happens to be
    # unbuilt: this used to name the 3LB, and adding the env it had been
    # missing since 2024 turned the case green-by-accident into a failure.
    unbuilt = dict(load('3lb'))
    unbuilt[bg.MACRO_KEY] = 'HW_NO_ENV_DEFINES_THIS'
    orphan = pp.board_facts(['3lb'], {'3lb': unbuilt})['3lb']
    check('a board with no env is still placed',
          orphan['env'] is None and orphan['family'] == 'ESP32' and orphan['flash_mb'] == 4,
          f'got {orphan}')
    check('a declared S3 board reads as one', facts['waveshare']['family'] == 'ESP32-S3')
    check('flash size comes through', facts['stark']['flash_mb'] == 8, f'got {facts["stark"]}')

    # Boards of different families are never in contention: one image cannot
    # span them, so a probe for one can never run on the other.
    groups = pp.split_free(list(data), facts)
    for members in groups.values():
        families = {facts[b]['family'] for b in members}
        check('a group never mixes chip families', len(families) == 1, f'got {families}')
    check('stark and 3lb are separated by flash size alone',
          all(not {'stark', '3lb'} <= set(m) for m in groups.values()))

    if failures:
        print(f'probe plan: {len(failures)} FAILED')
        for f in failures:
            print(f'  - {f}')
        sys.exit(1)
    print('probe plan: all cases pass (dangerous probes refused, harmless ones allowed)')


if __name__ == '__main__':
    main()
