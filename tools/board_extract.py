#!/usr/bin/env python3
"""Bootstrap Software/boards/<board>.yaml from the existing hw_*.h headers.

Transcribing ~197 pin assignments by hand is exactly where a silent error
hides, and the acceptance criterion for this work is binary-identical
firmware - so the declarations are extracted mechanically and then checked by
regenerating the header fragment and diffing it against what was extracted.

Only CONSTANT getters are extracted. Four of the seven boards also have
getters that pick a pin from a runtime user setting (BMS_POWER on gpioopt2,
LED_PIN on gpioopt6, the SD and display pins, ...). Those cannot be expressed
as a role->GPIO map without encoding the settings logic, which stage 1
explicitly defers; they stay hand-written in the board header.

Usage: python3 tools/board_extract.py            (writes Software/boards/*.yaml)
       python3 tools/board_extract.py --print devkit
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
HAL = ROOT / 'Software' / 'src' / 'devboard' / 'hal'
BOARDS = ROOT / 'Software' / 'boards'

# virtual gpio_num_t NAME() { return GPIO_NUM_X; }   - on one line, no branches
CONST_PIN = re.compile(
    r'^\s*virtual\s+gpio_num_t\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+(GPIO_NUM_[A-Z0-9]+);\s*\}\s*$')
CONST_SCALAR = re.compile(
    r'^\s*virtual\s+(uint8_t|uint32_t|int)\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+([^;]+);\s*\}\s*$')
NAME = re.compile(r'^\s*const char\* name\(\)\s*\{\s*return\s+"([^"]*)";\s*\}\s*$')
CLASS = re.compile(r'^\s*class\s+(\w+)\s*:\s*public\s+Esp32Hal')

BOARD_OF_FILE = {
    'hw_3LB.h': '3lb', 'hw_becom.h': 'becom', 'hw_devkit.h': 'devkit',
    'hw_lilygo.h': 'lilygo', 'hw_lilygo2can.h': 'lilygo2can',
    'hw_stark.h': 'stark', 'hw_waveshare.h': 'waveshare',
}


def extract(path):
    out = {'header': path.name, 'pins': [], 'scalars': [], 'name': None, 'class': None}
    for line in path.read_text(encoding='utf-8').splitlines():
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
    return out


def to_yaml(board, data):
    lines = [
        '# Board declaration. GENERATED ONCE from the header of the same name;',
        '# from here on this file is the source and tools/board_gen.py rebuilds',
        '# the generated fragment from it. Pins that depend on a runtime user',
        '# setting are NOT here - they stay hand-written in the board header.',
        f'board: {board}',
        f'class: {data["class"]}',
        f'header: {data["header"]}',
        f'name: "{data["name"]}"',
        'pins:',
    ]
    for role, gpio in data['pins']:
        lines.append(f'  {role}: {gpio[len("GPIO_NUM_"):]}')
    if data['scalars']:
        lines.append('scalars:')
        for name, ctype, value in data['scalars']:
            lines.append(f'  {name}: {{type: {ctype}, value: "{value}"}}')
    return '\n'.join(lines) + '\n'


def main():
    if '--print' in sys.argv:
        want = sys.argv[sys.argv.index('--print') + 1]
        for f, b in BOARD_OF_FILE.items():
            if b == want:
                print(to_yaml(b, extract(HAL / f)))
        return
    BOARDS.mkdir(parents=True, exist_ok=True)
    total = 0
    for f, board in sorted(BOARD_OF_FILE.items()):
        data = extract(HAL / f)
        (BOARDS / f'{board}.yaml').write_text(to_yaml(board, data), encoding='utf-8')
        total += len(data['pins'])
        print(f'{board:12} {len(data["pins"]):3} pins  {len(data["scalars"])} scalars  "{data["name"]}"')
    print(f'total constant pins extracted: {total}')


if __name__ == '__main__':
    main()
