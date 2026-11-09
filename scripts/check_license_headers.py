#!/usr/bin/env python3
"""Require the Apache-2.0 SPDX header and the origin notice on every engine header.

Each file under ``include/optimaru/`` starts with these two lines::

    // SPDX-License-Identifier: Apache-2.0
    // Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

    python3 scripts/check_license_headers.py     # exit 1 and list every file without it

The second line speaks of the engine as a whole, so it is the same on every engine file:
the engine began as a C++ port of parts of Timefold Solver and was later restructured
(see NOTICE), and the line is the modification notice of Apache-2.0 section 4(b) for the
parts that still derive from it. Use-case sources are out of scope; NOTICE and each use
case's README give their notice per directory.

A UTF-8 BOM and CRLF line endings are tolerated. Run from anywhere; paths resolve
against the repository root.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "include" / "optimaru"
HEADER = (
    "// SPDX-License-Identifier: Apache-2.0\n"
    "// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.\n"
)
SUFFIXES = {".hpp", ".h", ".hh", ".hxx", ".ipp", ".inl", ".tpp", ".cpp", ".cc", ".cxx"}


def main() -> int:
    files = sorted(p for p in ENGINE.rglob("*") if p.suffix in SUFFIXES and p.is_file())
    if not files:
        print(
            f"no engine headers found under {ENGINE.relative_to(ROOT)}", file=sys.stderr
        )
        return 1
    expected = HEADER.splitlines()
    missing = []
    for path in files:
        head = path.read_text(encoding="utf-8-sig").splitlines()[: len(expected)]
        if head != expected:
            missing.append((path, head[0] if head else ""))
    for path, first in missing:
        print(
            f"missing license header: {path.relative_to(ROOT)} (first line: {first!r})",
            file=sys.stderr,
        )
    if missing:
        print(
            f"{len(missing)}/{len(files)} engine headers lack the header. "
            f"Start each file with these two lines:\n{HEADER}",
            file=sys.stderr,
        )
        return 1
    print(f"{len(files)}/{len(files)} engine headers carry the license header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
