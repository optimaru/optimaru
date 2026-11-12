"""Files the benchmark runners write (results, and downloads they cache), and the options they share."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path


def write_whole(path: Path, payload: bytes) -> None:
    """Put `payload` at `path` in one step: a reader never sees half of it.

    It is written beside the target under a name of this process and moved over it, so
    several runners that write the same file at once (lanes that start on an empty
    download cache) each finish, and the last one wins.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(f"{path.name}.{os.getpid()}.part")
    partial.write_bytes(payload)
    partial.replace(path)


def write_results(path: Path, results: list[dict]) -> None:
    """Rewrite `path` with what has finished, so an interrupted sweep keeps its results."""
    write_whole(path, json.dumps(results, indent=2).encode())


def seed_list(text: str) -> list[int]:
    """`--synthetic-seeds` of the public-instance runners: distinct whole numbers, comma-separated.

    A repeated seed would give two instances of one name, so it is refused like a bad number.
    """
    try:
        seeds = [int(seed) for seed in text.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a comma-separated list of seeds: {text!r}") from None
    if len(set(seeds)) != len(seeds):
        raise argparse.ArgumentTypeError(f"a seed is given twice: {text!r}")
    return seeds
