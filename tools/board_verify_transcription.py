#!/usr/bin/env python3
"""Prove the declarations reproduce the original hand-written headers.

Three checks, against the headers as they were BEFORE any of this tooling
touched them (a git ref, so the proof stays meaningful after the migration
rather than comparing the generated block to itself):

  1. every line the generated block emits appears verbatim in the original,
     unless the member is acknowledged below as new since the baseline;
  2. every member the original declared still exists - either in the block or
     hand-written below it. Nothing may quietly disappear;
  3. nothing is defined twice.

Check 1 needed the escape hatch once the schema started expressing hardware the
original headers never had: the Edge101's Ethernet PHY cannot be verbatim from
a baseline that predates support for it. Moving the baseline forward would have
"fixed" this by blinding the check, so instead a new member has to be named in
NEW_SINCE_BASELINE with a reason - which keeps a board gaining hardware it never
declared before a deliberate act rather than a silent one. Those entries are
themselves checked: one whose member IS in the baseline, or which nothing emits
any more, is a stale exemption and fails.

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

# The transcription truth is the headers as they were before this tooling touched
# them - the last upstream commit this branch sits on. That cannot be a fixed SHA:
# pinned to the original fork point, this check hard-exited the moment upstream
# added an eighth board, because hw_dfrobot_edge101.h does not exist back there.
# Resolve it instead, and let --base override.
BASE_CANDIDATES = ('upstream/main', 'origin/main', 'main')


def default_base():
    for ref in BASE_CANDIDATES:
        out = subprocess.run(['git', 'merge-base', 'HEAD', ref], cwd=ROOT,
                             capture_output=True, text=True)
        if out.returncode == 0:
            return out.stdout.strip()
    sys.exit('cannot resolve a baseline from ' + ', '.join(BASE_CANDIDATES) +
             '. In CI this needs full history (fetch-depth: 0); '
             'otherwise pass --base REF.')

# Members the declarations emit that the original headers never had. Adding to
# this list is how new hardware support passes check 1; each entry says why it
# is not a transcription. Entries are validated in both directions, so the list
# cannot rot: it may not name something the baseline already had, nor something
# no board emits any more.
NEW_SINCE_BASELINE = {
    'dfrobot_edge101': {
        'ETH_MDC_PIN': 'Ethernet: the Edge101 has a PHY no hw_*.h expressed before the schema did',
        'ETH_MDIO_PIN': 'Ethernet, as above',
        'ETH_POWER_PIN': 'Ethernet, as above',
        'ETH_PHY_ADDR': 'Ethernet, as above',
        'ETH_CLK_MODE': 'Ethernet, as above',
    },
}

# Any member defined on one line: virtual T NAME() { ... }  /  const char* name()
MEMBER = re.compile(r'^\s*(?:virtual\s+)?[\w:<>*\s]+?\b(\w+)\(\)\s*(?:\{|$)')


def is_code(line):
    """Is this generated line a member definition rather than scaffolding?

    Derived from MEMBER, deliberately. This used to be its own regex listing
    the tokens a definition could start with - `const char*` or `virtual` -
    and the day the generator learned to emit a getter with neither (a member
    the base class does not declare, which must NOT be virtual), five real
    getters stopped being checked and the tool still reported green. That is
    the same failure this file's header warns about one paragraph up: a check
    that carries its own idea of what the thing looks like drifts away from
    the thing. The scaffolding is comments, blank lines and the #ifdef guards,
    and all three are cheap to name; a member definition is whatever MEMBER
    says one is.
    """
    if not line.startswith('  ') or line.startswith('#'):
        return False
    stripped = line.strip()
    if not stripped or stripped.startswith('//'):
        return False
    return MEMBER.match(line) is not None


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
    base = argv[argv.index('--base') + 1] if '--base' in argv else default_base()

    problems, checked = [], 0
    counts, new_counts = {}, {}
    for path in sorted(BOARDS.glob('*.yaml')):
        data = parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        board, header = data['board'], data['header']
        original = baseline(base, header)
        original_lines = {ln.rstrip() for ln in original.splitlines()}
        generated = block(board, data)
        current = (HEADERS / header).read_text(encoding='utf-8')

        # 1. verbatim, for everything that HAS an original to be verbatim from
        original_names = members(original)
        acknowledged = NEW_SINCE_BASELINE.get(board, {})
        emitted = [ln for ln in generated.splitlines() if is_code(ln)]
        emitted_new = set()
        for line in emitted:
            member = MEMBER.match(line)
            name = member.group(1) if member else None
            if name is not None and name not in original_names:
                # Nothing to compare against. Either this is acknowledged new
                # hardware, or it is a member that should have had a baseline -
                # a renamed or mistyped getter, which check 2 also catches from
                # the other side.
                emitted_new.add(name)
                if name not in acknowledged:
                    problems.append(
                        f'{board}: {name}() is emitted but {header} never had it at {base}. '
                        f'If the board genuinely gained this hardware, name it in '
                        f'NEW_SINCE_BASELINE with the reason; otherwise the declaration is '
                        f'describing something the header did not.')
                continue
            checked += 1
            if line.rstrip() not in original_lines:
                problems.append(f'{board}: generated line is not in {header} at {base}:\n'
                                f'    {line.strip()}')
        new_counts[board] = len(emitted_new & set(acknowledged))

        # 1b. the acknowledgements themselves, so the list cannot rot
        for name, reason in sorted(acknowledged.items()):
            if name in original_names:
                problems.append(f'{board}: {name}() is listed as new since {base}, but the header '
                                f'had it there. Remove the entry - it is a transcription after all.')
            elif name not in emitted_new:
                problems.append(f'{board}: {name}() is listed as new since {base}, but nothing '
                                f'emits it any more. Remove the entry.')
            elif not reason.strip():
                problems.append(f'{board}: {name}() is listed as new since {base} with no reason.')

        # 2. completeness, by name
        generated_names = members(generated)
        outside = BLOCK_RE.sub('', current) if BLOCK_RE.search(current) else current
        kept_names = members(outside)
        for name in sorted(original_names - generated_names - kept_names):
            problems.append(f'{board}: {name}() was declared in {header} at {base} '
                            f'but is now in neither the generated block nor the header')

        # 3. no duplicates
        for name in sorted(generated_names & kept_names):
            problems.append(f'{board}: {name}() is defined both in the generated block '
                            f'and by hand in {header}')

        # Hand-written means what it says: members defined outside the block.
        # This used to be derived as "original members minus generated", which
        # agreed with it only while every generated member came from the
        # original - the moment one did not, the column went negative.
        counts[board] = (len(generated_names), len(kept_names))

    print(f'checked {checked} generated lines against the headers at {base}')
    for b, (gen, hand) in sorted(counts.items()):
        new = new_counts.get(b, 0)
        suffix = f'  {new:3} new since the baseline' if new else ''
        print(f'  {b:12} {gen:3} generated  {hand:3} hand-written{suffix}')
    if problems:
        print()
        print('\n'.join(problems))
        print(f'\n{len(problems)} problem(s)')
        sys.exit(1)
    total_new = sum(new_counts.values())
    tail = (f'; {total_new} acknowledged as new hardware since then' if total_new else '')
    print('transcription: every generated line is verbatim from the original header, '
          f'every original member still exists, none defined twice{tail}')


if __name__ == '__main__':
    main()
