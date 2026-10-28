"""Run the Timefold quickstart models in benchmarks/timefold-quickstarts.

The jar solves or scores a quickstart solution given as the quickstart's own
JSON. Build it once with `mvn -f benchmarks/timefold-quickstarts/pom.xml
package` (JDK 21+); `OPTIMARU_TIMEFOLD_JAR` points elsewhere if needed.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from functools import cache
from pathlib import Path
from typing import Any

DEFAULT_JAR = (
    Path(__file__).resolve().parents[2]
    / "timefold-quickstarts"
    / "target"
    / "timefold-quickstarts-bench.jar"
)


def jar() -> Path:
    return Path(os.environ.get("OPTIMARU_TIMEFOLD_JAR", DEFAULT_JAR))


def java() -> str | None:
    home = os.environ.get("JAVA_HOME")
    if home and (Path(home) / "bin" / "java").exists():
        return str(Path(home) / "bin" / "java")
    return shutil.which("java")


@cache
def available() -> bool:
    """A working JDK (macOS ships a /usr/bin/java stub) and the built jar."""
    binary = java()
    if binary is None or not jar().exists():
        return False
    try:
        subprocess.run([binary, "-version"], check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def _run(mode: str, use_case: str, payload: dict, *extra: str) -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as tmp:
        source, target = Path(tmp) / "in.json", Path(tmp) / "out.json"
        source.write_text(json.dumps(payload))
        completed = subprocess.run(
            [java() or "java", "-jar", str(jar()), mode, use_case, str(source), str(target), *extra],
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            tail = "\n".join(completed.stderr.strip().splitlines()[-15:])
            raise RuntimeError(f"Timefold {mode} {use_case} failed:\n{tail}")
        return json.loads(target.read_text())


def solve(use_case: str, payload: dict, seconds: float, seed: int) -> dict[str, Any]:
    """`{"score": "<Timefold score>", "assignment": {Entity: {id: {variable: id}}}}`."""
    return _run("solve", use_case, payload, str(int(seconds * 1000)), str(seed))


def score(use_case: str, payload: dict) -> dict[str, Any]:
    """`{"score": "..."}`: the Timefold score of a given solution."""
    return _run("score", use_case, payload)
