"""Give the custom_sdkconfig file a content-derived mtime, before the framework looks.

pioarduino guards its per-config framework rebuild with a stamp written to the
first line of the project's sdkconfig.defaults, and the stamp's hash input is
the declared custom_sdkconfig file's MTIME - never its content
(builder/frameworks/arduino.py, matching_custom_sdkconfig()). Every checkout or
branch switch that rewrites the file therefore forces a full framework
reinstall plus an IDF-libs rebuild (~2 min) for a configuration that has not
changed, and two worktrees with identical configuration carry different stamps.

Setting the file's mtime to a value derived from its content makes the stamp
content-keyed in effect: same bytes, same mtime, same stamp - across checkouts
and across worktrees. A pre: script demonstrably runs before the reinstall
decision, which is what makes this normalization possible repo-side.

This does NOT make two genuinely different configurations safe to build
concurrently - the compiled-libs package is global and unstamped, so the last
rebuild wins it. That is the platform's defect to fix; until then, one
configuration at a time.
"""

import hashlib
import os

Import("env")


def normalize(path):
    with open(path, "rb") as f:
        digest = hashlib.md5(f.read()).hexdigest()
    # Deterministic, content-derived, and in a plausible timestamp range so
    # nothing downstream is confused by a date far in the past or future.
    stamp = 1_600_000_000 + int(digest[:12], 16) % 100_000_000
    stat = os.stat(path)
    if int(stat.st_mtime) != stamp or stat.st_mtime != int(stat.st_mtime):
        os.utime(path, (stamp, stamp))
        print(f"normalize_sdkconfig_mtime: {os.path.basename(path)} -> {stamp} (content {digest[:8]})")


option = env.GetProjectOption("custom_sdkconfig", "")
for token in option.split():
    if token.startswith("file://"):
        path = token[len("file://"):]
        if not os.path.isabs(path):
            path = os.path.join(env.subst("$PROJECT_DIR"), path)
        if os.path.isfile(path):
            normalize(path)
