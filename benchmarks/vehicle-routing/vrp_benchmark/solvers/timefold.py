"""Timefold Solver (open-source edition), run as the Java program in
`benchmarks/vehicle-routing/timefold`.

The instance goes over in a plain-text file (fleet, capacity, demands and the
same rounded distance matrix); the routes come back on stdout. Build the jar
first: `mvn -f timefold/pom.xml -q package` (JDK 21+).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from ..instance import CvrpInstance
from . import SolverOutcome

PROJECT_DIR = Path(__file__).resolve().parents[2] / "timefold"
JAR = PROJECT_DIR / "target" / "timefold-vrp-bench.jar"


def write_input(instance: CvrpInstance, path: Path, *, seconds: float, seed: int) -> None:
    lines = [
        f"{len(instance.demands)} {instance.vehicle_count} {instance.capacity}",
        f"{int(seconds * 1000)} {seed}",
        " ".join(map(str, instance.demands)),
        *(" ".join(map(str, row)) for row in instance.distances),
    ]
    path.write_text("\n".join(lines) + "\n")


def parse_output(text: str) -> tuple[list[list[int]], int, int]:
    """`route n1 n2 ...` per vehicle, then `score HARD SOFT`."""
    routes: list[list[int]] = []
    score: tuple[int, int] | None = None
    for line in text.splitlines():
        tokens = line.split()
        if tokens[:1] == ["route"]:
            routes.append([int(token) for token in tokens[1:]])
        elif tokens[:1] == ["score"]:
            score = (int(tokens[1]), int(tokens[2]))
    if score is None:
        raise RuntimeError(f"no score line in the Timefold output:\n{text}")
    return routes, *score


def _java() -> str:
    home = os.environ.get("JAVA_HOME")
    if home and (Path(home) / "bin" / "java").exists():
        return str(Path(home) / "bin" / "java")
    found = shutil.which("java")
    if found is None:
        raise RuntimeError("the timefold backend needs a JDK 21+ (`java` or JAVA_HOME)")
    return found


def solve(instance: CvrpInstance, seconds: float, seed: int) -> SolverOutcome:
    if not JAR.exists():
        raise RuntimeError(
            f"{JAR} is missing; build it with `mvn -f {PROJECT_DIR}/pom.xml -q package`"
        )
    with tempfile.TemporaryDirectory() as tmp:
        input_path = Path(tmp) / "instance.txt"
        write_input(instance, input_path, seconds=seconds, seed=seed)
        completed = subprocess.run(
            [_java(), "-jar", str(JAR), str(input_path)],
            capture_output=True,
            text=True,
            check=True,
        )
    routes, hard, soft = parse_output(completed.stdout)
    # hard = -(capacity overflow), soft = -(total distance).
    return SolverOutcome(
        routes=routes,
        reported_distance=-soft,
        summary=f"{hard}hard/{soft}soft",
    )
