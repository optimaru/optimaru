#!/usr/bin/env python3
"""Keep every use-case package's ``_constraint.py`` byte-identical to the canonical one.

The constraint-spec base has one source of truth, ``python/optimaru/_constraint.py``.
Each use-case package carries a copy as ``use-cases/<slug>/python/<package>/_constraint.py``
so that it builds and imports standalone as well as inside the ``optimaru`` wheel (the
top-level setup.py re-roots each package as ``optimaru.<subpackage>``), on Windows,
macOS and Linux alike, without symlinks.

    python3 scripts/check_constraint_base.py                 # exit 1 on a drifted or missing copy
    python3 scripts/check_constraint_base.py --write         # re-sync every existing copy
    python3 scripts/check_constraint_base.py --write use-cases/<slug>/python/<package>
                                                             # ...and add a copy to a package

Every use case must carry a copy: a package without one, or a use case without any
package under python/, fails the check. ``--require-all`` is accepted and is the default.

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


def use_cases_without_package() -> list[Path]:
    """Use-case directories with no importable package under python/."""
    with_package = {package.parent.parent for package in packages()}
    return [
        use_case
        for use_case in sorted(path for path in (ROOT / "use-cases").iterdir() if path.is_dir())
        if not use_case.name.startswith(".") and use_case not in with_package
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="overwrite drifted copies with the canonical file")
    parser.add_argument(
        "--require-all", action="store_true", help="fail when a package has no copy (the default; kept for CI)"
    )
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
    bare = use_cases_without_package()

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
        print(f"MISSING {package.relative_to(ROOT)}/{FILENAME}")
        status = 1
    for use_case in bare:
        print(f"MISSING {use_case.relative_to(ROOT)}/python/<package>/{FILENAME} (no package)")
        status = 1
    in_sync = len(targets) - (0 if args.write else len(drifted))
    print(
        f"{in_sync}/{len(targets)} copies of {CANONICAL.relative_to(ROOT)} in sync; "
        f"{len(missing) + len(bare)} without a copy"
    )
    return status


if __name__ == "__main__":
    sys.exit(main())
