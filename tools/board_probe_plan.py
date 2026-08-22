#!/usr/bin/env python3
"""Can a board work out at runtime which board it is, and is it safe to try?

A one-image build has to know its pin map before it can drive anything, and the
tempting shortcut is to find out by asking the hardware: reset the SPI CAN
controller and see whether a known register reads back, scan the i2c bus for the
display, fall through to the next candidate when nothing answers.

The probe that SUCCEEDS is not the problem. The problem is every probe before
it, because those run on the wrong board by definition - that is what a cascade
is - and a probe drives pins. On this device the pins in question are contactor
drives, precharge, battery wake-up and labelled power outputs, so "drive these
three GPIOs and see what answers" can mean "assert the positive contactor of a
board you have not identified yet". Chip select idles HIGH for the duration of a
probe rather than pulsing, and `NCCONTACTOR` lets an installation invert
contactor logic, so neither rail is reliably the harmless one.

This works that out from the declarations instead of from judgement. Every
board's pin roles are already declared; so is every board's SPI and i2c wiring.
That is enough to answer three questions mechanically:

  1. Which boards can even be confused? Chip family is a compile-time choice
     (the toolchain differs), so a single image only ever contends within one
     family. Flash size and PSRAM are free to read and risk nothing. The first
     two are declared and cross-checked against platformio.ini by board_gen;
     PSRAM is only in the build, so a board no env builds is taken as not
     having it.
  2. Of the probes that would separate what is left, which are SAFE - meaning
     no pin the probe DRIVES is an actuating or externally-driven role on any
     board still in contention?
  3. Using only the safe ones, does the cascade actually finish? An unsafe
     probe is not the only bad outcome; a safe cascade that cannot tell two
     boards apart is a dead end worth knowing about before it is built.

The output is a checked-in report so that a pin change which makes a probe
unsafe, or which collapses a distinction the cascade relied on, shows up as a
diff rather than as a surprise on a bench.

Usage: python3 tools/board_probe_plan.py            rewrite the report
       python3 tools/board_probe_plan.py --check    fail if it is out of date
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import board_gen as bg  # noqa: E402  - the declarations have one parser

ROOT = Path(__file__).parent.parent
REPORT = ROOT / 'Software' / 'boards' / 'probe-plan.md'
PLATFORMIO = ROOT / 'platformio.ini'

# ---------------------------------------------------------------------------
# What a pin is, for the purpose of being driven by a probe
# ---------------------------------------------------------------------------
# The split is the one board_gen's pin-conflict check already draws, for the
# same underlying reason: some features own a pad outright and some of them
# move real-world power.
#
# ACTUATING - driving this pin does something physical. A contactor coil, a
# precharge relay, the battery's wake-up line, a labelled product output. These
# are the pins that make a blind cascade unacceptable rather than untidy.
#
# GUARDED_INPUT - the board drives this pin from outside: an equipment-stop
# input, a CHAdeMO handshake line, the AP button. Driving it fights whatever is
# out there, which is a contention short at worst and a defeated safety input
# at best. Reading such a pin is fine; a probe only ever reads MISO.
#
# Everything else - a transceiver, an SD line, a display bus, an RMII pin, an
# LED - is BENIGN to drive: nothing downstream of it moves current on its own.
ACTUATING = ('contactors', 'precharge_auto', 'sma', 'battery_wakeup', 'output')
GUARDED_INPUT = ('equipment_stop', 'chademo', 'ap_button')
# CHAdeMO is mostly handshake signalling, but the connector lock is a solenoid.
ACTUATING_ROLES = ('chademo.lock',)


def role_class(feature, label=''):
    if label in ACTUATING_ROLES or feature in ACTUATING:
        return 'ACTUATING'
    if feature in GUARDED_INPUT:
        return 'GUARDED_INPUT'
    return 'BENIGN'


def pin_roles(data):
    """Every GPIO the declaration accounts for, and what it is there for, plus
    the roles whose GPIO the declaration does NOT state.

    That second half is the part that matters here. A pin declared `setting` or
    `variant` is chosen at runtime, so the declaration cannot say which pad it
    lands on - and a role whose pad is unknown could be ANY pad. Treating those
    as simply absent is what a safety check must not do: it would let a board
    whose contactor pin is user-selectable read as having no contactor pin at
    all, and certify a probe as safe across it. They are recorded as unplaced
    instead, and an unplaced ACTUATING role makes the board uncertifiable.

    Each unplaced role also carries HOW it is bound, because the two kinds are
    not equally unknowable. A `setting` role is resolved from stored config; a
    `variant` role is resolved from the hardware itself. Only the first can be
    genuinely absent - see virgin_vacuous()."""
    out = {}
    unplaced = []

    def claim(gpio, feature, label):
        pads = bg.candidate_pads(gpio)
        if str(gpio).isdigit():
            out.setdefault(int(gpio), []).append((feature, label))
        elif pads:
            # Placed on a finite candidate set: the role could be on any of
            # them, so it counts at EVERY candidate - a probe touching any
            # candidate pad must reason about the role - and is NOT unplaced.
            for pad in pads:
                out.setdefault(pad, []).append((feature, label))
        elif str(gpio) in bg.LATE_BOUND:
            unplaced.append((feature, label, str(gpio)))

    for feature in bg.FEATURE_ORDER:
        try:
            declared = bg.instances_of(data, feature)
        except bg.DeclError:
            continue
        for spec, index, inst, driver in declared:
            if index >= len(spec['instances']):
                continue
            _, bus = bg.bus_of(data, inst, feature)
            if bus and 'bus' in spec and index < len(spec['bus']):
                for role in spec['bus'][index]:
                    claim(bus.get(role), feature, f'{feature}.bus.{role}')
            for role in spec['instances'][index]:
                claim(inst.get(role), feature, f'{feature}.{role}')
    for out_spec in data.get('outputs', []):
        claim(out_spec.get('gpio'), 'output', f'output "{out_spec.get("label")}"')
    return out, unplaced


# ---------------------------------------------------------------------------
# The free discriminators: things a board can read about itself at no risk
# ---------------------------------------------------------------------------

FAMILY = {'esp32': 'ESP32', 'esp32s3': 'ESP32-S3'}


def board_facts(boards, data_by_board):
    """What each board can read about itself for free.

    Chip and flash come from the declaration - board_gen validates both and
    cross-checks them against platformio.ini, so they hold for boards no env
    builds too. PSRAM has no declaration, so it comes from the build; a board
    with no env is taken as not having it, which is true of every such board
    today and is stated in the report rather than assumed silently."""
    from_ini = platformio_facts()
    out = {}
    for board in boards:
        data = data_by_board[board]
        macro = data.get(bg.MACRO_KEY)
        built = from_ini.get(macro)
        out[board] = {
            'env': built['env'] if built else None,
            'family': FAMILY[data['chip']],
            'flash_mb': int(str(data['flash_mb'])),
            'psram': bool(built and built['psram']),
        }
    return out


def platformio_facts():
    """chip family, flash size and PSRAM per build macro, from platformio.ini.

    These cost nothing to read at runtime and drive no pins, so they come first
    in any cascade. Chip family is stronger than the others: the toolchain
    differs between ESP32 and ESP32-S3, so one image cannot span the two at all
    and boards of the other family are never in contention to begin with."""
    ini = PLATFORMIO.read_text(encoding='utf-8')
    commons = {m.group(1): m.group(2)
               for m in re.finditer(r'^\[(common_\w+)\]([\s\S]*?)(?=^\[|\Z)', ini, flags=re.M)}
    facts = {}
    for block in re.split(r'^\[env:', ini, flags=re.M)[1:]:
        env, body = block.split(']')[0], block.split('\n[', 1)[0]
        macros = re.findall(r'-D (HW_[A-Z0-9_]+)', body)
        if not macros:
            continue
        extends = re.search(r'extends\s*=\s*(\S+)', body)
        base = commons.get(extends.group(1), '') if extends else ''
        board_json = re.search(r'^board\s*=\s*(\S+)', body, re.M) or re.search(r'^board\s*=\s*(\S+)', base, re.M)
        flash = re.search(r'flash_size\s*=\s*(\d+)MB', body) or re.search(r'flash_size\s*=\s*(\d+)MB', base)
        if flash:
            flash_mb = int(flash.group(1))
        elif board_json and 'flash_16MB' in board_json.group(1):
            flash_mb = 16
        else:
            flash_mb = 4  # the esp32dev default these envs inherit
        facts[macros[0]] = {
            'env': env,
            'family': 'ESP32-S3' if (extends and 's3' in extends.group(1)) else 'ESP32',
            'flash_mb': flash_mb,
            # PSRAM is only present where the env asks for it.
            'psram': 'psram' in (body + base).lower(),
        }
    return facts


# ---------------------------------------------------------------------------
# The probes
# ---------------------------------------------------------------------------
# A probe is "drive these pins, read that one, and see whether the part
# answers". Only the DRIVEN pins can do harm; MISO is an input on our side and
# reading it costs nothing whatever the other board thinks that pad is for.

def probes_for(board, data):
    out = []
    for feature, driver_names in (('can', ('mcp2515', 'mcp2518fd')), ('sd_spi', None)):
        try:
            declared = bg.instances_of(data, feature)
        except bg.DeclError:
            continue
        for spec, index, inst, driver in declared:
            if driver_names and driver not in driver_names:
                continue
            bus_name, bus = bg.bus_of(data, inst, feature)
            if not bus:
                continue
            # A probe cannot be certified if its own select pad is unknown:
            # asserting chip select IS driving a pin, and a `variant` cs could
            # be any pad. No probe is better than a probe whose driven set is
            # missing an entry.
            if not str(inst.get('cs')).isdigit():
                continue
            driven, read = [], []
            for role in ('clk', 'mosi'):
                if str(bus.get(role)).isdigit():
                    driven.append(int(bus[role]))
            if str(bus.get('miso')).isdigit():
                read.append(int(bus['miso']))
            driven.append(int(inst['cs']))
            if len(driven) < 2:
                continue
            out.append({'kind': 'spi', 'what': f'{driver or feature} on bus {bus_name}',
                        'family': f'spi:{driver or feature}',
                        'driven': sorted(set(driven)), 'read': read})
    try:
        for spec, index, inst, driver in bg.instances_of(data, 'display_i2c'):
            driven = [int(inst[r]) for r in ('sda', 'scl') if str(inst.get(r)).isdigit()]
            if len(driven) == 2:
                # Open-drain and pulled up, so a probe only ever pulls LOW. That
                # is gentler than a push-pull SPI line but not harmless: with
                # NCCONTACTOR set, LOW is the asserting level.
                out.append({'kind': 'i2c', 'what': 'i2c device scan',
                            'family': 'i2c', 'driven': sorted(driven), 'read': []})
    except bg.DeclError:
        pass
    try:
        for spec, index, inst, driver in bg.instances_of(data, 'ethernet'):
            # The PHY answers MDIO only while its power switch is asserted, so
            # a probe drives that pin too - it belongs in the driven set.
            driven = [int(inst[r]) for r in ('mdc', 'mdio', 'power') if str(inst.get(r)).isdigit()]
            if len(driven) >= 2:
                out.append({'kind': 'mdio', 'what': f'{driver} PHY over MDIO',
                            'family': f'mdio:{driver}', 'driven': sorted(driven), 'read': []})
    except bg.DeclError:
        pass
    return out


def virgin_vacuous(binding):
    """Is this late-bound role genuinely absent on a device with virgin config?

    `setting` roles get their pad from stored configuration. Before anything is
    stored - factory state, first boot, the exact moment a cascade would run -
    there is no assignment, so the role occupies no pad at all and cannot be
    driven by accident. It is vacuous, not unknown.

    `variant` roles are NOT vacuous: the pad is decided by which hardware
    variant this is, which no amount of virgin NVS changes. Treating the two
    alike would be the unsafe direction of this whole idea - it would certify a
    probe against a contactor pin that is really there, just undeclared.
    """
    return binding == 'setting'


def unsafe_against(probe, candidates, roles, unplaced, virgin=False):
    """Why this probe must not be run while `candidates` are still possible.

    With `virgin`, config-bound roles are skipped per virgin_vacuous(); every
    PLACED role is judged exactly as before, because a declared pad is a pad
    whatever the configuration says."""
    reasons = []
    for other in candidates:
        for gpio in probe['driven']:
            for feature, label in roles[other].get(gpio, []):
                klass = role_class(feature, label)
                if klass != 'BENIGN':
                    reasons.append((other, gpio, label, klass))
        for feature, label, binding in unplaced[other]:
            if virgin and virgin_vacuous(binding):
                continue
            if role_class(feature, label) != 'BENIGN':
                reasons.append((other, None, label, 'UNPLACED'))
    return reasons


def indistinct_against(probe, board, candidates, devices):
    """Candidates that would answer this probe the same way `board` does.

    A probe is only worth running if the part responds here and does not
    respond there. Two boards that put a controller of the same family behind
    the same chip select on the same bus lines answer identically, so the
    probe says "a chip is present" - which was never in doubt - rather than
    which board this is."""
    same = []
    for other in candidates:
        for dev in devices[other]:
            if dev['driven'] == probe['driven'] and dev['family'] == probe['family']:
                same.append((other, dev['what']))
    return same


# ---------------------------------------------------------------------------
# The cascade
# ---------------------------------------------------------------------------

def split_free(candidates, facts):
    """Partition by the discriminators that drive no pins at all."""
    groups = {}
    for board in candidates:
        f = facts[board]
        groups.setdefault((f['family'], f['flash_mb'], f['psram']), []).append(board)
    return groups


def resolve(group, probes, roles, unplaced, log, virgin=False):
    """Narrow `group` using only probes that are safe against it.

    Safety is judged against THE GROUP, not against every board that exists:
    once the free discriminators have ruled a board out, a probe can no longer
    run on it, so its pins stop mattering. That is the whole reason to order a
    cascade free-first - it is what turns some unsafe probes into safe ones."""
    if len(group) < 2:
        return [list(group)]
    for board in sorted(group):
        for probe in probes[board]:
            others = [b for b in group if b != board]
            reasons = unsafe_against(probe, others, roles, unplaced, virgin)
            if reasons:
                log.append(('rejected', board, probe, reasons))
                continue
            same = indistinct_against(probe, board, others, probes)
            if same:
                log.append(('indistinct', board, probe, same))
                continue
            log.append(('safe', board, probe, []))
            rest = resolve(others, probes, roles, unplaced, log, virgin)
            return [[board]] + rest
    log.append(('stuck', None, None, sorted(group)))
    return [sorted(group)]


def render(boards, facts, macros, roles, unplaced, probes):
    L = []
    add = L.append
    add('<!-- GENERATED by tools/board_probe_plan.py - do not edit. '
        'Rebuild with `python3 tools/board_probe_plan.py`. -->')
    add('# Runtime board detection: what is safe to probe, and whether it is enough')
    add('')
    add('Derived from `Software/boards/*.yaml` and `platformio.ini`. See the header of')
    add('`tools/board_probe_plan.py` for why this is computed rather than argued.')
    add('')
    verdict_at = len(L)
    add('')
    add('## Boards, and the facts that cost nothing to read')
    add('')
    add('| board | build macro | env | chip family | flash | PSRAM |')
    add('|---|---|---|---|---|---|')
    for b in boards:
        f = facts[b]
        env = f'`{f["env"]}`' if f['env'] else '**none**'
        add(f'| {b} | `{macros[b]}` | {env} | {f["family"]} | {f["flash_mb"]} MB | '
            f'{"yes" if f["psram"] else "no"} |')
    add('')
    for b in [b for b in boards if facts[b]['env'] is None]:
        add(f'> **`{b}` has a declaration and a header (`hal.cpp` switches on `{macros[b]}`) but no '
            '`platformio.ini` env, so nothing builds it today.** Its chip and flash come from the')
        add('> declaration, which is the case for declaring them rather than reading the build; its '
            'PSRAM does not, so it is taken as having none. Still a gap worth closing: a board the')
        add('> tree can select but cannot build is one nothing else checks.')
        add('')

    add('## Which boards can be confused with which')
    add('')
    add('Chip family is a compile-time choice - the toolchain differs - so one image never spans')
    add('both. Flash size and PSRAM are read at runtime and drive no pins, so they are free.')
    add('')
    groups = split_free(boards, facts)
    for key, members in sorted(groups.items(), key=lambda kv: str(kv[0])):
        label = f'{key[0]}, {key[1]} MB flash, PSRAM {"yes" if key[2] else "no"}'
        if len(members) == 1:
            add(f'- **{label}** → `{members[0]}` alone. Nothing to probe for.')
        else:
            add(f'- **{label}** → `{"`, `".join(sorted(members))}` still in contention.')
    add('')

    add('## Probes, and whether they may be run')
    add('')
    add('A probe drives some pins and reads one. Only the DRIVEN pins can do harm - MISO is an')
    add('input on our side, so reading it is free whatever the other board calls that pad.')
    add('')
    logs = []
    outcome = {}
    for key, members in sorted(groups.items(), key=lambda kv: str(kv[0])):
        if len(members) < 2:
            continue
        log = []
        result = resolve(sorted(members), probes, roles, unplaced, log)
        logs.append((key, members, log, result))
        outcome[key] = result

    if not logs:
        add('Every group is already a single board. No probe is needed anywhere.')
        add('')
    for key, members, log, result in logs:
        label = f'{key[0]}, {key[1]} MB, PSRAM {"yes" if key[2] else "no"}'
        add(f'### Contending: `{"`, `".join(sorted(members))}` ({label})')
        add('')
        for kind, board, probe, detail in log:
            if kind == 'safe':
                add(f'- **SAFE** — probe `{board}` for {probe["what"]}: drives GPIO '
                    f'{", ".join(str(p) for p in probe["driven"])}'
                    + (f', reads {probe["read"][0]}' if probe['read'] else '') + '.')
                add('  No driven pin is an actuating or externally-driven role on any board still in')
                add('  contention here.')
            elif kind == 'rejected':
                add(f'- **REFUSED** — probe `{board}` for {probe["what"]} would drive GPIO '
                    f'{", ".join(str(p) for p in probe["driven"])}:')
                seen = set()
                for other, gpio, role_label, klass in detail:
                    line = (f'  - on `{other}`, the declaration does not say which pad `{role_label}` '
                            f'lands on — **UNPLACED**, so no pin can be certified'
                            if gpio is None else
                            f'  - on `{other}`, GPIO {gpio} is `{role_label}` — **{klass}**')
                    if line not in seen:
                        seen.add(line)
                        add(line)
            elif kind == 'indistinct':
                add(f'- **USELESS** — probe `{board}` for {probe["what"]} on GPIO '
                    f'{", ".join(str(p) for p in probe["driven"])} is safe, but answers the same on '
                    + ', '.join(f'`{o}` ({w})' for o, w in detail) + '.')
            else:
                add(f'- **STUCK** — no safe probe separates `{"`, `".join(detail)}`.')
        add('')
        if any(len(part) > 1 for part in result):
            stuck = [p for p in result if len(p) > 1]
            add(f'**Result: INCOMPLETE.** {"; ".join("`" + "` vs `".join(p) + "`" for p in stuck)} '
                'cannot be told apart safely.')
            # Second pass, virgin config only: a `setting` role has no pad before
            # anything is stored, which is exactly the state a first-boot cascade
            # runs in. If that unsticks the group, the probe is worth having as a
            # COMMISSIONING-TIME step - it is not licence to probe a configured
            # device, where those pads are real again.
            vlog = []
            vresult = resolve(sorted(members), probes, roles, unplaced, vlog, virgin=True)
            if not any(len(part) > 1 for part in vresult):
                add('')
                add('**Certifies under VIRGIN CONFIG.** Every refusal above is an unplaced role bound')
                add('by `setting`, which takes its pad from stored configuration. On a device with')
                add('nothing stored yet those roles occupy no pad at all, so the probe below drives')
                add('nothing that exists:')
                add('')
                for kind, board, probe, detail in vlog:
                    if kind == 'safe':
                        add(f'- **SAFE (virgin only)** — probe `{board}` for {probe["what"]}: drives GPIO '
                            f'{", ".join(str(p) for p in probe["driven"])}'
                            + (f', reads {probe["read"][0]}' if probe['read'] else '') + '.')
                add('')
                add('Emit this as a COMMISSIONING-TIME probe: valid on first boot before any pin')
                add('assignment is stored, and invalid the moment one is. A device that has been')
                add('configured must fall back to the group ruling above. `variant`-bound roles are')
                add('NOT excused here - their pad is decided by the hardware, not by configuration.')
        else:
            add('**Result: COMPLETE.** Every board in this group is reachable by safe probes alone.')
        add('')

    decided = [k for k, r in outcome.items() if not any(len(part) > 1 for part in r)]
    undecided = [k for k, r in outcome.items() if any(len(part) > 1 for part in r)]
    settled = [k for k, m in groups.items() if len(m) < 2]
    L[verdict_at:verdict_at + 1] = [
        '## Verdict',
        '',
        f'**{len(settled)} of {len(groups)} groups are settled by the free facts alone** - no probe',
        f'needed. Of the {len(groups) - len(settled)} that are not, '
        f'**{len(undecided)} cannot be resolved safely at all**; {len(decided)} can.',
        '',
        'The blocker is not that the parts are unreadable. It is that reaching them means driving',
        'pins that are contactor, precharge, wake-up or product-output lines on the other boards',
        'still in contention - and chip select idles HIGH for the length of a probe rather than',
        'pulsing, while `NCCONTACTOR` lets an installation invert contactor logic, so neither rail',
        'is dependably the harmless one.',
        '']

    # The UNPLACED verdicts are the ones worth acting on: they are a gap in the
    # declarations rather than a fact about the hardware.
    gaps = {b: [f'{lbl} (`{bind}`)' for feat, lbl, bind in u
                if role_class(feat, lbl) != 'BENIGN']
            for b, u in unplaced.items()}
    gaps = {b: v for b, v in gaps.items() if v}
    if gaps:
        add('## What would have to change first')
        add('')
        add('Some probes were refused not because a pin is dangerous but because the declaration')
        add('does not say WHERE a dangerous role lands. A `setting` or `variant` pin is chosen at')
        add('runtime, so it could be any pad, and a safety check cannot certify around it. These')
        add('are gaps in the declarations, not facts about the boards:')
        add('')
        for b in sorted(gaps):
            add(f'- **`{b}`** — {", ".join("`" + g + "`" for g in sorted(set(gaps[b])))}')
        add('')
        add('Placing these - even as "one of these two pads" - would let the checker reason about')
        add('them instead of refusing outright. It would not make every group decidable; the')
        add('numeric collisions above are real hardware.')
        add('')

    add('## What this means')
    add('')
    incomplete = [k for k, r in outcome.items() if any(len(p) > 1 for p in r)]
    if incomplete or not logs:
        add('Runtime detection cannot replace an explicit choice. Where a group stays incomplete the')
        add('remaining boards are separable only by pins that actuate something, and "drive an')
        add('unidentified board\'s contactor line to find out what it is" is not a trade this project')
        add('makes. Detection narrows the list and annotates the chooser - which is what')
        add('`board_profile_html.cpp` already does, and why `FailsafeHal` exists.')
    else:
        add('A safe cascade exists for every group. It would still be a shortlist feeding the')
        add('explicit chooser rather than a replacement for it, because a probe answers "a part')
        add('responded", not "this is the board".')
    return '\n'.join(L) + '\n'


def main():
    check = '--check' in sys.argv[1:]
    boards, macros, roles, probes, errors = [], {}, {}, {}, []
    unplaced, data_by_board = {}, {}
    for path in sorted((ROOT / 'Software' / 'boards').glob('*.yaml')):
        try:
            data = bg.parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        except bg.DeclError as exc:
            errors.append(f'{path.stem}: {exc}')
            continue
        board = data['board']
        boards.append(board)
        data_by_board[board] = data
        macros[board] = data.get(bg.MACRO_KEY, '?')
        roles[board], unplaced[board] = pin_roles(data)
        probes[board] = probes_for(board, data)
    if errors:
        print('board declaration errors:', file=sys.stderr)
        for err in errors:
            print(f'  {err}', file=sys.stderr)
        sys.exit(1)

    facts = board_facts(boards, data_by_board)
    report = render(sorted(boards), facts, macros, roles, unplaced, probes)
    previous = REPORT.read_text(encoding='utf-8') if REPORT.exists() else ''
    if check:
        if previous != report:
            print(f'{REPORT.relative_to(ROOT)} is out of date; run: python3 tools/board_probe_plan.py')
            sys.exit(1)
        print(f'probe plan: up to date ({len(boards)} boards)')
    else:
        REPORT.write_text(report, encoding='utf-8')
        print(f'{"wrote" if previous != report else "unchanged"} {REPORT.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
