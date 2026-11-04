"""Write benchmark results into the README tables and the homepage data file.

Usage::

    python scripts/publish_benchmarks.py RESULTS_DIR --date 2026-11-04 \\
        --machine "one Apple M4 Max Mac (16 cores)" --machine-ko "<the same in Korean>" \\
        [--homepage ../homepage]

RESULTS_DIR holds what the harnesses wrote:

- ``compare/<slug>.json``: ``python -m <slug>_cpsat.benchmark --json`` of each use case;
- ``vrp.json``: ``python -m vrp_benchmark --series x --out vrp.json``;
- ``extra.json``: the numbers the other harnesses print as text, copied by hand:
  ``{"timetabling": [{"size": {"en": ..., "ko": ...}, "engine": 231, "ortools": 234}, ...],
  "public": [<homepage PublicRow>, ...]}`` (school-timetabling track 2 weighted penalties;
  the public-instance rows of the homepage).

Every README results block between ``<!-- results:start -->`` and ``<!-- results:end -->``
of a use case in ``STANDARD`` is regenerated, and so is the vehicle-routing one. The
homepage file (``src/data/benchmarks.ts``) is rewritten as a whole; its use-case labels are
kept from the current file.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCHMARKS = ROOT / "benchmarks"

#: Use cases whose README results block is the plain engine vs CP-SAT table.
STANDARD = (
    "bed-allocation",
    "conference-scheduling",
    "employee-scheduling",
    "facility-location",
    "flight-crew-scheduling",
    "food-packaging",
    "knapsack",
    "maintenance-scheduling",
    "max-cut",
    "meeting-scheduling",
    "order-picking",
    "quadratic-assignment",
    "semiconductor-scheduling",
    "sports-league-scheduling",
    "task-assigning",
    "tournament-scheduling",
)

#: Use cases in the homepage's planning table (every instance of each).
PLANNING = (
    "bed-allocation",
    "conference-scheduling",
    "facility-location",
    "flight-crew-scheduling",
    "food-packaging",
    "maintenance-scheduling",
    "meeting-scheduling",
    "order-picking",
    "project-job-scheduling",
    "sports-league-scheduling",
    "task-assigning",
    "tournament-scheduling",
)

SOLVERS = {"engine": "engine", "cp-sat": "ortools"}
MISSING = {
    "UNKNOWN": "no solution in time",
    "INFEASIBLE": "infeasible",
    "MODEL_INVALID": "no solution",
}


def _status(run: dict | None) -> str:
    status = (run or {}).get("status") or "no solution"
    return MISSING.get(status, status)


def _runs(result: dict) -> dict[str, dict]:
    return {run["solver"]: run for run in result.get("runs", [])}


def _best(result: dict) -> list[str]:
    scored = [run for run in result.get("runs", []) if run.get("score") is not None]
    if not scored:
        return []
    top = max(tuple(run["score"]) for run in scored)
    return [run["solver"] for run in scored if tuple(run["score"]) == top]


# ---------------------------------------------------------------------- README tables


def _replace_block(path: Path, body: str) -> None:
    text = path.read_text()
    start, end = (
        text.index("<!-- results:start -->"),
        text.index("<!-- results:end -->"),
    )
    path.write_text(
        text[:start] + "<!-- results:start -->\n" + body.strip() + "\n" + text[end:]
    )


def _readme_of(slug: str) -> Path:
    for path in (
        BENCHMARKS / slug / "README.md",
        BENCHMARKS / slug / "cpsat" / "README.md",
    ):
        if path.exists() and "<!-- results:start -->" in path.read_text():
            return path
    raise FileNotFoundError(f"{slug}: no README with a results block")


def compare_block(results: list[dict], footer: str) -> str:
    lines = [
        "## Results",
        "",
        (
            "Every cell is the score of that solver's final solution under the use case's own rules, "
            "re-scored by its evaluator. Higher is better; ties are all bold."
        ),
        "",
        "| Instance | Size | optimaru engine | OR-Tools CP-SAT |",
        "| --- | --- | --- | --- |",
    ]
    caveats = []
    for result in results:
        if "error" in result:
            lines.append(f"| {result['instance']} | — | failed | failed |")
            continue
        runs, best = _runs(result), _best(result)
        cells = []
        for solver in SOLVERS:
            run = runs.get(solver)
            if run is None or run.get("score") is None:
                cells.append(f"— ({_status(run)})")
            else:
                text = run["score_text"]
                cells.append(f"**{text}**" if solver in best else text)
        lines.append(
            f"| {result['instance']} | {result['size']} | {cells[0]} | {cells[1]} |"
        )
        if result.get("caveat") and result["caveat"] not in caveats:
            caveats.append(result["caveat"])
    lines += ["", footer]
    lines += [f"Note: {caveat}." for caveat in caveats]
    return "\n".join(lines)


def vrp_block(data: dict, footer: str) -> str:
    by_instance: dict[str, dict[str, dict]] = {}
    for row in data["results"]:
        by_instance.setdefault(row["instance"], {})[row["backend"]] = row
    lines = [
        "## Results",
        "",
        (
            "One median instance per CVRPLIB Set X size bucket. Gap to the best-known solution; every route "
            "is re-verified by the harness. Lower is better; the best value is bold."
        ),
        "",
        "| Instance | Customers | engine | ortools |",
        "|---|---:|---:|---:|",
    ]
    for instance, runs in by_instance.items():
        cells = {name: _vrp_cell(runs.get(name)) for name in ("engine", "ortools")}
        values = {
            name: value for name, (value, _) in cells.items() if value is not None
        }
        top = min(values.values()) if values else None
        texts = [
            f"**{text}**" if value is not None and value == top else text
            for value, text in cells.values()
        ]
        customers = next(iter(runs.values()))["customer_count"]
        lines.append(f"| {instance} | {customers} | {texts[0]} | {texts[1]} |")
    lines += ["", footer]
    return "\n".join(lines)


def _vrp_cell(row: dict | None) -> tuple[float | None, str]:
    if row is None:
        return None, "not run"
    if row["unassigned"]:
        return None, f"{row['unassigned']} unassigned"
    if not row["feasible"] or row["gap"] is None:
        return None, "infeasible"
    return row["gap"], f"{row['gap']:.2f}%"


# ---------------------------------------------------------------------- homepage


def _level_label(name: str) -> str:
    return re.sub(r"(\D)(\d+)$", r"\1 \2", name)


def _penalty(value: float) -> float:
    return round(-float(value), 2) + 0.0  # + 0.0 turns -0.0 into 0.0


def planning_row(slug: str, result: dict, labels: dict, sizes: dict) -> dict:
    runs = _runs(result)
    scored = {
        solver: run for solver, run in runs.items() if run.get("score") is not None
    }
    names = next(iter(scored.values()))["score_names"] if scored else ["soft"]
    # The deciding level: the first one the solvers differ on; on a full tie, the objective
    # (last) level; with one solution only, its first non-zero level.
    if len(scored) == 2:
        engine, ortools = (scored[name]["score"] for name in SOLVERS)
        level = next(
            (i for i, (a, b) in enumerate(zip(engine, ortools)) if a != b),
            len(names) - 1,
        )
    elif scored:
        score = next(iter(scored.values()))["score"]
        level = next((i for i, value in enumerate(score) if value != 0), len(names) - 1)
    else:
        level = len(names) - 1
    cells = {}
    for solver, key in SOLVERS.items():
        run = runs.get(solver)
        if run is None or run.get("score") is None:
            cells[key] = {"value": None, "status": "no solution"}
        else:
            cells[key] = {"value": _penalty(run["score"][level])}
    return {
        "slug": slug,
        "useCase": labels[slug],
        "instance": result["instance"],
        # The homepage's own (shorter) size text where it has one.
        "size": sizes.get((slug, result["instance"]), result["size"]),
        "level": _level_label(names[level]),
        "cells": cells,
        "best": [SOLVERS[name] for name in _best(result)],
    }


def routing_rows(data: dict) -> list[dict]:
    rows: dict[str, dict] = {}
    for row in data["results"]:
        entry = rows.setdefault(
            row["instance"],
            {"instance": row["instance"], "customers": row["customer_count"]},
        )
        value, _ = _vrp_cell(row)
        entry.setdefault("cells", {})[row["backend"]] = (
            {"value": round(value, 2)}
            if value is not None
            else {"value": None, "status": "incomplete"}
        )
    for entry in rows.values():
        values = {
            name: cell["value"]
            for name, cell in entry["cells"].items()
            if cell["value"] is not None
        }
        top = min(values.values()) if values else None
        entry["best"] = [name for name, value in values.items() if value == top]
    return list(rows.values())


def timetabling_rows(extra: list[dict]) -> list[dict]:
    rows = []
    for row in extra:
        cells = {
            name: {"value": row[name]}
            for name in ("engine", "ortools")
            if row.get(name) is not None
        }
        for name in ("engine", "ortools"):
            cells.setdefault(name, {"value": None, "status": "no solution"})
        values = {
            name: cell["value"]
            for name, cell in cells.items()
            if cell["value"] is not None
        }
        top = min(values.values()) if values else None
        rows.append(
            {
                "size": row["size"],
                "cells": cells,
                "best": [name for name, value in values.items() if value == top],
            }
        )
    return rows


def _current_planning(path: Path) -> list[dict]:
    text = path.read_text()
    start = text.index("= [", text.index("export const planning")) + 2
    return json.loads(
        text[start : text.index("export const routing")].rstrip().rstrip(";")
    )


def _header(path: Path) -> str:
    text = path.read_text()
    return text[: text.index("export const meta")]


def write_homepage(
    path: Path,
    meta: dict,
    planning: list,
    routing: list,
    timetabling: list,
    public: list,
) -> None:
    def const(name: str, kind: str, value) -> str:
        return f"export const {name}{kind} = {json.dumps(value, indent=2, ensure_ascii=False)};\n"

    path.write_text(
        _header(path)
        + const("meta", "", meta)
        + "\n"
        + const("planning", ": PlanningRow[]", planning)
        + "\n"
        + const("routing", ": RoutingRow[]", routing)
        + "\n"
        + const("timetabling", ": TimetablingRow[]", timetabling)
        + "\n"
        + const("publicRows", ": PublicRow[]", public)
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results", type=Path)
    parser.add_argument("--date", required=True)
    parser.add_argument(
        "--machine",
        required=True,
        help="English, e.g. 'one Apple M4 Max Mac (16 cores)'",
    )
    parser.add_argument("--machine-ko", required=True)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--ortools", default="9.15")
    parser.add_argument("--homepage", type=Path, default=None)
    args = parser.parse_args()

    compare = {
        path.stem: json.loads(path.read_text())
        for path in sorted((args.results / "compare").glob("*.json"))
    }
    footer = (
        f"Measured {args.date} on {args.machine}, {args.seconds:g} s per solver and instance, seed 1. "
        f"The engine runs single-threaded; CP-SAT uses {args.workers} workers and its time includes model "
        "construction. Single runs — treat small differences as noise."
    )
    for slug in STANDARD:
        _replace_block(_readme_of(slug), compare_block(compare[slug], footer))

    vrp = json.loads((args.results / "vrp.json").read_text())
    _replace_block(
        BENCHMARKS / "vehicle-routing" / "README.md",
        vrp_block(
            vrp,
            f"Measured {args.date} on {args.machine}, {vrp['protocol']['seconds']:g} s per run, seed "
            f"{vrp['protocol']['seed']}, single-threaded. Single runs — a snapshot, not a statistical study.",
        ),
    )

    if args.homepage is not None:
        target = args.homepage / "src" / "data" / "benchmarks.ts"
        current = _current_planning(target)
        labels = {row["slug"]: row["useCase"] for row in current}
        sizes = {(row["slug"], row["instance"]): row["size"] for row in current}
        extra = json.loads((args.results / "extra.json").read_text())
        planning = [
            planning_row(slug, result, labels, sizes)
            for slug in PLANNING
            for result in compare[slug]
            if "error" not in result
        ]
        meta = {
            "date": args.date,
            "seconds": int(args.seconds)
            if float(args.seconds).is_integer()
            else args.seconds,
            "ortools": args.ortools,
            "machine": {"en": args.machine, "ko": args.machine_ko},
        }
        write_homepage(
            target,
            meta,
            planning,
            routing_rows(vrp),
            timetabling_rows(extra["timetabling"]),
            extra["public"],
        )


if __name__ == "__main__":
    main()
