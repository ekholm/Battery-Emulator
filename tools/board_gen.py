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
ADDONS = ROOT / 'Software' / 'addons'
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
        # ESP32 peripheral - so they are one driver here. The part is driven
        # from its single nINT on every board this firmware supports, so INT is
        # the only interrupt a declaration may name. A second instance needs
        # its own bus pins only when it sits on a different bus (:236).
        'mcp2518fd': {
            'instances': [{'cs': 'MCP2517_CS', 'int': 'MCP2517_INT'},
                          {'cs': 'MCP2517_CS2', 'int': 'MCP2517_INT2'},
                          {'cs': 'MCP2517_CS3', 'int': 'MCP2517_INT3'},
                          {'cs': 'MCP2517_CS4', 'int': 'MCP2517_INT4'}],
            # Instances 3 and 4 - an isolated dual-FD add-on board sharing the
            # host's bus - have no bus or scalar getters of their own: they sit
            # on the first instance's SPI bus and crystal, which is what "no
            # entry below" means. A board that puts them on a bus of their own
            # needs those getters added first.
            'bus': [{'clk': 'MCP2517_SCK', 'mosi': 'MCP2517_SDI', 'miso': 'MCP2517_SDO'},
                    {'clk': 'MCP2517_SCK2', 'mosi': 'MCP2517_SDI2', 'miso': 'MCP2517_SDO2'}],
            'requires': ['cs', 'int'],
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
        'provides': ['SdCard'],
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
        'provides': ['SdCard'],
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
    # Wired Ethernet. Drivers are PHY/MAC chips, like can's are controllers: an
    # RMII PHY's data pins are fixed ESP32 silicon functions, so a board
    # declares only what actually varies - the management pair, the PHY power
    # switch, the strap address and where the 50 MHz reference clock comes
    # from. clk_mode carries arduino-esp32's eth_clock_mode_t value verbatim
    # (0 = GPIO0 in, 1 = GPIO0 out, 2 = GPIO16 out, 3 = GPIO17 out). Nothing
    # in the tree drives Ethernet yet; the declaration exists so the one board
    # that has it (Edge101, IP101GRI) is expressed completely - the SD_CS_PIN
    # precedent. An SPI-attached MAC (W5500 and kin) gets its own driver with
    # cs/int/rst roles when a board needs one.
    'ethernet': {'by_driver': {
        'ip101': {
            'instances': [{'mdc': 'ETH_MDC_PIN', 'mdio': 'ETH_MDIO_PIN', 'power': 'ETH_POWER_PIN'}],
            'requires': ['mdc', 'mdio'],
            'scalars': [{'phy_addr': ('uint8_t', 'ETH_PHY_ADDR'), 'clk_mode': ('uint8_t', 'ETH_CLK_MODE')}],
        },
    }},
}

# Emission order, chosen to sit close to how the headers already read.
FEATURE_ORDER = ['rs485', 'can', 'chademo', 'contactors', 'precharge_auto', 'sma',
                 'sd_mmc', 'sd_spi', 'rgb_led', 'equipment_stop', 'battery_wakeup', 'ap_button',
                 'display_i2c', 'ethernet']

SECTION_COMMENT = {
    'rs485': 'RS485', 'can': 'CAN interfaces', 'chademo': 'CHAdeMO support pins',
    'contactors': 'Contactor handling', 'precharge_auto': 'Automatic precharging',
    'sma': 'SMA CAN contactor pins', 'sd_mmc': 'SD card', 'sd_spi': 'microSD (SPI)',
    'rgb_led': 'LED',
    'equipment_stop': 'Equipment stop pin', 'battery_wakeup': 'Battery wake up pins',
    'ap_button': 'Wi-Fi AP button', 'display_i2c': 'i2c display',
    'ethernet': 'Ethernet (RMII PHY)',
}


# --------------------------------------------------------------------------
# Capabilities
# --------------------------------------------------------------------------
# A board's CAPABILITY SET is the union of the features it declares - the
# mechanical tie between this schema and the registries that gate rows on
# hardware (the settings table's `requires` column is the first consumer).
# When upstream adds hardware, the feature lands in a YAML and the enum grows;
# nothing keeps a hand-written list in step.
#
# A feature contributes its own cap, plus any it `provides`. The provided caps
# are what lets a consumer ask a question the boards answer two ways: SD-card
# logging needs a card, and a board has one over SD_MMC or over SPI. Asking for
# `SdCard` keeps the requirement a single value, so requirement rows stay a
# conjunction of single ids with no OR anywhere in them.
CAPABILITIES = ROOT / 'Software' / 'src' / 'devboard' / 'hal' / 'capabilities.h'

# Board declarations name their build macro rather than having it derived from
# the board name: it is the macro `hal.cpp` switches on, and the two agreeing
# is checked below rather than assumed from a spelling convention.
MACRO_KEY = 'macro'

# The part a board is built around, and how much program flash it has. Declared
# rather than derived: platformio.ini knows both, but only for boards an env
# actually builds, and a declaration that describes hardware should not go
# missing because nobody wired up a build for it. Where an env DOES exist the
# two are cross-checked below, so the pair cannot drift apart.
VALID_CHIPS = ('esp32', 'esp32s3')

# GPIO numbers only one part has. The ESP32-S3 has no GPIO 22-25 at all and the
# classic ESP32 stops at 39, so a declaration using one of these could not be
# describing the other part - which makes a wrong `chip:` catchable rather than
# a thing to be careful about.
CHIP_ONLY_PINS = {'esp32': (22, 23, 25), 'esp32s3': tuple(range(40, 49))}

# Pads the chip simply does not have. 24 exists on NEITHER part, and it sat
# in CHIP_ONLY_PINS['esp32'], so an esp32 board declaring pad 24 validated
# and the runtime tables offered 20/24/28-31 as free pads. GPIO 20
# exists only on the ESP32-PICO-V3 package, which no declared board uses - if
# one ever does, it carves an exception here rather than deleting the entry.
ABSENT_PINS = {
    'esp32': (20, 24, 28, 29, 30, 31),
    'esp32s3': (22, 23, 24, 25),
}

# GPIOs that can only be read. A signal an add-on RECEIVES has to be driven by
# the MCU, so binding one of these to it cannot work - and the failure is
# silent, because nothing complains and the pin simply never asserts.
INPUT_ONLY = {
    'esp32': (34, 35, 36, 37, 38, 39),
    'esp32s3': (46,),
}


# --- Pad properties, per chip ----------------------------------------------
# What the wizard's validator needs to know about a PAD, as opposed to a role.
# Datasheet facts, kept here beside INPUT_ONLY/CHIP_ONLY_PINS so there is one
# place to check them, and emitted into the runtime tables (spec 1) so the
# on-device validator and the static checkers cannot hold different beliefs.

# Pads the ROM samples at reset. Legal to use, but a pull the wrong way stops
# the board booting, so the wizard warns and asks for explicit confirmation.
STRAPPING_PINS = {
    # 0 and 2 select boot mode, 5 SDIO timing, 12 (MTDI) flash voltage,
    # 15 (MTDO) debug output. 4 is not sampled at reset.
    'esp32': (0, 2, 5, 12, 15),
    'esp32s3': (0, 3, 45, 46),
}

# Pads wired to the flash (and, on modules that have it, PSRAM) die inside the
# package. Not "avoid": using one stops the chip fetching instructions.
RESERVED_PINS = {
    'esp32': (6, 7, 8, 9, 10, 11),
    'esp32s3': (26, 27, 28, 29, 30, 31, 32),
}

# Pads in the RTC domain, which is what lets a level survive reset. A role that
# must hold across a reset (BMS power) can only live here - the constraint that
# reset_hold_pins() encodes by hand today.
RTC_CAPABLE = {
    'esp32': (0, 2, 4, 12, 13, 14, 15, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39),
    'esp32s3': tuple(range(0, 22)),
}

# Pads an ADC can read. Only one role needs it today (chademo.ct, which calls
# analogReadMilliVolts), but a role that needs an ADC placed on a pad without
# one fails silently, which is the class of bug these tables exist to catch.
ADC_CAPABLE = {
    'esp32': (0, 2, 4, 12, 13, 14, 15, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39),
    'esp32s3': tuple(range(1, 21)),
}

# The half of ADC_CAPABLE a role can actually count on: ADC2 is shared with the
# WiFi radio and reads fail while it is active, and this firmware always runs
# WiFi. A role that needs an ADC needs ADC1 - the two esp32
# CHAdeMO boards that pre-date this constraint sit on a shrink-only exception
# list in test_board_validation.py.
ADC1_CAPABLE = {
    'esp32': (32, 33, 34, 35, 36, 37, 38, 39),
    'esp32s3': tuple(range(1, 11)),
}

# --- Role constraints, per (feature, role) ---------------------------------
# Per (feature, role), NOT per feature: chademo.lock is a solenoid inside an
# otherwise externally-driven feature, and a per-feature column would demote it
# (spec 4, desk review point 6).
#
# `dir` is the safety-relevant column - it is what makes "host-driven role on an
# INPUT_ONLY pad" refusable - so every entry below is taken from how the
# firmware actually configures the pin, not from the role's name:
#   READS  equipment_stop.pin      comm_equipmentstopbutton.cpp pinMode(pin, INPUT)
#          ap_button.pin           debounce_button.cpp          pinMode(pin, INPUT)
#          chademo.ct              CHADEMO-CT.cpp               pinMode(ct_pin, INPUT) + analogReadMilliVolts
#          chademo.pin4/pin7       CHADEMO-BATTERY.cpp:907-908  pinMode(..., INPUT)
#          can.int                 mcp2515_lite.cpp:144         pinMode(_int_pin, INPUT_PULLUP)
#   DRIVES chademo.pin2/pin10/lock CHADEMO-BATTERY.cpp          pinMode(..., OUTPUT)
#          contactors.*            contactor control            pinMode(pos/neg/prec/..., OUTPUT)
#          precharge_auto.hia4v1   precharge_control.cpp        pinMode(hia4v1_pin, OUTPUT)
#          battery_wakeup.wup*     CMP-SMART-CAR-BATTERY.cpp    pinMode(WUP_PIN1(), OUTPUT)
#          rs485.de/se/power_5v_en rs485 setup                  pinMode(..., OUTPUT)
#          can.rst/se              mcp2515 setup                pinMode(rst_pin, OUTPUT)
# Bus roles follow the bus: clk/mosi/tx are driven, miso/rx are read, and i2c
# plus RMII mdio are open-drain/bidirectional and constrain to neither.
DIR_DRIVES, DIR_READS, DIR_BOTH = 'drives', 'reads', 'both'

ROLE_DIRECTION = {
    # Named "enable" and it is an INPUT: the inverter tells US whether closing
    # is allowed (SmaInverterBase::setup does pinMode(pin, INPUT), and
    # allows_contactor_closing() digitalReads it). Taking the name at face value
    # produced two false "can never assert" reports against 3lb pad 36 and
    # lilygo2can pad 46 - both correct declarations, both input-only pads.
    ('sma', 'enable'): DIR_READS,
    ('ap_button', 'pin'): DIR_READS,
    ('equipment_stop', 'pin'): DIR_READS,
    ('chademo', 'ct'): DIR_READS,
    ('chademo', 'pin4'): DIR_READS,
    ('chademo', 'pin7'): DIR_READS,
    ('chademo', 'pin2'): DIR_DRIVES,
    ('chademo', 'pin10'): DIR_DRIVES,
    ('chademo', 'lock'): DIR_DRIVES,
    ('can', 'rx'): DIR_READS,
    ('can', 'int'): DIR_READS,
    ('can', 'bus.miso'): DIR_READS,
    ('rs485', 'rx'): DIR_READS,
    ('sd_mmc', 'miso'): DIR_READS,
    ('sd_spi', 'miso'): DIR_READS,
    ('display_i2c', 'sda'): DIR_BOTH,
    ('display_i2c', 'scl'): DIR_BOTH,
    ('ethernet', 'mdio'): DIR_BOTH,
}

# Roles that need a pad property beyond direction.
ROLE_NEEDS_ADC = {('chademo', 'ct')}
ROLE_NEEDS_INTERRUPT = {('can', 'int')}
# A role whose level must survive a reset: only RTC pads can hold one.
ROLE_NEEDS_RTC_HOLD = {('contactors', 'bms_power')}


def role_direction(feature, role):
    """Everything not listed as read-or-both is driven by us.

    The default is the SAFE one: treating a driven role as readable would let
    the validator put it on an input-only pad, where it silently never asserts.
    """
    return ROLE_DIRECTION.get((feature, role), DIR_DRIVES)


def cap_name(feature):
    """The enumerator a feature key contributes. Overridable per feature; the
    default is the key in CamelCase."""
    override = FEATURES.get(feature, {}).get('cap')
    return override or ''.join(part.capitalize() for part in feature.split('_'))


def declared_pins(data):
    """Every GPIO a declaration puts a role on.

    Only fields that map to a pin getter count. Walking the declaration for
    anything numeric would sweep up the scalars too, and an LED brightness of
    40 or a 16 MHz crystal is not a GPIO - which is exactly the false positive
    the first version of this produced."""
    found = set()

    def take(value):
        if str(value).isdigit():
            found.add(int(value))
        else:
            for pad in candidate_pads(value) or ():
                found.add(pad)

    for feature in FEATURE_ORDER:
        try:
            declared = instances_of(data, feature)
        except DeclError:
            continue  # reported elsewhere; a broken feature has no pins to read
        for spec, index, inst, driver in declared:
            if index >= len(spec['instances']):
                continue
            _, bus = bus_of(data, inst, feature)
            if bus and 'bus' in spec and index < len(spec['bus']):
                for role in spec['bus'][index]:
                    take(bus.get(role))
            for role in spec['instances'][index]:
                take(inst.get(role))
    for out in data.get('outputs', []):
        take(out.get('gpio'))
    return found


def caps_of(data):
    """Every capability a board declaration carries, in emission order."""
    out = []
    for feature in FEATURE_ORDER:
        if not instances_of(data, feature):
            continue
        for cap in [cap_name(feature)] + FEATURES[feature].get('provides', []):
            if cap not in out:
                out.append(cap)
    return out


def all_caps():
    """Every capability the schema can express, in emission order."""
    out = []
    for feature in FEATURE_ORDER:
        for cap in [cap_name(feature)] + FEATURES[feature].get('provides', []):
            if cap not in out:
                out.append(cap)
    return out


def existing_cap_order(text):
    """The enumerators an already-generated capabilities.h carries, in file
    order. Ids are compile-time only and never persisted, so renumbering would
    be safe - the order is preserved anyway so that adding a feature shows up
    as one added line rather than a renumbered file."""
    if not text:
        return []
    body = text.split('enum class BoardCap : uint8_t {', 1)
    if len(body) < 2:
        return []
    out = []
    for line in body[1].split('};', 1)[0].splitlines():
        # The trailing marker on a retained cap has to come off, or the name
        # read back would carry it and the next run would treat the same cap
        # as a new one.
        name = line.split('//')[0].strip().rstrip(',').split('=')[0].strip()
        if name and name not in ('None', 'Count') and not name.startswith('//'):
            out.append(name)
    return out


def load_addons(directory=None):
    """Every add-on definition, keyed by name.

    An add-on is a module that plugs onto a board: a CAN controller on a header,
    a precharge controller, the isolated dual-FD card. The board says which pins
    it wires to it; the template says which signals the module needs and which
    way each one goes. Neither half can check itself, which is the point of
    having both.

    Directions are from the ADD-ON's side, as the templates state: `in` is a
    signal the module receives and the host therefore has to drive."""
    out = {}
    for path in sorted((directory or ADDONS).glob('*.yaml')):
        decl = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        if 'addon' not in decl:
            raise DeclError(f'{path.name}: no "addon:" name')
        out[decl['addon']] = decl
    return out


def check_addon(chip, addon, inst, bus_name, where, addons):
    """An attached module against its template: does the board give it every
    signal, and can the board's pins actually carry them?"""
    if addon not in addons:
        return [f'{where} names add-on "{addon}", which has no definition in Software/addons']
    template = addons[addon]
    errors = []
    needs_bus = (template.get('requires') or {}).get('bus')
    if needs_bus and bus_name is None:
        errors.append(f'{where} attaches "{addon}", which needs a {needs_bus} bus, but the '
                      f'instance references none')
    optional = set(template.get('optional') or [])
    for pin, direction in (template.get('pins') or {}).items():
        value = inst.get(pin)
        if not present(value):
            if direction == 'in' and pin not in optional:
                errors.append(f'{where} attaches "{addon}", which needs "{pin}" driven, but the '
                              f'board does not provide it')
            continue
        if direction != 'in' or not str(value).isdigit():
            continue
        if int(value) in INPUT_ONLY.get(chip, ()):
            errors.append(f'{where} attaches "{addon}" with "{pin}" on GPIO {value}, which is '
                          f'input-only on the {chip} and cannot drive it')
    return errors


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


def candidate_pads(value):
    """A role placed on a finite candidate set (`role: [18, 25]`).

    Runtime-chosen like `setting` - the getter stays hand-written - but the
    declaration states exactly which pads it can land on, so the chip check
    and the probe-plan safety reasoning get real pads instead of "anywhere".
    Returns the pads, or None when the value is not a candidate list."""
    if isinstance(value, list) and len(value) >= 2 and all(str(v).strip().isdigit() for v in value):
        return [int(v) for v in value]
    return None


def present(value):
    """Is the role declared at all (a number, or late-bound)?"""
    return value is not None and str(value) not in ('NC',)


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------

def validate(board, data, addons=None):
    errors = []
    addons = {} if addons is None else addons
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

            if 'addon' in inst:
                errors += check_addon(data.get('chip'), inst['addon'], inst, bus_name, where, addons)
            for name in spec.get('requires', []):
                if not present(field(name)):
                    errors.append(f'{where} needs "{name}", which the board does not define')
            if 'bus' in spec:
                if bus_name is None:
                    errors.append(f'{where} is on a bus but declares no "bus:"')
                else:
                    for name in ('clk', 'mosi', 'miso'):
                        if not present(bus.get(name)):
                            errors.append(f'{where} uses bus "{bus_name}", which does not define "{name}"')
            unknown = set(inst) - set(spec['instances'][index]) - {'driver', 'bus', 'addon'} - set(
                spec.get('scalars', [{}] * (index + 1))[index] if index < len(spec.get('scalars', [])) else {})
            if unknown:
                errors.append(f'{where} has unknown field(s) {sorted(unknown)}')
            for role in spec['instances'][index]:
                value = inst.get(role)
                if isinstance(value, list) and candidate_pads(value) is None:
                    errors.append(f'{where}.{role}: a candidate list must be two or more GPIO '
                                  f'numbers, got {value}')

    # The build macro feeds the #if chain in the generated capabilities.h, so a
    # missing or misspelled one silently leaves a board with no capability set
    # rather than failing to build.
    chip = data.get('chip')
    if chip not in VALID_CHIPS:
        errors.append(f'{board}: chip must be one of {VALID_CHIPS}, got "{chip}"')
    else:
        for other, pins in CHIP_ONLY_PINS.items():
            if other == chip:
                continue
            used = sorted({p for p in declared_pins(data) if p in pins})
            if used:
                errors.append(f'{board}: declares chip "{chip}" but uses GPIO '
                              f'{", ".join(map(str, used))}, which only the {other} has')
        used = sorted({p for p in declared_pins(data) if p in ABSENT_PINS.get(chip, ())})
        if used:
            errors.append(f'{board}: uses GPIO {", ".join(map(str, used))}, '
                          f'which the {chip} does not have')
    flash_mb = data.get('flash_mb')
    try:
        if int(str(flash_mb)) <= 0:
            raise ValueError
    except (TypeError, ValueError):
        errors.append(f'{board}: flash_mb must be a positive integer, got "{flash_mb}"')

    macro = data.get(MACRO_KEY)
    if not macro:
        errors.append(f'{board}: no "{MACRO_KEY}:" - name the build macro hal.cpp switches on')
    elif not re.fullmatch(r'HW_[A-Z0-9_]+', str(macro)):
        errors.append(f'{board}: macro "{macro}" is not of the form HW_SOMETHING')

    # Both card features emit SD_MISO_PIN/SD_MOSI_PIN/SD_SCLK_PIN, so a board
    # declaring both would define them twice. Caught here rather than left to
    # the compiler, since the declaration is where the mistake is made.
    if data.get('sd_mmc') and data.get('sd_spi'):
        errors.append(f'{board}: declares both sd_mmc and sd_spi; a board has one kind of '
                      f'card interface, and the two emit the same getters')

    # Two dedicated-function features on one GPIO is always a declaration
    # mistake: a CAN transceiver, an SD line, a display bus, an RMII management
    # pin cannot time-share a pad. The actuator features (contactors, outputs,
    # battery_wakeup, equipment_stop, sma, precharge_auto, chademo) are
    # deliberately NOT in this list - the Stark muxes those lines by design
    # (its product outputs ARE the contactor pins, its wake-up pins double as
    # outputs), so sharing there expresses the hardware rather than a typo.
    # Declared buses are counted once each: instances referencing the same bus
    # share its pins legitimately.
    # Within one by_driver feature, DIFFERENT drivers may share pins: the
    # LilyGo's MCP2515 and MCP2518FD add-ons plug onto the same header (cs 18,
    # int 35), and only one is ever fitted. The same driver claiming a pin
    # twice is still a mistake.
    claims = {}
    for feature in ('rs485', 'can', 'sd_mmc', 'sd_spi', 'display_i2c', 'rgb_led', 'ap_button', 'ethernet'):
        try:
            declared = instances_of(data, feature)
        except DeclError:
            continue  # already reported above
        for spec, index, inst, driver in declared:
            if index >= len(spec['instances']):
                continue  # over-declared instance count, already reported above
            for role in spec['instances'][index]:
                value = inst.get(role)
                if value is not None and str(value).isdigit():
                    claims.setdefault(int(value), []).append((feature, driver, f'{feature}.{role}'))
    for name, bus in data.get('buses', {}).items():
        for field_name, value in bus.items():
            if str(value).isdigit():
                claims.setdefault(int(value), []).append((f'bus {name}', None, f'bus {name}.{field_name}'))

    def conflicts(users):
        for i in range(len(users)):
            for j in range(i + 1, len(users)):
                a, b = users[i], users[j]
                a_bus, b_bus = a[0].startswith('bus '), b[0].startswith('bus ')
                if a_bus != b_bus:
                    # A bus line reused by an alternative fitment is real
                    # hardware: the 3LB's MCP2517 add-on takes its INT from
                    # the SPI MISO line the MCP2515 fitment uses as a bus pin.
                    continue
                if a_bus and a[0] != b[0]:
                    continue  # two buses may describe alternative wiring
                if not a_bus and a[0] == b[0] and a[1] != b[1]:
                    continue  # alternative fitments of the same feature
                return True
        return False

    for gpio, users in sorted(claims.items()):
        if len(users) > 1 and conflicts(users):
            errors.append(f'{board}: GPIO {gpio} is claimed by ' + ' and '.join(u[2] for u in users))

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
    if value is None or str(value) in LATE_BOUND or candidate_pads(value):
        # Not declared, chosen at runtime, or placed on a candidate set the
        # runtime picks from: the getter is hand-written, not generated.
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



CAP_HEADER_NOTE = """// GENERATED by tools/board_gen.py from Software/boards/*.yaml - do not edit.
// Rebuild with `python3 tools/board_gen.py`; CI fails if this file and the
// declarations disagree.
//
// What each board can do, as the union of the features its declaration
// carries. This is the tie between the board schema and the registries that
// gate rows on hardware: a row asks for a capability, and the answer comes
// from the same file that already says which pins the feature uses, so a
// board cannot advertise a feature it has no pins for.
//
// Ids are compile-time only - never stored, never sent - so they may be
// renumbered freely. The generator preserves the order it already emitted
// anyway, so that declaring a new feature is one added line here."""


def capabilities_header(boards, previous):
    """The generated capabilities.h. `boards` is [(board, data)] sorted by
    board; `previous` is the file's current text, read for its enumerator
    order."""
    order = existing_cap_order(previous)
    for cap in all_caps():
        if cap not in order:
            order.append(cap)
    known = set(all_caps())

    lines = [CAP_HEADER_NOTE, '',
             '#ifndef BE_DEVBOARD_HAL_CAPABILITIES_H',
             '#define BE_DEVBOARD_HAL_CAPABILITIES_H', '',
             '#include <stdint.h>', '',
             'enum class BoardCap : uint8_t {',
             '  None = 0,  // "no requirement on this axis", so it is never a board\'s capability']
    for cap in order:
        lines.append(f'  {cap},' + ('' if cap in known else '  // no declaration carries this any more'))
    lines += ['  Count,', '};', '',
              'inline constexpr uint8_t BOARD_CAP_COUNT = static_cast<uint8_t>(BoardCap::Count);',
              '',
              '// The sets below are bitmasks, so the whole enum has to fit one word.',
              'static_assert(BOARD_CAP_COUNT <= 64, "BoardCap has outgrown the 64-bit capability set");',
              '',
              'constexpr uint64_t board_cap_bit(BoardCap cap) {',
              '  return cap == BoardCap::None ? uint64_t{0} : (uint64_t{1} << static_cast<uint8_t>(cap));',
              '}', '',
              '// Per-board capability sets. Constant expressions on purpose: a per-board',
              '// build folds a requirement check away entirely, so a row gated off on this',
              '// board costs it nothing.']
    for board, data in boards:
        caps = caps_of(data)
        const = f'BOARD_CAPS_{board.upper()}'
        if caps:
            body = ' |\n    '.join(f'board_cap_bit(BoardCap::{c})' for c in caps)
        else:
            body = 'uint64_t{0}'
        lines.append(f'inline constexpr uint64_t {const} =')
        lines.append(f'    {body};')
    lines += ['',
              "// The active board's set, where the build has exactly one board - the same",
              '// macro hal.cpp switches on. A build that compiles every board (one image',
              '// for all of them) matches no branch and leaves this undefined, which is the',
              '// signal to bind the set at runtime from the board that was selected.']
    for i, (board, data) in enumerate(boards):
        lines.append(f'#{"if" if i == 0 else "elif"} defined({data[MACRO_KEY]})')
        lines.append(f'#define BE_BOARD_CAPS BOARD_CAPS_{board.upper()}')
    lines += ['#endif', '', '#endif  // BE_DEVBOARD_HAL_CAPABILITIES_H']
    return '\n'.join(lines) + '\n'


BLOCK_RE = re.compile(r'^  // ---- BEGIN GENERATED.*?^  // ---- END GENERATED ----\n',
                      re.S | re.M)


def splice(header_text, new_block):
    """Replace the generated block in a header, or report that it has none."""
    if not BLOCK_RE.search(header_text):
        raise DeclError('header has no generated block to replace')
    return BLOCK_RE.sub(lambda _: new_block, header_text, count=1)


def movable_roles(declared):
    """The union movable-role census: every (feature, role) some profile
    declares as user-movable - a candidate list or `setting`. `variant` is a
    BOARD variant (fitment picks it, not the user) and is excluded. Returns
    {(feature, role): {board: default_pad_or_None}} where the default is the
    declared pad's value: the first candidate of a candidate list (the
    hand-written getters all default to their first candidate - cited in
    test_board_validation), or None for a bare `setting` whose default lives
    only in the getter."""
    out = {}
    for board, data in declared:
        for feature in FEATURE_ORDER:
            spec = FEATURES.get(feature, {})
            try:
                declared_insts = instances_of(data, feature)
            except DeclError:
                continue
            for vspec, index, inst, driver in declared_insts:
                if index >= len(vspec['instances']):
                    continue
                for role in vspec['instances'][index]:
                    value = inst.get(role)
                    pads = candidate_pads(value)
                    if pads:
                        out.setdefault((feature, role), {})[board] = pads[0]
                    elif str(value) == 'setting':
                        out.setdefault((feature, role), {})[board] = None
    return out


def load_pin_sidecar(path):
    if path.exists():
        import json as _json
        return _json.loads(path.read_text(encoding='utf-8'))
    return {'roles': {}}


def update_pin_sidecar(sidecar, roles):
    """Allocate ids append-only, i18n_ids.json-style: never reused, removed
    roles stay as tombstones, a renamed role gets a NEW id (its overlay row is
    genuinely a different address)."""
    ids = sidecar['roles']
    next_id = max(ids.values(), default=-1) + 1
    for feature, role in sorted(roles):
        name = f'{feature}.{role}'
        if name not in ids:
            ids[name] = next_id
            next_id += 1
    return sidecar


def pin_rows(declared, sidecar_path):
    """Emit spec 2's per-role settings rows: the PIN_SETTINGS_ROWS(S_INT)
    macro the settings table splices in, plus the role map and per-profile
    declared defaults the audit and the wizard read. The key is an address
    (PIN<id>, zero-padded), never a mnemonic - NVS caps keys at 15 chars with
    no headroom, and numeric keys cannot drift or collide (spec 2)."""
    roles = movable_roles(declared)
    import json as _json
    sidecar = update_pin_sidecar(load_pin_sidecar(sidecar_path), roles.keys())
    sidecar_path.write_text(_json.dumps(sidecar, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    ids = sidecar['roles']

    live = sorted(roles.keys(), key=lambda fr: ids[f'{fr[0]}.{fr[1]}'])
    L = ['// GENERATED by tools/board_gen.py --pin-rows - do not edit.',
         '// One settings row per user-movable role (union across profiles).',
         '// Value semantics: a pad number, PIN_NC, or PIN_FOLLOW_PROFILE (the',
         '// default - an absent row means the same). Only deviations occupy NVS.',
         '#ifndef BE_DEVBOARD_SETTINGS_PIN_SETTINGS_ROWS_INC',
         '#define BE_DEVBOARD_SETTINGS_PIN_SETTINGS_ROWS_INC', '',
         '#define PIN_FOLLOW_PROFILE (-1)',
         '#define PIN_NC (-2)', '',
         '// clang-format off',
         '#define PIN_SETTINGS_ROWS(S_INT) \\']
    for feature, role in live:
        rid = ids[f'{feature}.{role}']
        key = f'PIN{rid:03d}'
        L.append(f'  S_INT({key},         "{key}",         nullptr,          I32,    SF_REBOOT_REQUIRED, '
                 f'PIN_FOLLOW_PROFILE, -2, 48) /* {feature}.{role} */ \\')
    L += ['', '// clang-format on', '',
          'struct PinRoleRow {',
          '  uint16_t role_id;',
          '  const char* nvs_key;',
          '  const char* feature;',
          '  const char* role;',
          '};', '',
          'inline constexpr PinRoleRow PIN_ROLE_ROWS[] = {']
    for feature, role in live:
        rid = ids[f'{feature}.{role}']
        L.append(f'    {{{rid}, "PIN{rid:03d}", "{feature}", "{role}"}},')
    L += ['};',
          f'inline constexpr uint16_t PIN_ROLE_ROWS_COUNT = {len(live)};', '',
          '// Declared per-profile defaults, where the profile DECLARES a pad (the',
          '// first candidate of a candidate list). A bare `setting` role has its',
          '// default only in the hand-written getter: no row here, and the audit',
          '// reports overlay presence for it without a comparison.',
          'struct PinProfileDefault {',
          '  const char* board;',
          '  uint16_t role_id;',
          '  int16_t pad;',
          '};', '',
          'inline constexpr PinProfileDefault PIN_PROFILE_DEFAULTS[] = {']
    count = 0
    for feature, role in live:
        rid = ids[f'{feature}.{role}']
        for board in sorted(roles[(feature, role)]):
            pad = roles[(feature, role)][board]
            if pad is not None:
                L.append(f'    {{"{board}", {rid}, {pad}}},')
                count += 1
    L += ['};',
          f'inline constexpr uint16_t PIN_PROFILE_DEFAULTS_COUNT = {count};', '',
          '#endif  // BE_DEVBOARD_SETTINGS_PIN_SETTINGS_ROWS_INC', '']
    return '\n'.join(L)


def runtime_tables(declared):
    """Emit spec 1's runtime tables: pad inventory, role defaults, role constraints.

    One generated TU compiled into the union image, so the on-device validator
    reads the SAME facts the static checkers do. Pad properties are per chip and
    role constraints are per (feature, role); only the defaults are per profile.

    The safety class column is taken from board_probe_plan.role_class() rather
    than restated here - one source, and the drift test pins that they agree.
    """
    sys.path.insert(0, str(Path(__file__).parent))
    import board_probe_plan as pp

    L = ['// GENERATED by tools/board_gen.py --runtime-tables - do not edit',
         '#ifndef BE_DEVBOARD_HAL_BOARD_PIN_TABLES_INC',
         '#define BE_DEVBOARD_HAL_BOARD_PIN_TABLES_INC', '',
         '#include <stdint.h>', '',
         '// Pad flags. INPUT_ONLY and RESERVED are refusals; STRAPPING is a warning',
         '// with explicit confirm; ADC/RTC are capabilities a role can require.',
         'enum : uint8_t {',
         '  PAD_INPUT_ONLY = 1u << 0,',
         '  PAD_STRAPPING  = 1u << 1,',
         '  PAD_RESERVED   = 1u << 2,',
         '  PAD_ADC        = 1u << 3,',
         '  PAD_RTC        = 1u << 4,',
         '  // The half of PAD_ADC a role can count on: ADC2 fails while WiFi is',
         '  // active, and this firmware always runs WiFi. ROLE_REQ_ADC means ADC1.',
         '  PAD_ADC1       = 1u << 5,',
         '};', '',
         '// Role direction, and what a role requires of the pad it lands on.',
         'enum : uint8_t { ROLE_DRIVES = 0, ROLE_READS = 1, ROLE_BOTH = 2 };',
         'enum : uint8_t {',
         '  ROLE_REQ_ADC       = 1u << 0,',
         '  ROLE_REQ_INTERRUPT = 1u << 1,',
         '  ROLE_REQ_RTC_HOLD  = 1u << 2,',
         '};', '',
         '// Safety class, from board_probe_plan.role_class() - the probe plan and',
         '// this table are checked against each other by test_board_validation.py.',
         'enum : uint8_t { ROLE_BENIGN = 0, ROLE_GUARDED_INPUT = 1, ROLE_ACTUATING = 2 };', '',
         'struct PadInfo {', '  uint8_t pad;', '  uint8_t flags;', '};', '',
         'struct RoleInfo {', '  const char* feature;', '  const char* role;',
         '  uint8_t direction;', '  uint8_t requires_mask;', '  uint8_t safety_class;', '};', '']

    for chip in sorted(set(RTC_CAPABLE) | set(ADC_CAPABLE)):
        pads = []
        highest = max(list(RTC_CAPABLE.get(chip, (0,))) + list(ADC_CAPABLE.get(chip, (0,)))
                      + list(RESERVED_PINS.get(chip, (0,))) + list(STRAPPING_PINS.get(chip, (0,)))
                      + list(INPUT_ONLY.get(chip, (0,))))
        for pad in range(highest + 1):
            if pad in CHIP_ONLY_PINS.get('esp32' if chip == 'esp32s3' else 'esp32s3', ()):
                continue  # a pad this part does not have at all
            if pad in ABSENT_PINS.get(chip, ()):
                continue  # a pad NO part of this chip has
            flags = 0
            flags |= 1 if pad in INPUT_ONLY.get(chip, ()) else 0
            flags |= 2 if pad in STRAPPING_PINS.get(chip, ()) else 0
            flags |= 4 if pad in RESERVED_PINS.get(chip, ()) else 0
            flags |= 8 if pad in ADC_CAPABLE.get(chip, ()) else 0
            flags |= 16 if pad in RTC_CAPABLE.get(chip, ()) else 0
            flags |= 32 if pad in ADC1_CAPABLE.get(chip, ()) else 0
            pads.append((pad, flags))
        name = chip.upper()
        L.append(f'inline constexpr PadInfo PADS_{name}[] = {{')
        for pad, flags in pads:
            L.append(f'    {{{pad}, {flags}}},')
        L += ['};', f'inline constexpr uint16_t PADS_{name}_COUNT = {len(pads)};', '']

    L.append('inline constexpr RoleInfo ROLES[] = {')
    seen = set()
    for feature in FEATURE_ORDER:
        spec = FEATURES.get(feature, {})
        variants = spec.get('by_driver', {}).values() if 'by_driver' in spec else [spec]
        for variant in variants:
            roles = []
            for inst in variant.get('instances', []):
                roles += list(inst)
            for bus in variant.get('bus', []):
                roles += ['bus.' + r for r in bus]
            for role in roles:
                if (feature, role) in seen:
                    continue
                seen.add((feature, role))
                klass = {'BENIGN': 0, 'GUARDED_INPUT': 1, 'ACTUATING': 2}[
                    pp.role_class(feature, f'{feature}.{role}')]
                direction = {'drives': 0, 'reads': 1, 'both': 2}[role_direction(feature, role)]
                mask = ((1 if (feature, role) in ROLE_NEEDS_ADC else 0)
                        | (2 if (feature, role) in ROLE_NEEDS_INTERRUPT else 0)
                        | (4 if (feature, role) in ROLE_NEEDS_RTC_HOLD else 0))
                L.append(f'    {{"{feature}", "{role}", {direction}, {mask}, {klass}}},')
    L += ['};', f'inline constexpr uint16_t ROLES_COUNT = {len(seen)};', '', '#endif', '']
    return '\n'.join(L)


def main():
    argv = sys.argv[1:]
    check = '--check' in argv
    if '--pin-rows' in argv:
        out = Path(argv[argv.index('--pin-rows') + 1])
        boards_dir = Path(argv[argv.index('--boards') + 1]) if '--boards' in argv else BOARDS
        decl = []
        for path in sorted(boards_dir.glob('*.yaml')):
            data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
            decl.append((data['board'], data))
        out.write_text(pin_rows(decl, Path(__file__).parent / 'pin_role_ids.json'),
                       encoding='utf-8', newline='\n')
        print(f'pin rows: {out}')
        return
    if '--runtime-tables' in argv:
        out = Path(argv[argv.index('--runtime-tables') + 1])
        boards_dir = Path(argv[argv.index('--boards') + 1]) if '--boards' in argv else BOARDS
        decl = []
        for path in sorted(boards_dir.glob('*.yaml')):
            data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
            decl.append((data['board'], data))
        out.write_text(runtime_tables(decl), encoding='utf-8', newline='\n')
        print(f'runtime tables: {out}')
        return
    boards = Path(argv[argv.index('--boards') + 1]) if '--boards' in argv else BOARDS
    headers = Path(argv[argv.index('--headers') + 1]) if '--headers' in argv else HEADERS

    hal_cpp = headers / 'hal.cpp'
    HAL_MACROS = hal_cpp.read_text(encoding='utf-8') if hal_cpp.exists() else ''

    try:
        addons = load_addons(boards.parent / 'addons' if '--boards' in argv else None)
    except DeclError as exc:
        print(f'add-on declaration error: {exc}', file=sys.stderr)
        sys.exit(1)

    updates, errors, declared = [], [], []
    for path in sorted(boards.glob('*.yaml')):
        try:
            data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
            board = data['board']
            board_errors = validate(board, data, addons)
            if board_errors:
                errors += board_errors
                continue
            target = headers / data['header']
            if not target.exists():
                errors.append(f'{board}: header {data["header"]} not found')
                continue
            text = target.read_text(encoding='utf-8')
            updates.append((target, text, splice(text, block(board, data))))
            declared.append((board, data))
        except DeclError as exc:
            errors.append(f'{path.stem}: {exc}' if not str(exc).startswith(path.stem) else str(exc))

    if errors:
        print('board declaration errors:', file=sys.stderr)
        for err in errors:
            print(f'  {err}', file=sys.stderr)
        sys.exit(1)

    # The capability header covers every board at once, so it is emitted only
    # once all the declarations have parsed and validated.
    caps_path = (boards.parent.parent / 'capabilities.h' if '--boards' in argv
                 else CAPABILITIES)
    if '--headers' in argv:
        caps_path = headers / 'capabilities.h'
    previous = caps_path.read_text(encoding='utf-8') if caps_path.exists() else ''
    updates.append((caps_path, previous, capabilities_header(declared, previous)))

    missing = ([b for b, d in declared if f'defined({d[MACRO_KEY]})' not in HAL_MACROS]
               if HAL_MACROS else [])
    if missing:
        errors.append('capabilities.h would emit a branch hal.cpp never takes, for: '
                      + ', '.join(missing))
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
