#!/usr/bin/env python3
"""One-time migration: move each board header's constant definitions into a
generated block.

Removes the getters the declaration now owns, drops the comments that only
introduced them, and inserts the generated block at the top of the class. What
is left is exactly the hand-written part: getters whose pin is chosen at
runtime, the interface lists and the board helpers.

Run once. After this, tools/board_gen.py maintains the block in place.

Usage: python3 tools/board_switch_headers.py [--dry-run]
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from board_gen import HEADERS, BOARDS, block, parse_yaml_subset  # noqa: E402

DEFINES = re.compile(r'^\s*(?:virtual\s+)?[\w:<>*\s]+?\b(\w+)\(\)\s*\{.*\}\s*(?://.*)?$')


def defined_names(text):
    """Names of the one-line members the generated block defines."""
    return {m.group(1) for line in text.splitlines() if (m := DEFINES.match(line))}


def migrate(header_text, generated):
    owned = defined_names(generated)
    lines = header_text.splitlines()

    drop = [False] * len(lines)
    for i, line in enumerate(lines):
        m = DEFINES.match(line)
        if m and m.group(1) in owned:
            drop[i] = True

    # A comment line goes only if the next thing it introduces is going too.
    for i in range(len(lines) - 1, -1, -1):
        if drop[i] or not lines[i].strip().startswith('//'):
            continue
        for j in range(i + 1, len(lines)):
            if not lines[j].strip():
                continue
            drop[i] = drop[j]
            break

    kept = [ln for ln, d in zip(lines, drop) if not d]

    # Insert the block right after the class's "public:".
    for i, line in enumerate(kept):
        if line.strip() == 'public:':
            kept[i + 1:i + 1] = generated.rstrip('\n').split('\n')
            break
    else:
        raise SystemExit('no "public:" found - cannot place the generated block')

    # Collapse the blank runs the removals left behind.
    out, blank = [], False
    for line in kept:
        if not line.strip():
            if blank:
                continue
            blank = True
        else:
            blank = False
        out.append(line)
    return '\n'.join(out) + '\n'


def main():
    dry = '--dry-run' in sys.argv
    for path in sorted(BOARDS.glob('*.yaml')):
        data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        target = HEADERS / data['header']
        text = target.read_text(encoding='utf-8')
        if '---- BEGIN GENERATED' in text:
            print(f'{data["board"]:12} already migrated, skipping')
            continue
        new = migrate(text, block(data['board'], data))
        removed = len(text.splitlines()) - len(new.splitlines())
        print(f'{data["board"]:12} {len(text.splitlines()):4} -> {len(new.splitlines()):4} lines')
        if not dry:
            target.write_text(new, encoding='utf-8')


if __name__ == '__main__':
    main()
