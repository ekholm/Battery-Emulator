#!/usr/bin/env python3
"""Prove the declarations reproduce the original hand-written headers.

Three checks, against the headers as they were BEFORE any of this tooling
touched them (a git ref, so the proof stays meaningful after the migration
rather than comparing the generated block to itself):

  1. every line the generated block emits appears verbatim in the original;
  2. every member the original declared still exists - either in the block or
     hand-written below it. Nothing may quietly disappear;
  3. nothing is defined twice.

Check 2 enumerates members by NAME, never by matching the shape of their body.
An earlier version of this tool decided what was "constant" with the same
anchored regex the extractor used, and both missed the three getters that
carry a trailing comment - so three real pins were dropped from the
declarations and the check still reported green. A completeness check must not
share the assumption it is meant to police.

Usage: python3 tools/board_verify_transcription.py [--base REF]
"""
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from board_gen import BOARDS, HEADERS, ROOT, block, parse_yaml_subset, BLOCK_RE  # noqa: E402

# The commit this work branched from; its headers are the transcription truth.
DEFAULT_BASE = '6362af8e'

# Any member defined on one line: virtual T NAME() { ... }  /  const char* name()
MEMBER = re.compile(r'^\s*(?:virtual\s+)?[\w:<>*\s]+?\b(\w+)\(\)\s*(?:\{|$)')
CODE = re.compile(r'^\s{2}(?:const char\*|virtual)\s')


def baseline(ref, header):
    path = f'Software/src/devboard/hal/{header}'
    out = subprocess.run(['git', 'show', f'{ref}:{path}'], cwd=ROOT,
                         capture_output=True, text=True)
    if out.returncode:
        sys.exit(f'cannot read {path} at {ref}: {out.stderr.strip()}')
    return out.stdout


def members(text):
    """Every member name the text declares, whatever the body looks like."""
    names = set()
    for line in text.splitlines():
        m = MEMBER.match(line)
        if m and not line.strip().startswith(('//', '*', '#')):
            names.add(m.group(1))
    return names


def main():
    argv = sys.argv[1:]
    base = argv[argv.index('--base') + 1] if '--base' in argv else DEFAULT_BASE

    problems, checked = [], 0
    counts = {}
    for path in sorted(BOARDS.glob('*.yaml')):
        data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        board, header = data['board'], data['header']
        original = baseline(base, header)
        original_lines = {ln.rstrip() for ln in original.splitlines()}
        generated = block(board, data)
        current = (HEADERS / header).read_text(encoding='utf-8')

        # 1. verbatim
        emitted = [ln for ln in generated.splitlines() if CODE.match(ln)]
        for line in emitted:
            checked += 1
            if line.rstrip() not in original_lines:
                problems.append(f'{board}: generated line is not in {header} at {base}:\n'
                                f'    {line.strip()}')

        # 2. completeness, by name
        generated_names = members(generated)
        outside = BLOCK_RE.sub('', current) if BLOCK_RE.search(current) else current
        kept_names = members(outside)
        for name in sorted(members(original) - generated_names - kept_names):
            problems.append(f'{board}: {name}() was declared in {header} at {base} '
                            f'but is now in neither the generated block nor the header')

        # 3. no duplicates
        for name in sorted(generated_names & kept_names):
            problems.append(f'{board}: {name}() is defined both in the generated block '
                            f'and by hand in {header}')

        counts[board] = (len(generated_names), len(members(original)) - len(generated_names))

    print(f'checked {checked} generated lines against the headers at {base}')
    for b, (gen, hand) in sorted(counts.items()):
        print(f'  {b:12} {gen:3} generated  {hand:3} hand-written')
    if problems:
        print()
        print('\n'.join(problems))
        print(f'\n{len(problems)} problem(s)')
        sys.exit(1)
    print('transcription: every generated line is verbatim from the original header, '
          'every original member still exists, none defined twice')


if __name__ == '__main__':
    main()
