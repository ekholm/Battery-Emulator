#!/usr/bin/env python3
"""Prove the board declarations transcribe the headers exactly.

Every member line the generator emits must appear verbatim in the header it
was extracted from, and every constant getter in the header must be accounted
for - either emitted by the generator or explicitly recognised as one of the
runtime-conditional getters that stay hand-written.

This is what makes "transcription-verified line by line" a mechanical fact
rather than an eyeball claim, which matters because the acceptance criterion
is binary-identical firmware from ~196 hand-copyable pin assignments.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from board_gen import parse_yaml_subset  # noqa: E402

ROOT = Path(__file__).parent.parent
HAL = ROOT / 'Software' / 'src' / 'devboard' / 'hal'
GEN = HAL / 'generated'
BOARDS = {'3lb': 'hw_3LB.h', 'becom': 'hw_becom.h', 'devkit': 'hw_devkit.h',
          'lilygo': 'hw_lilygo.h', 'lilygo2can': 'hw_lilygo2can.h',
          'stark': 'hw_stark.h', 'waveshare': 'hw_waveshare.h'}

MEMBER = re.compile(r'^\s{2}(const char\*|virtual)\s')
# Truly constant: returns a bare GPIO_NUM_x / literal. These MUST be declared.
CONST_GETTER = re.compile(
    r'^\s*virtual\s+(?:gpio_num_t)\s+([A-Z0-9_]+)\(\)\s*\{\s*return\s+(GPIO_NUM_[A-Z0-9]+);\s*\}\s*$')
# Everything else that returns a pin is conditional and stays hand-written.
# Three distinct reasons, worth keeping separate in the mind:
#   user setting     BMS_POWER on gpioopt2, LED_PIN on gpioopt6, SD_*, DISPLAY_*
#   hardware variant is_fd() on the 2CAN, isStarkVersion1() on the Stark
#   non-pin scalars  MCP2517_FREQ, MCP2517_CLKODIV
COND_GETTER = re.compile(r'^\s*virtual\s+gpio_num_t\s+([A-Z0-9_]+)\(\)\s*\{(?!\s*return\s+GPIO_NUM_[A-Z0-9]+;\s*\})')

problems, checked, counts = [], 0, {}
for board, header in sorted(BOARDS.items()):
    htext = (HAL / header).read_text(encoding='utf-8')
    hlines = {l.rstrip() for l in htext.splitlines()}
    frag = (GEN / f'{board}_pins.inc').read_text(encoding='utf-8')

    emitted = []
    for line in frag.splitlines():
        if not MEMBER.match(line):
            continue
        emitted.append(line)
        checked += 1
        if line.rstrip() not in hlines:
            problems.append(f'{board}: generated line not present verbatim in {header}:\n    {line.strip()}')

    # Every constant getter in the header must be emitted, or it silently
    # stops being declarative the moment the header is switched to the include.
    emitted_names = {CONST_GETTER.match(l).group(1) for l in emitted if CONST_GETTER.match(l)}
    conditional = set()
    for line in htext.splitlines():
        m = CONST_GETTER.match(line)
        if m and m.group(1) not in emitted_names:
            problems.append(f'{board}: constant getter {m.group(1)}() in {header} is NOT in the declaration')
        c = COND_GETTER.match(line)
        if c:
            conditional.add(c.group(1))
            if c.group(1) in emitted_names:
                problems.append(f'{board}: {c.group(1)}() is conditional in {header} but the '
                                f'declaration claims a fixed pin - the generated value would be wrong')
    counts[board] = (len(emitted_names), len(conditional))

    # The conditional getters are not generated, but they must be DECLARED, or
    # a capability that needs one looks unsupported and validation would reject
    # hardware the board really has.
    declared_cond = set(parse_yaml_subset(
        (ROOT / 'Software' / 'boards' / f'{board}.yaml').read_text(encoding='utf-8'),
        f'{board}.yaml').get('conditional_pins', {}))
    for role in sorted(conditional - declared_cond):
        problems.append(f'{board}: {role}() is conditional in {header} but is not in conditional_pins')
    for role in sorted(declared_cond - conditional):
        problems.append(f'{board}: conditional_pins declares {role}, which {header} does not define')

print(f'checked {checked} generated member lines across {len(BOARDS)} boards')
for b, (dec, cond) in sorted(counts.items()):
    print(f'  {b:12} {dec:3} declared  {cond:3} conditional (hand-written)')
if problems:
    print('\n'.join(problems))
    print(f'\n{len(problems)} transcription problem(s)')
    sys.exit(1)
print('transcription: every generated line matches its header verbatim, no constant getter missed')

# ---------------------------------------------------------------------------
# Claim audit. Each header states what interfaces it has in two places:
# available_interfaces(), and name_for_comm_interface() returning "" to hide
# one. Neither is checked against the pin map today, and they disagree with it
# and with each other. Reported, not fatal: making it fatal would block on
# changes that alter behaviour, which stage 1 excludes.
# ---------------------------------------------------------------------------
IFACE_DRIVER = {'CanNative': 'native', 'CanAddonMcp2515': 'mcp2515',
                'CanFdNative': 'mcp2518fd', 'CanFdAddonMcp2518': 'mcp2518fd',
                'CanFdAddonMcp2518_2': 'mcp2518fd_2'}


def switch_names(text):
    m = re.search(r'name_for_comm_interface\(comm_interface comm\)\s*\{(.*?)\n  \}', text, re.S)
    return dict(re.findall(r'case comm_interface::(\w+):\s*\n\s*return\s+"([^"]*)";', m.group(1))) if m else {}


base_names = switch_names((HAL / 'hal.h').read_text(encoding='utf-8'))
notes = []
for board, header in sorted(BOARDS.items()):
    htext = (HAL / header).read_text(encoding='utf-8')
    data = parse_yaml_subset((ROOT / 'Software' / 'boards' / f'{board}.yaml').read_text(encoding='utf-8'),
                             f'{board}.yaml')
    supported = {s['driver'] for s in data.get('can', [])}
    listed = set(re.findall(r'comm_interface::(\w+)',
                            re.search(r'available_interfaces\(\)\s*\{(.*?)\n  \}', htext, re.S).group(1)))
    names = dict(base_names)
    names.update(switch_names(htext))
    for iface, driver in sorted(IFACE_DRIVER.items()):
        has = driver in supported
        if (iface in listed) and not has:
            notes.append(f'{board}: available_interfaces() lists {iface}, but the pin map has no '
                         f'{driver} - selecting it can only fail at runtime')
        if names.get(iface, '') != '' and not has:
            notes.append(f'{board}: the settings page offers {iface} ("{names[iface]}"), but the pin '
                         f'map has no {driver}')
        if has and iface not in listed and names.get(iface, '') != '':
            notes.append(f'{board}: the settings page offers {iface} ("{names[iface]}") and the pins '
                         f'exist, but available_interfaces() does not list it')

if notes:
    print(f'\nclaim audit: {len(notes)} disagreement(s) between the headers and the pin map')
    for n in notes:
        print(f'  NOTE {n}')
    print('  (reported only - reconciling these changes behaviour, which is not stage 1)')
else:
    print('claim audit: headers and pin map agree')
