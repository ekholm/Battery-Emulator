#!/usr/bin/env python3
"""Bootstrap Software/boards/<board>.yaml from the hw_*.h headers.

Transcribing ~200 pin assignments by hand is exactly where a silent error
hides, and the acceptance criterion is binary-identical firmware, so the
declarations are extracted mechanically and then proved by regenerating the
header block and diffing it against the original headers.

Output is the feature-centric schema: a feature owns its pins, shared SPI
buses are declared once and referenced, and a getter that cannot be a constant
is recorded as `setting` or `variant` so the role still exists for validation.

Hand-authored sections (`outputs:` product labels, `gpio_header:`) are carried
over from an existing declaration rather than dropped, so this stays safe to
re-run.

Usage: python3 tools/board_extract.py            (writes Software/boards/*.yaml)
       python3 tools/board_extract.py --print stark
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from board_gen import FEATURES, FEATURE_ORDER, parse_yaml_subset  # noqa: E402

ROOT = Path(__file__).parent.parent
HAL = ROOT / 'Software' / 'src' / 'devboard' / 'hal'
BOARDS = ROOT / 'Software' / 'boards'

CONST_PIN = re.compile(
    r'^\s*virtual\s+gpio_num_t\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+GPIO_NUM_([A-Z0-9]+);\s*\}\s*(//.*)?$')
CONST_SCALAR = re.compile(
    r'^\s*virtual\s+(uint8_t|uint32_t|int)\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+([^;]+);\s*\}\s*$')
COND_PIN = re.compile(r'^\s*virtual\s+gpio_num_t\s+([A-Z0-9_]+)\(\)\s*\{(?!\s*return\s+GPIO_NUM_[A-Z0-9]+;\s*\})')
NAME = re.compile(r'^\s*const char\* name\(\)\s*\{\s*return\s+"([^"]*)";\s*\}\s*$')
CLASS = re.compile(r'^\s*class\s+(\w+)\s*:\s*public\s+Esp32Hal')

BOARD_OF_FILE = {
    'hw_3LB.h': '3lb', 'hw_becom.h': 'becom', 'hw_devkit.h': 'devkit',
    'hw_lilygo.h': 'lilygo', 'hw_lilygo2can.h': 'lilygo2can',
    'hw_stark.h': 'stark', 'hw_waveshare.h': 'waveshare',
}

# A getter is conditional for one of two reasons, and they need different
# treatment in stage 2: a user setting can change at runtime, a hardware
# variant is fixed for a given physical board but unknown to the compiler.
VARIANT_MARKERS = ('is_fd()', 'isStarkVersion1()')


def read_header(path):
    out = {'header': path.name, 'pins': {}, 'scalars': {}, 'conditional': {},
           'comments': {}, 'name': None, 'class': None}
    lines = path.read_text(encoding='utf-8').splitlines()
    for n, line in enumerate(lines):
        m = CLASS.match(line)
        if m:
            out['class'] = m.group(1)
        m = NAME.match(line)
        if m:
            out['name'] = m.group(1)
        m = CONST_PIN.match(line)
        if m:
            out['pins'][m.group(1)] = m.group(2)
            if m.group(3):
                out['comments'][m.group(1)] = m.group(3).strip()
            continue
        m = CONST_SCALAR.match(line)
        if m:
            out['scalars'][m.group(2)] = (m.group(1), m.group(3).strip())
            continue
        m = COND_PIN.match(line)
        if m:
            body = '\n'.join(lines[n:n + 8])
            out['conditional'][m.group(1)] = (
                'variant' if any(k in body for k in VARIANT_MARKERS) else 'setting')
    return out


def value_of(hdr, getter):
    """The declaration value for a getter, or None if the board lacks it."""
    if getter in hdr['pins']:
        return hdr['pins'][getter]
    if getter in hdr['conditional']:
        return hdr['conditional'][getter]
    return None


def build(hdr):
    """Regroup the flat getters into features, buses and instances."""
    buses, bus_names = {}, {}

    def bus_ref(fields):
        """Name the (clk, mosi, miso) triple, reusing a bus already declared
        with the same pins - which is how a shared SPI bus becomes visible."""
        triple = tuple(value_of(hdr, g) for g in fields.values())
        if all(v is None for v in triple):
            return None
        if triple in bus_names:
            return bus_names[triple]
        name = f'SPI{len(buses) + 1}'
        bus_names[triple] = name
        buses[name] = {k: v for k, v in zip(fields, triple) if v is not None}
        return name

    features = {}
    for feature in FEATURE_ORDER:
        spec = FEATURES[feature]
        drivers = spec['by_driver'].items() if 'by_driver' in spec else [(None, spec)]
        entries = []
        for driver, dspec in drivers:
            for index, fields in enumerate(dspec['instances']):
                values = {k: value_of(hdr, g) for k, g in fields.items()}
                if all(v is None for v in values.values()):
                    continue
                entry = {}
                if driver:
                    entry['driver'] = driver
                if 'bus' in dspec:
                    ref = bus_ref(dspec['bus'][index])
                    if ref is None and index > 0:
                        # Second chip with no -2 bus pins shares the first bus.
                        ref = bus_ref(dspec['bus'][0])
                    if ref:
                        entry['bus'] = ref
                entry.update({k: v for k, v in values.items() if v is not None})
                for name, (ctype, getter) in (dspec.get('scalars', [{}] * (index + 1))[index]
                                              if index < len(dspec.get('scalars', [])) else {}).items():
                    if getter in hdr['scalars']:
                        entry[name] = hdr['scalars'][getter][1]
                entries.append(entry)
        if entries:
            features[feature] = entries
    return buses, features


def to_yaml(board, hdr, carried):
    buses, features = build(hdr)
    lines = [
        '# Board declaration - the source for the generated block in the header',
        '# of the same name. Features own their pins; shared buses are declared',
        '# once and referenced. A pin is a GPIO number, NC (the board does not',
        '# have it), or setting/variant when the getter is chosen at runtime and',
        '# stays hand-written below the generated block.',
        f'board: {board}',
        f'class: {hdr["class"]}',
        f'header: {hdr["header"]}',
        f'name: "{hdr["name"]}"',
    ]
    if buses:
        lines.append('buses:')
        for name, bus in buses.items():
            body = ', '.join(f'{k}: {v}' for k, v in bus.items())
            lines.append(f'  {name}: {{{body}}}')
    for feature in FEATURE_ORDER:
        if feature not in features:
            continue
        lines.append(f'{feature}:')
        for entry in features[feature]:
            body = ', '.join(f'{k}: {v}' for k, v in entry.items())
            lines.append(f'  - {{{body}}}')
    if hdr['comments']:
        lines.append('comments:')
        for getter, text in hdr['comments'].items():
            lines.append(f'  {getter}: "{text}"')
    return '\n'.join(lines) + '\n' + carried


def carried_sections(path):
    """Keep hand-authored sections that cannot be extracted from a header."""
    if not path.exists():
        return ''
    text = path.read_text(encoding='utf-8')
    kept = []
    for section in ('outputs', 'gpio_header'):
        m = re.search(rf'^((?:#[^\n]*\n)*){section}:.*?(?=^\S|\Z)', text, re.S | re.M)
        if m:
            kept.append(m.group(0).rstrip('\n'))
    return ('\n'.join(kept) + '\n') if kept else ''


def main():
    if '--print' in sys.argv:
        want = sys.argv[sys.argv.index('--print') + 1]
        for f, b in BOARD_OF_FILE.items():
            if b == want:
                print(to_yaml(b, read_header(HAL / f), carried_sections(BOARDS / f'{b}.yaml')))
        return
    BOARDS.mkdir(parents=True, exist_ok=True)
    for f, board in sorted(BOARD_OF_FILE.items()):
        hdr = read_header(HAL / f)
        target = BOARDS / f'{board}.yaml'
        target.write_text(to_yaml(board, hdr, carried_sections(target)), encoding='utf-8')
        buses, features = build(hdr)
        print(f'{board:12} {len(hdr["pins"]):3} const {len(hdr["conditional"]):2} late-bound  '
              f'{len(buses)} bus  {len(features)} features')


if __name__ == '__main__':
    main()
