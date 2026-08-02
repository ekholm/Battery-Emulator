#!/usr/bin/env python3
"""Prove board_gen.py's capability validation rejects what it should.

A validator is only worth its runtime if it fails on bad input, and the way to
be sure is to feed it bad input rather than to read it. Each case below mutates
a real board declaration into something the hardware could not support and
requires a nonzero exit whose message names the board, the capability and the
missing role - and requires that nothing was written.

The passing cases matter as much: they pin the two requirement shapes that
exist in the drivers (the CAN-FD interrupt is INT *or* INT0+INT1; the second
CAN-FD chip needs its own SPI pins *only* on a separate bus), so a later
simplification of DRIVER_SPECS that flattened either one would fail here.

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
GEN = ROOT / 'tools' / 'board_gen.py'

failures = []


def run(yaml_by_board):
    """Run the generator over a temp board set; return (rc, output, files written)."""
    with tempfile.TemporaryDirectory() as tmp:
        boards, out = Path(tmp) / 'boards', Path(tmp) / 'out'
        boards.mkdir()
        out.mkdir()
        for board, text in yaml_by_board.items():
            (boards / f'{board}.yaml').write_text(text, encoding='utf-8')
        proc = subprocess.run(
            [sys.executable, str(GEN), '--boards', str(boards), '--out', str(out)],
            capture_output=True, text=True)
        return proc.returncode, proc.stdout + proc.stderr, sorted(p.name for p in out.iterdir())


def expect_reject(case, text, *must_mention, board='stark'):
    rc, output, written = run({board: text})
    if rc == 0:
        failures.append(f'{case}: accepted a declaration it must reject')
        return
    missing = [m for m in must_mention if m not in output]
    if missing:
        failures.append(f'{case}: rejected, but the message never mentions {missing}\n    {output.strip()}')
    if written:
        failures.append(f'{case}: rejected but still wrote {written}')


def expect_accept(case, text, board='stark'):
    rc, output, written = run({board: text})
    if rc != 0:
        failures.append(f'{case}: rejected a valid declaration\n    {output.strip()}')
    elif not written:
        failures.append(f'{case}: accepted but wrote nothing')


def drop_line(text, pattern):
    kept = [ln for ln in text.splitlines() if not re.match(pattern, ln)]
    if len(kept) == len(text.splitlines()):
        raise AssertionError(f'test setup: nothing matched {pattern!r}')
    return '\n'.join(kept) + '\n'


def main():
    stark = (BOARDS / 'stark.yaml').read_text(encoding='utf-8')
    lilygo2can = (BOARDS / 'lilygo2can.yaml').read_text(encoding='utf-8')

    # The declarations as committed must pass, or every rejection below is
    # meaningless - it would just be failing for an unrelated reason.
    expect_accept('committed stark', stark)
    expect_accept('committed lilygo2can', lilygo2can, board='lilygo2can')

    # A capability whose pin the board does not define.
    expect_reject('missing capability pin', drop_line(stark, r'\s+EQUIPMENT_STOP_PIN:'),
                  'stark', 'equipment_stop', 'EQUIPMENT_STOP_PIN')

    # NC means the board deliberately does not have the pin, so it must not
    # satisfy a requirement. This is the subtle one: the role IS present.
    expect_reject('required pin declared NC',
                  stark.replace('  EQUIPMENT_STOP_PIN: 2', '  EQUIPMENT_STOP_PIN: NC'),
                  'stark', 'equipment_stop', 'EQUIPMENT_STOP_PIN')

    # A conditional pin satisfies a requirement (the role exists, the GPIO is
    # chosen at runtime) - and removing it must break the same capability.
    expect_reject('missing conditional pin', drop_line(stark, r'\s+MCP2517_SCK: variant'),
                  'stark', 'canfd1', 'MCP2517_SCK')

    # CAN-FD interrupt: INT, or INT0+INT1. Dropping INT alone is fatal here
    # because stark has no INT0/INT1; a board with them may drop INT.
    expect_reject('canfd with no interrupt at all', drop_line(stark, r'\s+MCP2517_INT: '),
                  'stark', 'canfd1', 'MCP2517_INT')
    with_int01 = drop_line(stark, r'\s+MCP2517_INT: ').replace(
        '  MCP2517_CS: 18', '  MCP2517_CS: 18\n  MCP2517_INT0: 36\n  MCP2517_INT1: 39')
    expect_accept('canfd with INT0+INT1 instead of INT', with_int01)
    expect_reject('canfd with only half of INT0/INT1',
                  drop_line(with_int01, r'\s+MCP2517_INT1: '),
                  'stark', 'canfd1', 'MCP2517_INT')

    # Second CAN-FD chip: shares the first chip's SPI bus unless the declared
    # buses differ, in which case it needs its own three pins. stark shares.
    expect_reject('second canfd on its own bus without its own SPI pins',
                  stark.replace('scalars:', 'scalars:\n  MCP2517_BUS2: {type: uint8_t, value: "DEFAULT_MCP2515_BUS"}'),
                  'stark', 'canfd2', 'MCP2517_SCK2')

    # Typos must not pass silently as "some capability I do not know about".
    expect_reject('unknown capability',
                  stark.replace('capabilities: [rs485', 'capabilities: [rs485_typo'),
                  'stark', 'rs485_typo')
    expect_reject('unknown CAN driver',
                  stark.replace('driver: mcp2518fd_2', 'driver: mcp2519fd'),
                  'stark', 'mcp2519fd')
    expect_reject('duplicate CAN slot',
                  stark.replace('{slot: canfd2,', '{slot: canfd1,'),
                  'stark', 'canfd1')

    # Two product labels cannot name the same physical output.
    expect_reject('two outputs on one GPIO',
                  stark.replace('{label: "Output 3", gpio: 32', '{label: "Output 3", gpio: 33'),
                  'stark', 'Output 3', '33')

    # The parser must refuse what it does not understand rather than guess.
    expect_reject('unparseable line', stark.replace('pins:', 'pins:\n      DEEP: 1'), 'indent')

    if failures:
        print(f'board validation: {len(failures)} FAILED')
        for f in failures:
            print(f'  - {f}')
        sys.exit(1)
    print('board validation: all cases pass (rejections rejected, valid shapes accepted)')


if __name__ == '__main__':
    main()
