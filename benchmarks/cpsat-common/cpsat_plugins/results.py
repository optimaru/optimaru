"""Results files of the benchmark runners."""

from __future__ import annotations

import json
from pathlib import Path


def write_results(path: Path, results: list[dict]) -> None:
    """Rewrite `path` with what has finished, so an interrupted sweep keeps its results.

    The file is replaced in one step: a reader never sees half of it.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + ".part")
    partial.write_text(json.dumps(results, indent=2))
    partial.replace(path)
