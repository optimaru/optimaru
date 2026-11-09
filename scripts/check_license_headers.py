#!/usr/bin/env python3
"""Require the Apache-2.0 SPDX header and Timefold derivation notice on every engine header.

The engine under ``include/optimaru/`` is derived from Timefold Solver (see NOTICE), so
each file carries, as its first two lines::

    // SPDX-License-Identifier: Apache-2.0
    // Derived from Timefold Solver (Apache-2.0); modified by The optimaru Authors. See NOTICE.

    python3 scripts/check_license_headers.py     # exit 1 and list every file without it

Run from anywhere; paths resolve against the repository root.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "include" / "optimaru"
HEADER = (
    "// SPDX-License-Identifier: Apache-2.0\n"
    "// Derived from Timefold Solver (Apache-2.0); modified by The optimaru Authors. See NOTICE.\n"
)
SUFFIXES = {".hpp", ".h", ".hh", ".hxx", ".ipp", ".inl", ".tpp", ".cpp", ".cc", ".cxx"}


def main() -> int:
    files = sorted(p for p in ENGINE.rglob("*") if p.suffix in SUFFIXES and p.is_file())
    if not files:
        print(
            f"no engine headers found under {ENGINE.relative_to(ROOT)}", file=sys.stderr
        )
        return 1
    missing = [p for p in files if not p.read_text(encoding="utf-8").startswith(HEADER)]
    for path in missing:
        print(f"missing license header: {path.relative_to(ROOT)}", file=sys.stderr)
    if missing:
        print(
            f"{len(missing)}/{len(files)} engine headers lack the header; see the docstring.",
            file=sys.stderr,
        )
        return 1
    print(f"{len(files)}/{len(files)} engine headers carry the license header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
