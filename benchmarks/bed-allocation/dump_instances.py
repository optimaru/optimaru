"""Dump the engine/CP-SAT comparison instances to a plain-text format.

The three deterministic instances the ``bed-allocation-compare`` harness
builds in Python (``bed_allocation_cpsat.benchmark.instances``) are written to
``<outdir>/<name>.batxt`` so the C++ engine-only bench
(``use-cases/bed-allocation/cpp/bench/ba_quality_bench.cpp``) can solve EXACTLY the same problems
without a Python round-trip per run.

Run from the repo root:

    uv run --project benchmarks/bed-allocation/cpsat \
        python benchmarks/bed-allocation/dump_instances.py <outdir>

Format (whitespace-separated tokens; every name in these instances is a
single token, which the dumper asserts):

    BA1
    departments <n>
      <id> <name> <minAge|-> <maxAge|-> <nspec> {<specialty> <priority>}...
    rooms <n>
      <id> <name> <dept> <capacity> <genderLimitation-name> <nequip> {<equip>}...
    beds <n>
      <id> <room> <indexInRoom>
    stays <n>
      <id> <patientName> <gender-name> <age> <prefMaxCap|-> <arrival>
      <departure> <specialty> <nreq> {<equip>}... <npref> {<equip>}...
"""

from __future__ import annotations

import sys
from pathlib import Path

from bed_allocation_cpsat import benchmark


def _token(text: str) -> str:
    if not text or any(ch.isspace() for ch in text):
        raise ValueError(f"not a single token: {text!r}")
    return text


def _opt(value) -> str:
    return "-" if value is None else str(value)


def dump(plan, path: Path) -> None:
    lines: list[str] = ["BA1"]
    lines.append(f"departments {len(plan.departments)}")
    for dept in plan.departments:
        spec = dept.specialty_to_priority
        parts = [
            _token(dept.id),
            _token(dept.name),
            _opt(dept.minimum_age),
            _opt(dept.maximum_age),
            str(len(spec)),
        ]
        for name in sorted(spec):
            parts += [_token(name), str(spec[name])]
        lines.append(" ".join(parts))
    lines.append(f"rooms {len(plan.rooms)}")
    for room in plan.rooms:
        parts = [
            _token(room.id),
            _token(room.name),
            str(room.department),
            str(room.capacity),
            _token(room.gender_limitation.name),
            str(len(room.equipment)),
        ]
        parts += [_token(e) for e in room.equipment]
        lines.append(" ".join(parts))
    lines.append(f"beds {len(plan.beds)}")
    for bed in plan.beds:
        lines.append(f"{_token(bed.id)} {bed.room} {bed.index_in_room}")
    lines.append(f"stays {len(plan.stays)}")
    for stay in plan.stays:
        parts = [
            _token(stay.id),
            _token(stay.patient_name),
            _token(stay.patient_gender.name),
            str(stay.patient_age),
            _opt(stay.patient_preferred_maximum_room_capacity),
            str(stay.arrival_day),
            str(stay.departure_day),
            _token(stay.specialty),
            str(len(stay.patient_required_equipment)),
        ]
        parts += [_token(e) for e in stay.patient_required_equipment]
        parts.append(str(len(stay.patient_preferred_equipment)))
        parts += [_token(e) for e in stay.patient_preferred_equipment]
        lines.append(" ".join(parts))
    path.write_text("\n".join(lines) + "\n")


def main(argv: list[str]) -> int:
    outdir = Path(argv[1]) if len(argv) > 1 else Path("out")
    outdir.mkdir(parents=True, exist_ok=True)
    for name, factory in benchmark.instances().items():
        plan = factory()
        path = outdir / f"{name}.batxt"
        dump(plan, path)
        print(f"{name}: {benchmark.describe(plan)} -> {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
