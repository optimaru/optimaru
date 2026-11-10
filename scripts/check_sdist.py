#!/usr/bin/env python3
"""Check that an sdist carries every file a wheel build needs, and no tests.

    python3 scripts/check_sdist.py dist/optimaru-*.tar.gz

Needed: the build files, the Python package (*.py, *.pyi, py.typed), the engine
headers and, per use case, cpp/bindings.cpp plus everything under cpp/include/
and cpp/src/ (setup.py compiles cpp/src/*.cpp but main.cpp; the headers may be
included from anywhere). The list comes from `git ls-files`, so a new file type
or directory that MANIFEST.in misses fails here instead of at release time.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NEEDED = re.compile(
    r"^(setup\.py|pyproject\.toml|LICENSE|NOTICE|THIRD-PARTY-LICENSES|docs/pypi\.md"
    r"|optimaru/.*\.pyi?|optimaru/(.*/)?py\.typed"
    r"|optimaru/cpp/.+"
    r"|optimaru/[^/]+/cpp/(bindings\.cpp|include/.+|src/.+))$"
)
FORBIDDEN = re.compile(r"^(tests|benchmarks|examples)/")


def main(sdist: str) -> int:
    with tarfile.open(sdist) as archive:
        names = {name.split("/", 1)[1] for name in archive.getnames() if "/" in name}
    tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    missing = sorted(path for path in tracked if NEEDED.match(path) and path not in names)
    extra = sorted(name for name in names if FORBIDDEN.match(name))
    for path in missing:
        print(f"missing from the sdist: {path}", file=sys.stderr)
    for path in extra:
        print(f"should not be in the sdist: {path}", file=sys.stderr)
    print(f"{sdist}: {len(names)} files, {len(missing)} missing, {len(extra)} unwanted")
    return 1 if missing or extra else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
