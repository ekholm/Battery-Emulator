#!/usr/bin/env python3
"""Prove board_gen.py rejects declarations it should reject.

A validator is only worth its runtime if it fails on bad input, and the way to
be sure is to feed it bad input rather than to read it. Each case mutates a
real declaration into something the hardware could not support and requires a
nonzero exit whose message names the board, the feature and the field - and
requires that no header was touched.

The accepted cases pin the requirement shapes that exist in the drivers, so a
later simplification that flattened one of them fails here: the CAN-FD
interrupt is INT *or* INT0+INT1; a pin that is chosen at runtime still counts
as present; a pin declared NC does not.

Usage: python3 tools/test_board_validation.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent.parent
BOARDS = ROOT / 'Software' / 'boards'
HEADERS = ROOT / 'Software' / 'src' / 'devboard' / 'hal'
GEN = ROOT / 'tools' / 'board_gen.py'

failures = []


def run(board, text):
    """Run the generator over one declaration in a sandbox; return
    (rc, output, whether any header changed, the headers as generated).

    The generated text is returned rather than read back from the tree: a
    check that opens the real header proves nothing about this run, because
    the file still holds whatever the last real generator run left there."""
    with tempfile.TemporaryDirectory() as tmp:
        boards, headers = Path(tmp) / 'boards', Path(tmp) / 'hal'
        boards.mkdir()
        headers.mkdir()
        for src in HEADERS.glob('hw_*.h'):
            shutil.copy(src, headers / src.name)
        before = {p.name: p.read_text(encoding='utf-8') for p in headers.iterdir()}
        (boards / f'{board}.yaml').write_text(text, encoding='utf-8')
        proc = subprocess.run(
            [sys.executable, str(GEN), '--boards', str(boards), '--headers', str(headers)],
            capture_output=True, text=True)
        after = {p.name: p.read_text(encoding='utf-8') for p in headers.iterdir()}
        # capabilities.h lands in the same directory but is not a board header;
        # excluded so that "did a header change" keeps meaning what it says.
        touched = {k: v for k, v in after.items() if k != 'capabilities.h'}
        return proc.returncode, proc.stdout + proc.stderr, before != touched, after


def expect_reject(case, text, *must_mention, board='stark'):
    rc, output, changed, _ = run(board, text)
    if rc == 0:
        failures.append(f'{case}: accepted a declaration it must reject')
        return
    missing = [m for m in must_mention if m not in output]
    if missing:
        failures.append(f'{case}: rejected, but the message never mentions {missing}\n'
                        f'    {output.strip()}')
    if changed:
        failures.append(f'{case}: rejected but still rewrote a header')


def expect_accept(case, text, board='stark'):
    rc, output, _, _ = run(board, text)
    if rc != 0:
        failures.append(f'{case}: rejected a valid declaration\n    {output.strip()}')


def drop(text, pattern):
    """Remove a key: value pair from an inline map."""
    new = re.sub(pattern, '', text, count=1)
    if new == text:
        raise AssertionError(f'test setup: nothing matched {pattern!r}')
    return new


def main():
    stark = (BOARDS / 'stark.yaml').read_text(encoding='utf-8')
    lilygo = (BOARDS / 'lilygo.yaml').read_text(encoding='utf-8')

    # If the committed declarations did not pass, every rejection below would
    # be meaningless - it would just be failing for an unrelated reason.
    expect_accept('committed stark', stark)
    expect_accept('committed lilygo', lilygo, board='lilygo')

    # A feature missing a pin its driver reads.
    expect_reject('missing required field', drop(stark, r'positive: 32, '),
                  'stark', 'contactors', 'positive')

    # NC means the board deliberately lacks the pin, so it must not satisfy a
    # requirement even though the field is present. This is the subtle one.
    expect_reject('required field declared NC', stark.replace('positive: 32', 'positive: NC'),
                  'stark', 'contactors', 'positive')

    # A pin chosen at runtime is still a real role: the getter is hand-written,
    # but the hardware is there, so the feature must validate.
    expect_accept('late-bound pin satisfies a requirement',
                  stark.replace('positive: 32', 'positive: setting'))

    # CAN-FD interrupt: INT, or INT0+INT1, and half of the pair is not enough.
    expect_reject('canfd with no interrupt', drop(stark, r', int: 35'),
                  'stark', 'can', 'mcp2518fd', 'int')
    expect_accept('canfd with INT0+INT1 instead of INT',
                  drop(stark, r', int: 35').replace('cs: 18', 'cs: 18, int0: 36, int1: 39'))
    expect_reject('canfd with only half of INT0/INT1',
                  drop(stark, r', int: 35').replace('cs: 18', 'cs: 18, int0: 36'),
                  'stark', 'can', 'int')

    # Buses are declared once and referenced; a dangling or incomplete bus is
    # exactly the kind of thing the old flat pin map could not express.
    expect_reject('reference to an undeclared bus', stark.replace('bus: SPI1', 'bus: SPI9'),
                  'stark', 'SPI9')
    expect_reject('bus missing a pin', drop(stark, r'clk: variant, '),
                  'stark', 'SPI1', 'clk')

    # Typos must not pass silently.
    expect_reject('unknown driver', stark.replace('driver: native', 'driver: nativ'),
                  'stark', 'nativ')
    expect_reject('unknown field', stark.replace('cs: 12, int: 14', 'cs: 12, int: 14, csx: 9'),
                  'stark', 'csx')
    expect_reject('more instances than the driver has',
                  stark.replace('  - {driver: mcp2518fd, bus: SPI1, cs: 12, int: 14}',
                                '  - {driver: mcp2518fd, bus: SPI1, cs: 12, int: 14}\n'
                                '  - {driver: mcp2518fd, bus: SPI1, cs: 7, int: 8}'),
                  'stark', 'mcp2518fd')

    # sd_spi: the SPI-attached card. Its chip select is not optional the way
    # SD_MMC's is - an SPI slave with no CS is not addressable - and a board
    # has one kind of card interface, never both.
    dfrobot = (BOARDS / 'dfrobot_edge101.yaml').read_text(encoding='utf-8')
    expect_accept('committed dfrobot_edge101', dfrobot, board='dfrobot_edge101')
    expect_reject('sd_spi without chip select', drop(dfrobot, r', cs: 5'),
                  'dfrobot_edge101', 'sd_spi', 'cs', board='dfrobot_edge101')
    expect_reject('both card interfaces on one board',
                  dfrobot.replace('sd_spi:', 'sd_mmc:\n  - {miso: 39, mosi: 12, clk: 14}\nsd_spi:'),
                  'dfrobot_edge101', 'sd_mmc', 'sd_spi', board='dfrobot_edge101')

    # The guard is what makes sd_spi expressible at all: the group is compiled
    # out unless SDCARD is defined, so the block has to carry the #ifdef. A
    # generator that emitted the getters bare would still pass every other
    # check here and silently change what the board compiles.
    rc, output, _, generated = run('dfrobot_edge101', dfrobot)
    if rc != 0:
        failures.append(f'sd_spi emission: the generator failed\n    {output.strip()}')
    else:
        header = generated['hw_dfrobot_edge101.h']
        for marker in ('#ifdef SDCARD', '#endif  // SDCARD', 'uint8_t SD_SPI_BUS() override'):
            if marker not in header:
                failures.append(f'sd_spi emission: the generated header lacks {marker!r}')

    # Two product labels cannot name the same physical output.
    expect_reject('two outputs on one GPIO',
                  stark.replace('{label: "Output 3", gpio: 32', '{label: "Output 3", gpio: 33'),
                  'stark', 'Output 3', '33')

    # The parser refuses what it does not understand rather than guessing.
    expect_reject('unparseable indent', stark.replace('rs485:', 'rs485:\n      deep: 1'),
                  'indent')

    # A board with no build macro, or one that is not a macro, gets no branch in
    # the capability header's #if chain - and would silently have no capability
    # set rather than failing to build.
    expect_reject('no macro', drop(stark, r'macro: HW_STARK\n'), 'stark', 'macro')
    expect_reject('macro is not one', stark.replace('macro: HW_STARK', 'macro: stark'),
                  'stark', 'HW_SOMETHING')

    # THE MIGRATION'S SAFETY ARGUMENT, checked rather than asserted: a row that
    # asks for BoardCap::SdCard must be live on exactly the builds where the
    # `#ifdef SDCARD` it replaces is true. The two are derived from different
    # files - the declarations and platformio.ini - so nothing but this test
    # keeps them in step, and if they ever diverge the migration has silently
    # changed which boards log to a card.
    ini = (ROOT / 'platformio.ini').read_text(encoding='utf-8')
    sdcard_macros = set()
    for env in re.split(r'^\[env:', ini, flags=re.M)[1:]:
        block = env.split('\n[', 1)[0]
        if re.search(r'^\s*-D SDCARD\s*$', block, re.M):
            sdcard_macros.update(re.findall(r'-D (HW_[A-Z0-9_]+)', block))
    sys.path.insert(0, str(ROOT / 'tools'))
    import board_gen
    declaring = set()
    for path in sorted(BOARDS.glob('*.yaml')):
        data = board_gen.parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        if 'SdCard' in board_gen.caps_of(data):
            declaring.add(data[board_gen.MACRO_KEY])
    if declaring != sdcard_macros:
        failures.append('SdCard capability and -D SDCARD disagree: declared by '
                        f'{sorted(declaring)}, built with the define on {sorted(sdcard_macros)}')

    # Ids are preserved across regeneration. A feature added in the middle of
    # the emission order must not renumber the enumerators already emitted -
    # that is the difference between one added line and a rewritten file.
    caps = board_gen.capabilities_header([], '')
    grown = board_gen.capabilities_header([], caps.replace('  Rs485,', '  Ethernet,\n  Rs485,'))
    order = board_gen.existing_cap_order(grown)
    if order[:1] != ['Ethernet']:
        failures.append(f'capability ids are not append-only: {order[:3]} lost its first entry')
    if 'Ethernet' in grown and 'no declaration carries this any more' not in grown:
        failures.append('a capability no declaration carries is kept but not marked as such')

    if failures:
        print(f'board validation: {len(failures)} FAILED')
        for f in failures:
            print(f'  - {f}')
        sys.exit(1)
    print('board validation: all cases pass (rejections rejected, valid shapes accepted)')


if __name__ == '__main__':
    main()
