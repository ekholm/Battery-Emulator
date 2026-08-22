#!/usr/bin/env python3
"""Pins tools/normalize_sdkconfig_mtime.py.

The script's whole value is one property: the declared sdkconfig file's mtime
is a pure function of its content. Same bytes must mean same mtime in every
worktree (that is what collapses five stamps into two), a content change must
mean a different mtime (that is what keeps the rebuild guard alive), and
re-running must change nothing (a script that kept bumping the mtime would
reintroduce the 2-minute reinstall on every build).

The script is an SCons extra_script (Import("env") at top), so it cannot be
imported; it is exec'd here with the two names SCons would inject.
"""

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SCRIPT = (ROOT / 'normalize_sdkconfig_mtime.py').read_text(encoding='utf-8')


class FakeEnv:
    def __init__(self, project_dir, option):
        self._dir = str(project_dir)
        self._option = option

    def GetProjectOption(self, name, default=""):
        assert name == 'custom_sdkconfig'
        return self._option

    def subst(self, s):
        assert s == '$PROJECT_DIR'
        return self._dir


def run_script(project_dir, option):
    exec(compile(SCRIPT, 'normalize_sdkconfig_mtime.py', 'exec'),
         {'Import': lambda *_: None, 'env': FakeEnv(project_dir, option)})


def main():
    failures = []

    def check(name, cond, detail=''):
        if not cond:
            failures.append(f'{name}: {detail}')

    with tempfile.TemporaryDirectory() as tmp:
        a = Path(tmp) / 'a.defaults'
        b = Path(tmp) / 'sub' / 'b.defaults'
        b.parent.mkdir()
        a.write_text('CONFIG_X=y\n')
        b.write_text('CONFIG_X=y\n')

        # Same content, two files, one absolute and one project-relative:
        # identical mtime - the cross-worktree stamp-agreement property.
        run_script(tmp, f'file://{a} file://sub/b.defaults')
        check('same content same mtime', os.stat(a).st_mtime == os.stat(b).st_mtime,
              f'{os.stat(a).st_mtime} != {os.stat(b).st_mtime}')

        # Idempotent: a second run leaves the mtime exactly where it is.
        before = os.stat(a).st_mtime
        run_script(tmp, f'file://{a}')
        check('idempotent', os.stat(a).st_mtime == before)

        # The mtime survives a checkout-style rewrite of the same bytes.
        content = a.read_text()
        a.write_text(content)  # bumps mtime like a fresh checkout would
        run_script(tmp, f'file://{a}')
        check('rewrite converges', os.stat(a).st_mtime == before)

        # A content change moves the mtime - the guard must still see it.
        a.write_text('CONFIG_X=y\nCONFIG_Y=y\n')
        run_script(tmp, f'file://{a}')
        check('content change moves mtime', os.stat(a).st_mtime != before)

        # Normalization never touches the bytes.
        check('content untouched', a.read_text() == 'CONFIG_X=y\nCONFIG_Y=y\n')

        # The stamp epoch stays in the plausible range the script promises.
        check('plausible range', 1_600_000_000 <= os.stat(a).st_mtime < 1_700_000_000,
              str(os.stat(a).st_mtime))

        # Non-file tokens and missing files are ignored, not crashed on.
        run_script(tmp, 'CONFIG_LITERAL=y file://does/not/exist.defaults')

    if failures:
        print('normalize_sdkconfig_mtime:', file=sys.stderr)
        for f in failures:
            print(f'  FAIL {f}', file=sys.stderr)
        sys.exit(1)
    print('normalize_sdkconfig_mtime: all cases pass (content-keyed, idempotent, guard-preserving)')


if __name__ == '__main__':
    main()
