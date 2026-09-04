#!/usr/bin/env python3
"""Prove the transcription verifier's new-hardware exemption cannot rot.

Check 1 - every generated line is verbatim from the pre-tooling header - has an
escape hatch now, because a board that gains hardware the original header never
had has nothing to be verbatim from. An escape hatch in a check that exists to
catch silent drift is worth exactly as much as the guard around it, so these
cases require that guard to fail in every direction it should:

  - a generated member with no baseline and no acknowledgement is refused;
  - an acknowledgement for something the baseline DID have is refused, because
    that is a transcription pretending to be new hardware;
  - an acknowledgement nothing emits any more is refused, so the list shrinks
    when hardware goes away instead of accumulating;
  - and the shipped list still passes, or the three above prove nothing.

Usage: python3 tools/test_transcription_verify.py
"""
import contextlib
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import board_verify_transcription as bvt  # noqa: E402

failures = []


def run():
    """Run the verifier, returning (exit code, output)."""
    out = io.StringIO()
    code = 0
    try:
        with contextlib.redirect_stdout(out):
            bvt.main()
    except SystemExit as exc:
        code = exc.code or 0
    return code, out.getvalue()


def expect_reject(case, *must_mention):
    code, output = run()
    if code == 0:
        failures.append(f'{case}: accepted, and it must not be')
        return
    missing = [m for m in must_mention if m not in output]
    if missing:
        failures.append(f'{case}: rejected, but the message never mentions {missing}\n'
                        f'    {output.strip()[-400:]}')


def main():
    shipped = {b: dict(v) for b, v in bvt.NEW_SINCE_BASELINE.items()}

    # The shipped list passes. Without this the rejections below could be
    # rejecting for some reason that has nothing to do with the list.
    code, output = run()
    if code != 0:
        failures.append(f'the shipped acknowledgements do not verify:\n    {output.strip()[-400:]}')
    elif 'acknowledged as new hardware' not in output:
        failures.append('the summary does not say how many members are acknowledged as new')

    # A new member nobody acknowledged. This is the case that was silently
    # failing CI before the hatch existed, and it must still fail - loudly, and
    # naming the member - rather than passing now that a hatch is there.
    bvt.NEW_SINCE_BASELINE['dfrobot_edge101'] = {
        k: v for k, v in shipped['dfrobot_edge101'].items() if k != 'ETH_MDC_PIN'}
    expect_reject('an unacknowledged new member', 'ETH_MDC_PIN', 'NEW_SINCE_BASELINE')
    bvt.NEW_SINCE_BASELINE = {b: dict(v) for b, v in shipped.items()}

    # An acknowledgement for a member the original header DID have. That is a
    # transcription being waved through as new hardware, which is exactly the
    # drift check 1 exists to catch.
    bvt.NEW_SINCE_BASELINE['devkit'] = {'CAN_TX_PIN': 'not actually new'}
    expect_reject('an acknowledgement that shadows a real transcription',
                  'CAN_TX_PIN', 'transcription after all')
    bvt.NEW_SINCE_BASELINE = {b: dict(v) for b, v in shipped.items()}

    # An acknowledgement nothing emits. Left alone these accumulate until the
    # list is a graveyard nobody trusts.
    bvt.NEW_SINCE_BASELINE['dfrobot_edge101']['ETH_GONE_PIN'] = 'hardware that no longer exists'
    expect_reject('a stale acknowledgement', 'ETH_GONE_PIN', 'nothing')
    bvt.NEW_SINCE_BASELINE = {b: dict(v) for b, v in shipped.items()}

    # An acknowledgement with no reason is not an acknowledgement.
    bvt.NEW_SINCE_BASELINE['dfrobot_edge101']['ETH_MDC_PIN'] = '   '
    expect_reject('an acknowledgement with no reason', 'ETH_MDC_PIN', 'no reason')
    bvt.NEW_SINCE_BASELINE = {b: dict(v) for b, v in shipped.items()}

    # The exemption is a hatch in check 1, so pin that the checks AROUND it
    # still bite: an assertion nobody can re-run mechanically decays into a
    # belief. Both cases mutate a real schema file and restore it;
    # expect_reject re-runs the verifier in between.

    # Check 1 for a baselined member: a changed pin is not verbatim any more.
    devkit_yaml = Path(bvt.ROOT) / 'Software' / 'boards' / 'devkit.yaml'
    yaml_orig = devkit_yaml.read_text()
    import re as _re
    can_line = _re.search(r' *- \{driver: native, tx: (\d+), rx: \d+\}', yaml_orig)
    tx = can_line.group(1)
    try:
        devkit_yaml.write_text(yaml_orig.replace(f'tx: {tx}', f'tx: {int(tx) + 1}', 1))
        expect_reject('a baselined member whose generated line drifted', 'devkit')
    finally:
        devkit_yaml.write_text(yaml_orig)

    # Check 1 for a member emitted with a TRAILING specifier. `SD_SPI_BUS()
    # override` is the only shape in the tree that carries one, and while
    # MEMBER could not see it the drift above went entirely unnoticed on it:
    # the wrong spi_bus regenerated a wrong SPI bus into the header and the
    # verifier still reported every generated line verbatim. The case is here
    # rather than in the keyword tests because what failed was this file's
    # idea of what a member looks like, not the generator's.
    edge_yaml = Path(bvt.ROOT) / 'Software' / 'boards' / 'dfrobot_edge101.yaml'
    edge_orig = edge_yaml.read_text()
    assert 'spi_bus: VSPI' in edge_orig, 'the override-styled getter this case needs is gone'
    try:
        edge_yaml.write_text(edge_orig.replace('spi_bus: VSPI', 'spi_bus: HSPI', 1))
        expect_reject('an override-styled member whose generated line drifted',
                      'dfrobot_edge101', 'SD_SPI_BUS')
    finally:
        edge_yaml.write_text(edge_orig)

    # Check 2: a baselined member the schema stops emitting is caught from the
    # other side - which is also what still catches a renamed getter.
    try:
        devkit_yaml.write_text(yaml_orig.replace(can_line.group(0) + '\n', '', 1))
        expect_reject('a baselined member the schema dropped', 'neither the generated block nor the header')
    finally:
        devkit_yaml.write_text(yaml_orig)

    if failures:
        print(f'transcription exemptions: {len(failures)} FAILED')
        for f in failures:
            print(f'  - {f}')
        sys.exit(1)
    print('transcription exemptions: all cases pass '
          '(unacknowledged new members refused, stale acknowledgements refused)')


if __name__ == '__main__':
    main()
