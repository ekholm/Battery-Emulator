#!/usr/bin/env python3
"""Bootstrap Software/boards/<board>.yaml from the existing hw_*.h headers.

Transcribing ~197 pin assignments by hand is exactly where a silent error
hides, and the acceptance criterion for this work is binary-identical
firmware - so the declarations are extracted mechanically and then checked by
regenerating the header fragment and diffing it against what was extracted.

Constant getters become `pins:`. Getters that pick their pin at runtime are
recorded in `conditional_pins:` with the reason (a user setting, or a hardware
variant of the same board) - they stay hand-written in the board header, but
declaring that the ROLE exists lets capability validation see it. Leaving them
out entirely would make a board look like it lacks hardware it has.

Capabilities and CAN slots are derived from which roles the board defines,
because the pin map is the hardware truth. The header's own two claims about
its interfaces - available_interfaces() and name_for_comm_interface() - are
NOT used as the source here; board_verify_transcription.py audits them against
what was derived, which is how their disagreements were found.

Usage: python3 tools/board_extract.py            (writes Software/boards/*.yaml)
       python3 tools/board_extract.py --print devkit
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from board_gen import CAP_SPECS, DRIVER_SPECS, _missing  # noqa: E402

ROOT = Path(__file__).parent.parent
HAL = ROOT / 'Software' / 'src' / 'devboard' / 'hal'
BOARDS = ROOT / 'Software' / 'boards'

# virtual gpio_num_t NAME() { return GPIO_NUM_X; }   - on one line, no branches
CONST_PIN = re.compile(
    r'^\s*virtual\s+gpio_num_t\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+(GPIO_NUM_[A-Z0-9]+);\s*\}\s*$')
CONST_SCALAR = re.compile(
    r'^\s*virtual\s+(uint8_t|uint32_t|int)\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+([^;]+);\s*\}\s*$')
# Any gpio_num_t getter that is NOT the constant one-liner above.
COND_PIN = re.compile(r'^\s*virtual\s+gpio_num_t\s+([A-Z0-9_]+)\(\)\s*\{(?!\s*return\s+GPIO_NUM_[A-Z0-9]+;\s*\})')
NAME = re.compile(r'^\s*const char\* name\(\)\s*\{\s*return\s+"([^"]*)";\s*\}\s*$')
CLASS = re.compile(r'^\s*class\s+(\w+)\s*:\s*public\s+Esp32Hal')

BOARD_OF_FILE = {
    'hw_3LB.h': '3lb', 'hw_becom.h': 'becom', 'hw_devkit.h': 'devkit',
    'hw_lilygo.h': 'lilygo', 'hw_lilygo2can.h': 'lilygo2can',
    'hw_stark.h': 'stark', 'hw_waveshare.h': 'waveshare',
}

# What makes a getter conditional. A user setting can change at runtime; a
# hardware variant is fixed for a given physical board but not known to the
# compiler. They need different treatment in stage 2, so record which is which.
VARIANT_MARKERS = ('is_fd()', 'isStarkVersion1()')

# CAN slot names, in the order the driver's enum exposes them.
SLOT_NAME = {'native': 'can1', 'mcp2515': 'can2', 'mcp2518fd': 'canfd1', 'mcp2518fd_2': 'canfd2'}


def extract(path):
    out = {'header': path.name, 'pins': [], 'scalars': [], 'conditional': [],
           'name': None, 'class': None}
    lines = path.read_text(encoding='utf-8').splitlines()
    for n, line in enumerate(lines):
        m = CLASS.match(line)
        if m:
            out['class'] = m.group(1)
            continue
        m = NAME.match(line)
        if m:
            out['name'] = m.group(1)
            continue
        m = CONST_PIN.match(line)
        if m:
            out['pins'].append((m.group(1), m.group(2)))
            continue
        m = CONST_SCALAR.match(line)
        if m:
            out['scalars'].append((m.group(2), m.group(1), m.group(3).strip()))
            continue
        m = COND_PIN.match(line)
        if m:
            body = '\n'.join(lines[n:n + 8])
            reason = 'variant' if any(k in body for k in VARIANT_MARKERS) else 'setting'
            out['conditional'].append((m.group(1), reason))
    return out


def derive(data):
    """Capabilities and CAN slots the pin map can actually support."""
    roles = {r for r, g in data['pins'] if g != 'GPIO_NUM_NC'}
    roles |= {r for r, _ in data['conditional']}
    scalars = {n: {'type': t, 'value': v} for n, t, v in data['scalars']}
    shim = {'scalars': scalars}
    caps = [c for c, spec in CAP_SPECS.items() if not _missing(spec, roles, shim)]
    slots = [{'slot': SLOT_NAME[d], 'driver': d}
             for d, spec in DRIVER_SPECS.items() if not _missing(spec, roles, shim)]
    return caps, slots


def to_yaml(board, data, extra=''):
    lines = [
        '# Board declaration. GENERATED ONCE from the header of the same name;',
        '# from here on this file is the source and tools/board_gen.py rebuilds',
        '# the generated fragments from it. Pins whose GPIO depends on a runtime',
        '# user setting or a hardware variant stay hand-written in the board',
        '# header; they are listed under conditional_pins so that capability',
        '# validation still knows the role exists.',
        f'board: {board}',
        f'class: {data["class"]}',
        f'header: {data["header"]}',
        f'name: "{data["name"]}"',
        'pins:',
    ]
    for role, gpio in data['pins']:
        lines.append(f'  {role}: {gpio[len("GPIO_NUM_"):]}')
    if data['conditional']:
        lines.append('conditional_pins:')
        for role, reason in data['conditional']:
            lines.append(f'  {role}: {reason}')
    if data['scalars']:
        lines.append('scalars:')
        for name, ctype, value in data['scalars']:
            lines.append(f'  {name}: {{type: {ctype}, value: "{value}"}}')
    caps, slots = derive(data)
    lines.append('capabilities: [' + ', '.join(caps) + ']')
    if slots:
        lines.append('can:')
        for slot in slots:
            lines.append(f'  - {{slot: {slot["slot"]}, driver: {slot["driver"]}}}')
    return '\n'.join(lines) + '\n' + extra


def main():
    if '--print' in sys.argv:
        want = sys.argv[sys.argv.index('--print') + 1]
        for f, b in BOARD_OF_FILE.items():
            if b == want:
                print(to_yaml(b, extract(HAL / f)))
        return
    BOARDS.mkdir(parents=True, exist_ok=True)
    total = cond = 0
    for f, board in sorted(BOARD_OF_FILE.items()):
        data = extract(HAL / f)
        caps, slots = derive(data)
        (BOARDS / f'{board}.yaml').write_text(to_yaml(board, data), encoding='utf-8')
        total += len(data['pins'])
        cond += len(data['conditional'])
        print(f'{board:12} {len(data["pins"]):3} pins {len(data["conditional"]):2} conditional  '
              f'{len(caps):2} capabilities  {len(slots)} CAN slots')
    print(f'total: {total} constant pins, {cond} conditional getters')


if __name__ == '__main__':
    main()
