#!/usr/bin/env python3
"""Keep every use-case package's ``_constraint.py`` byte-identical to the canonical one.

The constraint-spec base has one source of truth, ``python/optimaru/_constraint.py``.
Each use-case package carries a copy as ``use-cases/<slug>/python/<package>/_constraint.py``
so that it builds and imports standalone as well as inside the ``optimaru`` wheel (the
top-level setup.py re-roots each package as ``optimaru.<subpackage>``), on Windows,
macOS and Linux alike, without symlinks.

    python3 scripts/check_constraint_base.py                 # exit 1 on any drifted copy
    python3 scripts/check_constraint_base.py --require-all   # ...or on a package without one
    python3 scripts/check_constraint_base.py --write         # re-sync every existing copy
    python3 scripts/check_constraint_base.py --write use-cases/<slug>/python/<package>
                                                             # ...and add a copy to a package

Run from anywhere; paths resolve against the repository root.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CANONICAL = ROOT / "python" / "optimaru" / "_constraint.py"
FILENAME = "_constraint.py"


def packages() -> list[Path]:
    """Every importable package directly under a use case's python/ directory."""
    found = []
    for python_dir in sorted((ROOT / "use-cases").glob("*/python")):
        for candidate in sorted(python_dir.iterdir()):
            if (
                candidate.is_dir()
                and (candidate / "__init__.py").is_file()
                and candidate.name not in {"tests", "build"}
                and not candidate.name.startswith(".")
            ):
                found.append(candidate)
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="overwrite drifted copies with the canonical file")
    parser.add_argument("--require-all", action="store_true", help="also fail when a package has no copy")
    parser.add_argument("add", nargs="*", type=Path, help="with --write: package directories that get a new copy")
    args = parser.parse_args(argv)
    if args.add and not args.write:
        parser.error("package directories are only accepted with --write")

    canonical = CANONICAL.read_bytes()
    targets = [package / FILENAME for package in packages() if (package / FILENAME).exists()]
    for package in args.add:
        package = (Path.cwd() / package).resolve()
        if not (package / "__init__.py").is_file():
            parser.error(f"{package} is not a package directory (no __init__.py)")
        if package / FILENAME not in targets:
            targets.append(package / FILENAME)

    drifted = [target for target in targets if not target.exists() or target.read_bytes() != canonical]
    missing = [package for package in packages() if not (package / FILENAME).exists() and package / FILENAME not in targets]

    status = 0
    for target in drifted:
        relative = target.relative_to(ROOT)
        if args.write:
            target.write_bytes(canonical)
            print(f"synced  {relative}")
        else:
            print(f"DRIFT   {relative} differs from {CANONICAL.relative_to(ROOT)}")
            status = 1
    for package in missing:
        relative = package.relative_to(ROOT)
        if args.require_all:
            print(f"MISSING {relative}/{FILENAME}")
            status = 1
    in_sync = len(targets) - (0 if args.write else len(drifted))
    print(
        f"{in_sync}/{len(targets)} copies of {CANONICAL.relative_to(ROOT)} in sync; "
        f"{len(missing)} package(s) without a copy" + ("" if args.require_all else " (allowed)")
    )
    return status


if __name__ == "__main__":
    sys.exit(main())
