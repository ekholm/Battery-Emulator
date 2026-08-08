#!/usr/bin/env python3
"""Generate each board's constant definitions from Software/boards/*.yaml.

The generated text lives IN the board header, between marker comments, rather
than in a separate file the header includes: the header stays the one file a
reader opens, and there is no tier of generated files that nothing consumes.
Everything outside the markers - getters whose pin is chosen at runtime, the
interface lists, the helpers - is hand-written and left untouched.

Declarations are FEATURE-CENTRIC: a feature owns its pins, so validation is
local to the block it sits in and role-name strings stop carrying structure.
Shared buses are declared once under `buses:` and referenced by the features
on them, which makes pin sharing visible instead of implied by two getters
happening to return the same GPIO.

A pin value is a number, NC (the board deliberately lacks it), or `setting` /
`variant` - the two reasons a getter cannot be a constant. Those emit nothing
and stay hand-written, but declaring the field keeps the feature's validation
honest: the role exists, only its GPIO is late-bound.

No PyYAML: this repo has no Python dependencies. The parser accepts the small
subset these files use and REFUSES anything else rather than guessing, because
a declaration that silently parses wrong produces a wrong pin map.

Usage: python3 tools/board_gen.py            rewrite the generated blocks
       python3 tools/board_gen.py --check    fail if any block is out of date
       python3 tools/board_gen.py --boards DIR --headers DIR   (for tests)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
BOARDS = ROOT / 'Software' / 'boards'
HEADERS = ROOT / 'Software' / 'src' / 'devboard' / 'hal'

BEGIN = '  // ---- BEGIN GENERATED from Software/boards/{board}.yaml ----'
END = '  // ---- END GENERATED ----'
NOTE = ('  // Rebuild with tools/board_gen.py; CI fails if this block and the\n'
        '  // declaration disagree. Getters below the block are hand-written.')

# Pin values that are not a GPIO number.
NOT_A_PIN = ('NC', 'setting', 'variant')
LATE_BOUND = ('setting', 'variant')  # hand-written getter, role still exists

# Every feature: which fields map to which getter, which are required, and any
# scalar getters the feature carries. Features whose key holds several
# instances list one field-map per instance, because the second instance of a
# thing has different getter names (MCP2517_CS -> MCP2517_CS2) even though it
# is the same driver with the same requirements.
FEATURES = {
    'rs485': {
        'instances': [{'tx': 'RS485_TX_PIN', 'rx': 'RS485_RX_PIN', 'en': 'RS485_EN_PIN',
                       'se': 'RS485_SE_PIN', 'de': 'RS485_DE_PIN', 'power_5v_en': 'PIN_5V_EN'}],
        'requires': ['tx', 'rx'],
    },
    'can': {'by_driver': {
        'native': {
            'instances': [{'tx': 'CAN_TX_PIN', 'rx': 'CAN_RX_PIN', 'se': 'CAN_SE_PIN'}],
            'requires': ['tx', 'rx'],
        },
        # comm_can.cpp CAN_ADDON_MCP2515: bus pins and CS/INT all go to one
        # alloc_pins() call, so all five must resolve. RST is driven only when
        # it is not NC.
        'mcp2515': {
            'instances': [{'cs': 'MCP2515_CS', 'int': 'MCP2515_INT', 'rst': 'MCP2515_RST'}],
            'bus': [{'clk': 'MCP2515_SCK', 'mosi': 'MCP2515_MOSI', 'miso': 'MCP2515_MISO'}],
            'requires': ['cs', 'int'],
            'scalars': [{'freq': ('uint32_t', 'MCP2515_FREQ'), 'spi_bus': ('uint8_t', 'MCP2515_BUS')}],
        },
        # comm_can.cpp CANFD_NATIVE / CANFD_ADDON_MCP2518 are the same code
        # path on the same chip - "native" means soldered to the board, not an
        # ESP32 peripheral - so they are one driver here. The interrupt is INT,
        # or INT0+INT1 when the part is wired that way. A second instance needs
        # its own bus pins only when it sits on a different bus (:236).
        'mcp2518fd': {
            'instances': [{'cs': 'MCP2517_CS', 'int': 'MCP2517_INT',
                           'int0': 'MCP2517_INT0', 'int1': 'MCP2517_INT1'},
                          {'cs': 'MCP2517_CS2', 'int': 'MCP2517_INT2'}],
            'bus': [{'clk': 'MCP2517_SCK', 'mosi': 'MCP2517_SDI', 'miso': 'MCP2517_SDO'},
                    {'clk': 'MCP2517_SCK2', 'mosi': 'MCP2517_SDI2', 'miso': 'MCP2517_SDO2'}],
            'requires': ['cs'],
            'one_of': [['int'], ['int0', 'int1']],
            'scalars': [{'freq': ('uint32_t', 'MCP2517_FREQ'), 'spi_bus': ('uint8_t', 'MCP2517_BUS'),
                         'clkodiv': ('int', 'MCP2517_CLKODIV')},
                        {'freq': ('uint32_t', 'MCP2517_FREQ2'), 'spi_bus': ('uint8_t', 'MCP2517_BUS2')}],
        },
    }},
    'chademo': {
        'instances': [{'pin2': 'CHADEMO_PIN_2', 'pin10': 'CHADEMO_PIN_10', 'pin7': 'CHADEMO_PIN_7',
                       'pin4': 'CHADEMO_PIN_4', 'lock': 'CHADEMO_LOCK', 'ct': 'CHADEMO_CT_PIN'}],
        'requires': ['pin2', 'pin10', 'pin7', 'pin4', 'lock'],
    },
    'contactors': {
        'instances': [{'positive': 'POSITIVE_CONTACTOR_PIN', 'negative': 'NEGATIVE_CONTACTOR_PIN',
                       'precharge': 'PRECHARGE_PIN', 'bms_power': 'BMS_POWER',
                       'second': 'SECOND_BATTERY_CONTACTORS_PIN',
                       'third': 'TRIPLE_BATTERY_CONTACTORS_PIN'}],
        'requires': ['positive', 'negative', 'precharge'],
    },
    'precharge_auto': {
        'instances': [{'hia4v1': 'HIA4V1_PIN', 'inverter_disconnect': 'INVERTER_DISCONNECT_CONTACTOR_PIN'}],
        'requires': ['hia4v1', 'inverter_disconnect'],
    },
    'sma': {
        'instances': [{'enable': 'INVERTER_CONTACTOR_ENABLE_PIN', 'led': 'INVERTER_CONTACTOR_ENABLE_LED_PIN'}],
        'requires': ['enable'],
    },
    # sdcard.cpp init_sdcard() drives SD_MMC in 1-bit mode and allocates
    # miso/mosi/clk only. SD_CS_PIN is declared by two boards and read by
    # nothing in the tree, so it is optional here rather than required.
    'sd_mmc': {
        'instances': [{'miso': 'SD_MISO_PIN', 'mosi': 'SD_MOSI_PIN',
                       'clk': 'SD_SCLK_PIN', 'cs': 'SD_CS_PIN'}],
        'requires': ['miso', 'mosi', 'clk'],
    },
    # The SPI-attached card, a separate feature rather than a field on sd_mmc.
    # The pins carry a different meaning - this is a chip-select SPI slave, not
    # SD_MMC's 1-bit bus - so `cs` is required here and optional there, and the
    # board must also say which SPI peripheral it sits on. The whole group is
    # compiled out unless SDCARD is defined, which is what `guard` emits.
    'sd_spi': {
        'instances': [{'mosi': 'SD_MOSI_PIN', 'miso': 'SD_MISO_PIN',
                       'clk': 'SD_SCLK_PIN', 'cs': 'SD_CS_PIN'}],
        'requires': ['mosi', 'miso', 'clk', 'cs'],
        # `override` rather than `virtual`: the base class already declares
        # SD_SPI_BUS(), so this one overrides it and the emitted line has to
        # match that shape or the transcription check rejects it.
        'scalars': [{'spi_bus': ('uint8_t', 'SD_SPI_BUS', 'override')}],
        'guard': 'SDCARD',
    },
    'display_i2c': {
        'instances': [{'sda': 'DISPLAY_SDA_PIN', 'scl': 'DISPLAY_SCL_PIN'}],
        'requires': ['sda', 'scl'],
    },
    'rgb_led': {
        'instances': [{'pin': 'LED_PIN'}],
        'requires': ['pin'],
        'scalars': [{'count': ('uint8_t', 'LED_COUNT'),
                     'max_brightness': ('uint8_t', 'LED_MAX_BRIGHTNESS')}],
    },
    'equipment_stop': {'instances': [{'pin': 'EQUIPMENT_STOP_PIN'}], 'requires': ['pin']},
    'battery_wakeup': {
        'instances': [{'wup1': 'WUP_PIN1', 'wup2': 'WUP_PIN2'}],
        'requires': ['wup1', 'wup2'],
    },
    'ap_button': {'instances': [{'pin': 'AP_BUTTON_PIN'}], 'requires': ['pin']},
}

# Emission order, chosen to sit close to how the headers already read.
FEATURE_ORDER = ['rs485', 'can', 'chademo', 'contactors', 'precharge_auto', 'sma',
                 'sd_mmc', 'sd_spi', 'rgb_led', 'equipment_stop', 'battery_wakeup', 'ap_button',
                 'display_i2c']

SECTION_COMMENT = {
    'rs485': 'RS485', 'can': 'CAN interfaces', 'chademo': 'CHAdeMO support pins',
    'contactors': 'Contactor handling', 'precharge_auto': 'Automatic precharging',
    'sma': 'SMA CAN contactor pins', 'sd_mmc': 'SD card', 'sd_spi': 'microSD (SPI)',
    'rgb_led': 'LED',
    'equipment_stop': 'Equipment stop pin', 'battery_wakeup': 'Battery wake up pins',
    'ap_button': 'Wi-Fi AP button', 'display_i2c': 'i2c display',
}


class DeclError(Exception):
    pass


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

def _split_top(text, sep=','):
    """Split on sep, ignoring separators inside quotes or brackets."""
    parts, cur, quoted, depth = [], '', False, 0
    for ch in text:
        if ch == '"':
            quoted = not quoted
        elif not quoted and ch in '[{':
            depth += 1
        elif not quoted and ch in ']}':
            depth -= 1
        if ch == sep and not quoted and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)
    return [p.strip() for p in parts if p.strip()]


def _value(text, where):
    text = text.strip()
    if text.startswith('{'):
        return parse_inline_map(text, where)
    if text.startswith('['):
        if not text.endswith(']'):
            raise DeclError(f'{where}: unterminated list "{text}"')
        return _split_top(text[1:-1])
    return text.strip('"')


def parse_inline_map(text, where):
    if not (text.startswith('{') and text.endswith('}')):
        raise DeclError(f'{where}: expected an inline map, got "{text}"')
    out = {}
    for item in _split_top(text[1:-1]):
        key, sep, value = item.partition(':')
        if not sep:
            raise DeclError(f'{where}: "{item}" is not key: value')
        out[key.strip()] = _value(value, where)
    return out


def parse_yaml_subset(text, where):
    """Top-level scalars and inline values, one level of nested "key: value"
    maps, and one level of nested "- {inline map}" lists. Nothing else."""
    data, section, kind = {}, None, None
    for n, raw in enumerate(text.splitlines(), 1):
        line = '' if raw.lstrip().startswith('#') else raw.split(' #', 1)[0].rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        body = line.strip()
        if indent == 0:
            key, sep, value = body.partition(':')
            if not sep:
                raise DeclError(f'{where}:{n}: cannot parse "{body}"')
            key, value = key.strip(), value.strip()
            if value == '':
                section, kind, data[key] = key, None, None
            else:
                section, kind = None, None
                data[key] = _value(value, f'{where}:{n}')
        elif indent == 2 and section:
            if body.startswith('- '):
                if kind is None:
                    kind, data[section] = 'list', []
                if kind != 'list':
                    raise DeclError(f'{where}:{n}: list item inside a map section "{section}"')
                data[section].append(parse_inline_map(body[2:].strip(), f'{where}:{n}'))
            else:
                if kind is None:
                    kind, data[section] = 'map', {}
                if kind != 'map':
                    raise DeclError(f'{where}:{n}: map entry inside a list section "{section}"')
                key, sep, value = body.partition(':')
                if not sep:
                    raise DeclError(f'{where}:{n}: cannot parse "{body}"')
                data[section][key.strip()] = _value(value, f'{where}:{n}')
        else:
            raise DeclError(f'{where}:{n}: unexpected indent {indent} in "{body}"')
    for key, value in data.items():
        if value is None:
            data[key] = {}
    return data


# --------------------------------------------------------------------------
# Model
# --------------------------------------------------------------------------

def instances_of(data, feature):
    """Declared instances of a feature, as (spec, index, instance) triples."""
    spec = FEATURES[feature]
    out = []
    if 'by_driver' in spec:
        per_driver = {}
        for inst in data.get(feature, []):
            driver = inst.get('driver')
            if driver is None:
                raise DeclError(f'{feature} entry {inst} has no driver')
            if driver not in spec['by_driver']:
                raise DeclError(f'{feature} entry has unknown driver "{driver}"')
            index = per_driver.get(driver, 0)
            per_driver[driver] = index + 1
            out.append((spec['by_driver'][driver], index, inst, driver))
    else:
        for index, inst in enumerate(data.get(feature, [])):
            out.append((spec, index, inst, feature))
    return out


def bus_of(data, inst, where):
    name = inst.get('bus')
    if name is None:
        return None, None
    buses = data.get('buses', {})
    if name not in buses:
        raise DeclError(f'{where}: references bus "{name}", which is not declared')
    return name, buses[name]


def resolved(value):
    """Does this field give a usable GPIO? NC and late-bound values do not."""
    return value is not None and str(value) != 'NC'


def present(value):
    """Is the role declared at all (a number, or late-bound)?"""
    return value is not None and str(value) not in ('NC',)


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------

def validate(board, data):
    errors = []
    for feature in FEATURES:
        try:
            declared = instances_of(data, feature)
        except DeclError as exc:
            errors.append(f'{board}: {exc}')
            continue
        for spec, index, inst, driver in declared:
            where = f'{board}: {feature}' + (f' ({driver} #{index + 1})' if 'by_driver' in FEATURES[feature] else '')
            try:
                bus_name, bus = bus_of(data, inst, where)
            except DeclError as exc:
                errors.append(str(exc))
                continue
            if index >= len(spec['instances']):
                errors.append(f'{where}: more instances declared than the driver supports '
                              f'({len(spec["instances"])})')
                continue

            def field(name):
                if name in inst:
                    return inst[name]
                if bus and name in bus:
                    return bus[name]
                return None

            for name in spec.get('requires', []):
                if not present(field(name)):
                    errors.append(f'{where} needs "{name}", which the board does not define')
            for group in spec.get('one_of', []):
                if any(all(present(field(n)) for n in g) for g in spec['one_of']):
                    break
            else:
                if 'one_of' in spec:
                    shapes = ' or '.join('+'.join(g) for g in spec['one_of'])
                    errors.append(f'{where} needs {shapes}, none of which the board defines')
            if 'bus' in spec:
                if bus_name is None:
                    errors.append(f'{where} is on a bus but declares no "bus:"')
                else:
                    for name in ('clk', 'mosi', 'miso'):
                        if not present(bus.get(name)):
                            errors.append(f'{where} uses bus "{bus_name}", which does not define "{name}"')
            unknown = set(inst) - set(spec['instances'][index]) - {'driver', 'bus'} - set(
                spec.get('scalars', [{}] * (index + 1))[index] if index < len(spec.get('scalars', [])) else {})
            if unknown:
                errors.append(f'{where} has unknown field(s) {sorted(unknown)}')

    # Both card features emit SD_MISO_PIN/SD_MOSI_PIN/SD_SCLK_PIN, so a board
    # declaring both would define them twice. Caught here rather than left to
    # the compiler, since the declaration is where the mistake is made.
    if data.get('sd_mmc') and data.get('sd_spi'):
        errors.append(f'{board}: declares both sd_mmc and sd_spi; a board has one kind of '
                      f'card interface, and the two emit the same getters')

    seen_bus_pins = {}
    for name, bus in data.get('buses', {}).items():
        for field_name, value in bus.items():
            if str(value).isdigit():
                seen_bus_pins.setdefault(int(value), []).append(f'{name}.{field_name}')

    labelled = {}
    for out in data.get('outputs', []):
        if 'label' not in out or 'gpio' not in out:
            errors.append(f'{board}: output {out} needs both label and gpio')
            continue
        if out['gpio'] in labelled:
            errors.append(f'{board}: outputs "{labelled[out["gpio"]]}" and "{out["label"]}" '
                          f'both claim GPIO {out["gpio"]}')
        labelled[out['gpio']] = out['label']
    return errors


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------

def _pin_line(getter, value, comments):
    if value is None or str(value) in LATE_BOUND:
        # Not declared at all, or chosen at runtime: hand-written, not here.
        return None
    num = 'NC' if str(value) == 'NC' else str(value)
    line = f'  virtual gpio_num_t {getter}() {{ return GPIO_NUM_{num}; }}'
    if getter in comments:
        line += '  ' + comments[getter]
    return line


def _scalar_line(sspec, value):
    """A scalar getter. The optional third element picks `override` over
    `virtual`, for getters the base class already declares."""
    ctype, getter = sspec[0], sspec[1]
    style = sspec[2] if len(sspec) > 2 else 'virtual'
    if style == 'override':
        return f'  {ctype} {getter}() override {{ return {value}; }}'
    return f'  virtual {ctype} {getter}() {{ return {value}; }}'


def block(board, data):
    comments = data.get('comments', {})
    lines = [BEGIN.format(board=board), NOTE, '',
             f'  const char* name() {{ return "{data["name"]}"; }}']
    for feature in FEATURE_ORDER:
        declared = instances_of(data, feature)
        if not declared:
            continue
        emitted = []
        for spec, index, inst, driver in declared:
            _, bus = bus_of(data, inst, feature)
            fields = spec['instances'][index]
            if 'bus' in spec and bus is not None and index < len(spec['bus']):
                # A second chip on the same bus as the first reuses it, so the
                # -2 getters do not exist; only a distinct bus emits its own.
                first_bus = None
                if index > 0:
                    for s2, i2, inst2, d2 in declared:
                        if d2 == driver and i2 == 0:
                            first_bus = inst2.get('bus')
                if index == 0 or inst.get('bus') != first_bus:
                    for name, getter in spec['bus'][index].items():
                        line = _pin_line(getter, bus.get(name), comments)
                        if line:
                            emitted.append(line)
            for name, getter in fields.items():
                line = _pin_line(getter, inst.get(name), comments)
                if line:
                    emitted.append(line)
            for name, sspec in (spec.get('scalars', [{}] * (index + 1))[index]
                                if index < len(spec.get('scalars', [])) else {}).items():
                if name in inst:
                    emitted.append(_scalar_line(sspec, inst[name]))
        if emitted:
            guard = FEATURES[feature].get('guard')
            lines.append('')
            lines.append(f'  // {SECTION_COMMENT[feature]}')
            if guard:
                lines.append(f'#ifdef {guard}')
            lines += emitted
            if guard:
                lines.append(f'#endif  // {guard}')
    lines.append(END)
    return '\n'.join(lines) + '\n'


BLOCK_RE = re.compile(r'^  // ---- BEGIN GENERATED.*?^  // ---- END GENERATED ----\n',
                      re.S | re.M)


def splice(header_text, new_block):
    """Replace the generated block in a header, or report that it has none."""
    if not BLOCK_RE.search(header_text):
        raise DeclError('header has no generated block to replace')
    return BLOCK_RE.sub(lambda _: new_block, header_text, count=1)


def main():
    argv = sys.argv[1:]
    check = '--check' in argv
    boards = Path(argv[argv.index('--boards') + 1]) if '--boards' in argv else BOARDS
    headers = Path(argv[argv.index('--headers') + 1]) if '--headers' in argv else HEADERS

    updates, errors = [], []
    for path in sorted(boards.glob('*.yaml')):
        try:
            data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
            board = data['board']
            board_errors = validate(board, data)
            if board_errors:
                errors += board_errors
                continue
            target = headers / data['header']
            if not target.exists():
                errors.append(f'{board}: header {data["header"]} not found')
                continue
            text = target.read_text(encoding='utf-8')
            updates.append((target, text, splice(text, block(board, data))))
        except DeclError as exc:
            errors.append(f'{path.stem}: {exc}' if not str(exc).startswith(path.stem) else str(exc))

    if errors:
        print('board declaration errors:', file=sys.stderr)
        for err in errors:
            print(f'  {err}', file=sys.stderr)
        sys.exit(1)

    stale = [t for t, old, new in updates if old != new]
    if check:
        if stale:
            print('generated blocks are out of date:')
            print('\n'.join(f'  {s}' for s in stale))
            print('run: python3 tools/board_gen.py')
            sys.exit(1)
        print(f'board blocks: up to date ({len(updates)} headers)')
    else:
        for target, _, new in updates:
            target.write_text(new, encoding='utf-8')
        print(f'updated {len(stale)} of {len(updates)} generated blocks')


if __name__ == '__main__':
    main()
