"""Write benchmark results into the README tables and the homepage data file.

Usage::

    python scripts/publish_benchmarks.py RESULTS_DIR --date 2026-11-10 \\
        --machine "one Apple M4 Max Mac (16 cores)" --machine-ko "<the same in Korean>" \\
        [--homepage ../homepage]

RESULTS_DIR holds what the harnesses wrote. Every file is optional: a data set
whose file is missing keeps its README text and its homepage entry.

- ``compare/<slug>.json``: ``python -m <package>.benchmark --json`` of a use case;
- ``vrp-x.json``, ``vrp-a.json``: ``python -m vrp_benchmark --series x|a --out FILE``;
- ``pdp-li-lim.json``, ``pdp-synthetic.json``: ``pickup-delivery-public-bench --json``
  (the lists of several runs concatenated);
- ``rcpsp.json``: ``project-job-public-bench --all --json``;
- ``bpp.json``: bin-packing ``python -m bench.run_bench --json``;
- ``qaplib.json``: quadratic-assignment ``python -m bench.run_bench --all --json``;
- ``salbp-<data set>.json``: assembly-line-balancing
  ``python -m bench.run_bench --dataset <data set> --json``;
- ``extra.json``: the numbers the school-timetabling harness prints as text, copied by
  hand: ``{"timetabling": {"date": "2026-11-04", "seconds": 30, "rows": [{"instance": "S",
  "size": "6 classes / 15 teachers / 72 courses", "engine": 231, "ortools": 320}, ...]}}``
  (track 2 weighted penalties).

``--seconds`` and ``--workers`` are what the harnesses were run with: the tables state
them, and a result that records another budget or worker count is refused. Which instances
a table holds, and up to which size CP-SAT was run, is read from the results.

Every README results block between ``<!-- results:start -->`` and ``<!-- results:end -->``
of a data set with results is regenerated. The homepage file (``src/data/benchmarks.ts``)
holds one entry per data set; its prose lives in the homepage repository.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCHMARKS = ROOT / "benchmarks"

#: Use cases compared on generated or demo instances by the shared runner
#: (``cpsat_plugins.benchmark``), in the order of the homepage.
COMPARED = (
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
    "project-job-scheduling",
    "quadratic-assignment",
    "semiconductor-scheduling",
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
VRP_BUCKETS = (
    ("100-200", 100, 200),
    ("200-400", 200, 400),
    ("400-600", 400, 600),
    ("600-800", 600, 800),
    ("800-1000", 800, 1_001),
)
SALBP_SETS = (("small", 20), ("medium", 50), ("large", 100), ("very-large", 1000))


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


def _lowest(cells: dict[str, dict]) -> list[str]:
    """The solvers with the lowest value, when both were run."""
    if any(cell.get("status") == "not run" for cell in cells.values()) or len(cells) < 2:
        return []
    values = {name: cell["value"] for name, cell in cells.items() if cell["value"] is not None}
    if not values:
        return []
    top = min(values.values())
    return [name for name, value in values.items() if value == top]


def _number(value: float) -> str:
    """1234.5 -> '1,234.5'; whole numbers lose the decimals."""
    return f"{value:,.2f}".rstrip("0").rstrip(".")


def _signed(percent: float) -> str:
    """+5.1% / -0.3%; a difference that rounds to nothing has no sign."""
    text = f"{percent:+.1f}%"
    return "0.0%" if text in ("+0.0%", "-0.0%") else text


def _listed(items: list[str]) -> str:
    """'a', 'a and b', 'a, b and c'."""
    items = list(items)
    return items[0] if len(items) == 1 else f"{', '.join(items[:-1])} and {items[-1]}"


def _shared_numbers(names: list[str]) -> list[str] | None:
    """The instance numbers every class of a sample has in common (`u120_00`, `t60_00`, ... ->
    ['00', ...]); None when the classes were sampled differently."""
    by_class: dict[str, list[str]] = {}
    for name in names:
        stem, _, number = name.rpartition("_")
        by_class.setdefault(stem, []).append(number)
    picks = list(by_class.values())
    return picks[0] if picks and all(pick == picks[0] for pick in picks) else None


def _exact_cap(results: list[dict], size: str) -> int | None:
    """The size up to which CP-SAT was run, when larger instances were left to the engine."""
    stated = {result["exact_max_size"] for result in results if result.get("exact_max_size")}
    if len(stated) == 1:
        return stated.pop()
    ran = [result[size] for result in results if "cp-sat" in _runs(result)]
    skipped = [result[size] for result in results if "cp-sat" not in _runs(result)]
    return max(ran) if ran and skipped else None


def _check_protocol(what: str, results: list[dict], args: argparse.Namespace) -> None:
    """Refuse results that were run with another budget or worker count than the tables will state.

    A harness records `budget` (and `workers`) in each result; results written before they did
    carry neither and pass.
    """
    for result in results:
        budget, workers = result.get("budget"), result.get("workers")
        if budget is not None and budget != args.seconds:
            raise SystemExit(f"{what}: {result['instance']} was run with {budget:g} s, not --seconds {args.seconds:g}")
        if workers is not None and workers != args.workers and "cp-sat" in _runs(result):
            raise SystemExit(
                f"{what}: {result['instance']} gave CP-SAT {workers} workers, not --workers {args.workers}"
            )


def _bold(cells: dict[str, dict], texts: dict[str, str]) -> dict[str, str]:
    best = _lowest(cells)
    return {name: f"**{text}**" if name in best else text for name, text in texts.items()}


def _seed(results: list[dict], default: int) -> int:
    """The seed the results record. `default` is the harness's own default, for results written
    before a harness recorded its seed; results of different seeds do not share a table."""
    seeds = {result["seed"] for result in results if "seed" in result}
    if len(seeds) > 1:
        raise SystemExit(f"results of different seeds cannot share a table: {sorted(seeds)}")
    return seeds.pop() if seeds else default


def _check_vrp(what: str, data: dict, args: argparse.Namespace) -> None:
    """Refuse a routing file run with another budget than the homepage entry will state."""
    seconds = data["protocol"]["seconds"]
    if seconds != args.seconds:
        raise SystemExit(f"{what}: run with {seconds:g} s, not --seconds {args.seconds:g}")


def _footer(args: argparse.Namespace, seed: int, *, workers: bool = True) -> str:
    text = f"Measured {args.date} on {args.machine}, {args.seconds:g} s per solver and instance, seed {seed}. "
    if workers:
        text += (
            f"The engine runs single-threaded; CP-SAT uses {args.workers} workers and its budget starts once "
            "its model is built. "
        )
    return text + "Single runs — a snapshot, not a statistical study."


# ---------------------------------------------------------------------- README blocks


def _replace_block(path: Path, body: str, tag: str = "results") -> None:
    text = path.read_text()
    opening, closing = f"<!-- {tag}:start -->", f"<!-- {tag}:end -->"
    start, end = text.index(opening), text.index(closing)
    path.write_text(text[:start] + opening + "\n" + body.strip() + "\n" + text[end:])


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
        lines.append(f"| {result['instance']} | {result['size']} | {cells[0]} | {cells[1]} |")
        if result.get("caveat") and result["caveat"] not in caveats:
            caveats.append(result["caveat"])
    lines += ["", footer]
    lines += [f"Note: {caveat}." for caveat in caveats]
    return "\n".join(lines)


def _vrp_cell(row: dict | None) -> tuple[float | None, str]:
    if row is None:
        return None, "not run"
    if row["unassigned"]:
        return None, f"{row['unassigned']} unassigned"
    if not row["feasible"] or row["gap"] is None:
        return None, "infeasible"
    return row["gap"], f"{row['gap']:.2f}%"


def _vrp_instances(data: dict) -> dict[str, dict[str, dict]]:
    by_instance: dict[str, dict[str, dict]] = {}
    for row in data["results"]:
        by_instance.setdefault(row["instance"], {})[row["backend"]] = row
    return by_instance


def _vrp_bucket(customers: int) -> str:
    for label, lower, upper in VRP_BUCKETS:
        if lower <= customers < upper:
            return label
    raise ValueError(f"{customers} customers is outside the Set X size buckets (100 to 1000)")


def _vrp_means(runs_by_instance: dict[str, dict[str, dict]], label: str | None) -> dict[str, tuple]:
    """Per backend: (mean gap over complete runs, complete runs, runs), for one bucket or all."""
    means = {}
    for backend in ("engine", "ortools"):
        rows = [
            runs[backend]
            for runs in runs_by_instance.values()
            if backend in runs and (label is None or _vrp_bucket(runs[backend]["customer_count"]) == label)
        ]
        gaps = [value for value, _ in map(_vrp_cell, rows) if value is not None]
        means[backend] = (statistics.fmean(gaps) if gaps else None, len(gaps), len(rows))
    return means


def _vrp_table(by_instance: dict[str, dict[str, dict]]) -> list[str]:
    lines = [
        "| Instance | Customers | Best known | engine | ortools |",
        "|---|---:|---:|---:|---:|",
    ]
    for instance, runs in by_instance.items():
        cells = {name: _vrp_cell(runs.get(name)) for name in ("engine", "ortools")}
        values = {name: value for name, (value, _) in cells.items() if value is not None}
        top = min(values.values()) if values else None
        texts = [f"**{text}**" if value is not None and value == top else text for value, text in cells.values()]
        first = next(iter(runs.values()))
        lines.append(
            f"| {instance} | {first['customer_count']} | {first['best_known_distance']:,} | {texts[0]} | {texts[1]} |"
        )
    return lines


def _mean_text(mean: tuple) -> str:
    value, solved, runs = mean
    return f"{'-' if value is None else f'{value:.2f}%'} ({solved}/{runs})"


def vrp_block(set_x: dict | None, set_a: dict | None, args: argparse.Namespace) -> str:
    lines = [
        "## Results",
        "",
        (
            "Gap to the best-known solution; every route is re-verified by the harness. Lower is better; "
            "the best value is bold. A mean gap counts the complete runs only (complete runs / runs)."
        ),
    ]
    for title, data in (("CVRPLIB Set X", set_x), ("CVRPLIB Set A (Augerat)", set_a)):
        if data is None:
            continue
        by_instance = _vrp_instances(data)
        lines += ["", f"### {title}", ""]
        if data["protocol"]["series"] == "x":
            lines += ["| Customers | Instances | engine | ortools |", "|---|---:|---:|---:|"]
            for label, _, _ in VRP_BUCKETS:
                means = _vrp_means(by_instance, label)
                if means["engine"][2] or means["ortools"][2]:
                    count = max(means["engine"][2], means["ortools"][2])
                    lines.append(
                        f"| {label} | {count} | {_mean_text(means['engine'])} | {_mean_text(means['ortools'])} |"
                    )
        means = _vrp_means(by_instance, None)
        if data["protocol"]["series"] == "x":
            lines.append(
                f"| all | {len(by_instance)} | {_mean_text(means['engine'])} | {_mean_text(means['ortools'])} |"
            )
        else:
            lines.append(
                f"Mean gap over the {len(by_instance)} instances: engine {_mean_text(means['engine'])}, "
                f"ortools {_mean_text(means['ortools'])}."
            )
        lines += ["", *_vrp_table(by_instance)]
    protocol = (set_x or set_a)["protocol"]
    lines += [
        "",
        f"Measured {args.date} on {args.machine}, {protocol['seconds']:g} s per run, seed "
        f"{protocol['seed']}, single-threaded. Single runs — a snapshot, not a statistical study.",
    ]
    return "\n".join(lines)


def _pdp_cell(run: dict | None) -> tuple[dict, str]:
    """Homepage cell and README text of one pickup-delivery run."""
    if run is None:
        return {"value": None, "status": "not run"}, "not run"
    outcome = run.get("outcome")
    if outcome == "mismatch":
        raise SystemExit(f"pickup-delivery: a run cannot be published: {run['status']}")
    if run["distance"] is None:
        status = run["status"]
        if outcome is None:
            # Results written before a run carried its outcome: read it from the words.
            if status.startswith("no solution"):
                outcome = "no solution"
            else:
                outcome = "incomplete" if "do not cover" in status else "infeasible"
        if outcome == "no solution":
            return {"value": None, "status": "no solution"}, "no solution in time"
        note = "leaves stops unserved" if outcome == "incomplete" else status.removeprefix("rejected: ")
        return {"value": None, "status": outcome}, f"{outcome} ({note})"
    distance = round(run["distance"], 1)
    return (
        {"value": distance, "detail": f"{run['vehicles']} vehicles"},
        f"{run['vehicles']} / {_number(distance)}",
    )


def _pdp_cells(case: dict) -> tuple[dict[str, dict], dict[str, str]]:
    runs = _runs(case)
    pairs = {key: _pdp_cell(runs[solver]) for solver, key in SOLVERS.items() if solver in runs}
    return {key: cell for key, (cell, _) in pairs.items()}, {key: text for key, (_, text) in pairs.items()}


def _pdp_size(case: dict) -> str:
    if case["requests"] == case["total_requests"]:
        return f"all {case['requests']} requests"
    return f"first {case['requests']} of {case['total_requests']} requests"


def _pdp_split(cases: list[dict]) -> tuple[list[dict], list[dict]]:
    """Li & Lim cases -> (every full instance, the cases of instances CP-SAT was run on)."""
    full = sorted(
        (case for case in cases if case["requests"] == case["total_requests"]),
        key=lambda case: case["instance"],
    )
    head = sorted(
        (case for case in cases if "cp-sat" in _runs(case)),
        key=lambda case: (case["instance"], case["requests"]),
    )
    return full, head


def pdp_li_lim_block(cases: list[dict], args: argparse.Namespace) -> str:
    full, head = _pdp_split(cases)
    lines = [
        "Each plan is re-verified independently: every request served, pickup before dropoff on the",
        "same vehicle, capacity and every time window. Distance is the Li & Lim round trip, with every",
        "leg rounded to a tenth of a unit; lower is better. Cells are vehicles / distance.",
    ]
    if head:
        names = list(dict.fromkeys(case["instance"] for case in head))
        partial = sorted({case["requests"] for case in head if case["requests"] < case["total_requests"]})
        sizes = [f"their first {_listed([str(count) for count in partial])} requests"] if partial else []
        if any(case["requests"] == case["total_requests"] for case in head):
            sizes.append("whole")
        lines += [
            "",
            f"**Engine and CP-SAT** on {_listed(names)}, at {' and '.join(sizes)}:",
            "",
            "| Instance | Requests | engine | CP-SAT |",
            "| --- | --- | --- | --- |",
        ]
        for case in head:
            cells, texts = _pdp_cells(case)
            texts = _bold(cells, texts)
            lines.append(f"| {case['instance']} | {_pdp_size(case)} | {texts['engine']} | {texts['ortools']} |")
    if full:
        lines += [
            "",
            f"**Engine on {len(full)} whole instances**, beside the best-known solution (which ranks by",
            "fewest vehicles first, then distance, on unrounded distances — a reference, not this",
            "harness's objective):",
            "",
            "| Instance | Requests | Best known | engine | Distance vs best known |",
            "| --- | ---: | --- | --- | ---: |",
        ]
        for case in full:
            cell, text = _pdp_cell(_runs(case).get("engine"))
            best = case.get("best_known")
            reference = "—" if best is None else f"{best['vehicles']} / {_number(best['distance'])}"
            delta = (
                "—"
                if best is None or cell["value"] is None
                else _signed(100 * (cell["value"] - best["distance"]) / best["distance"])
            )
            lines.append(f"| {case['instance']} | {case['requests']} | {reference} | {text} | {delta} |")
    return "\n".join(lines)


def pdp_synthetic_block(cases: list[dict]) -> str:
    # A synthetic instance is named synthetic-<requests>-s<seed>.
    seeds = sorted({case["instance"].rsplit("-s", 1)[-1] for case in cases})
    lines = [
        f"**Synthetic Li & Lim-shaped instances** (`--synthetic N`, seed {_listed(seeds)}): customers in a",
        "100 x 100 square, half in six clusters, service time 90, one vehicle per three requests. Cells",
        "are vehicles / distance.",
        "",
        "| Requests | engine | CP-SAT |",
        "| --- | --- | --- |",
    ]
    engine_only = []
    for case in cases:
        cells, texts = _pdp_cells(case)
        texts = _bold(cells, texts)
        lines.append(f"| {case['requests']} | {texts['engine']} | {texts.get('ortools', 'not run')} |")
        if "ortools" not in cells:
            engine_only.append(str(case["requests"]))
    if engine_only:
        measured = " (at 400 requests it was still being built after 10 minutes at 11 GB, 2026-10-29)"
        lines += [
            "",
            f"CP-SAT is not run at {_listed(engine_only)} requests: its model takes minutes and gigabytes to"
            f" build{measured if '400' in engine_only else ''}.",
        ]
    return "\n".join(lines)


def _count_cells(result: dict, field: str) -> tuple[dict[str, dict], dict[str, str]]:
    """Cells of a bin-count / station-count / makespan result; a missing CP-SAT run is 'not run'."""
    runs = _runs(result)
    cells, texts = {}, {}
    for solver, key in SOLVERS.items():
        run = runs.get(solver)
        if run is not None and run.get("outcome") == "mismatch":
            raise SystemExit(f"{result['instance']}: a run cannot be published: {run['status']}")
        if run is None:
            cells[key], texts[key] = {"value": None, "status": "not run"}, "not run (size cap)"
        elif run.get(field) is None or run.get("feasible") is False:
            kind = "no solution" if run.get(field) is None else "infeasible"
            cells[key], texts[key] = {"value": None, "status": kind}, f"{kind} in time"
        else:
            cells[key], texts[key] = {"value": run[field]}, _number(run[field])
    return cells, texts


def rcpsp_block(results: list[dict], args: argparse.Namespace) -> str:
    lines = [
        "Makespan of each solver's schedule, re-verified independently (durations, precedences and",
        "cumulative resource use at every tick). Lower is better; the best value is bold.",
        "",
        "| Instance | Activities | Critical path | engine | CP-SAT |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        cells, texts = _count_cells(result, "makespan")
        texts = _bold(cells, texts)
        lines.append(
            f"| {result['instance']} | {result['tasks'] - 2} | {result['critical_path']} | "
            f"{texts['engine']} | {texts['ortools']} |"
        )
    lines += ["", _footer(args, _seed(results, 0))]
    return "\n".join(lines)


def bpp_block(results: list[dict], args: argparse.Namespace) -> str:
    numbers = _shared_numbers([result["instance"] for result in results])
    sample = f"instances {_listed(numbers)} of each class" if numbers else f"{len(results)} instances"
    cap = _exact_cap(results, "items")
    lines = [
        "## Results",
        "",
        f"Falkenauer: {sample}. Bins used; lower is better and the best value is bold.",
        "",
        "| Instance | Items | Best known | optimaru engine | CP-SAT |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        cells, texts = _count_cells(result, "bins")
        texts = _bold(cells, texts)
        lines.append(
            f"| {result['instance']} | {result['items']} | {result['best_known']} | "
            f"{texts['engine']} | {texts['ortools']} |"
        )
    lines += [
        "",
        (f"CP-SAT runs only up to the exact-model size cap (`--exact-max-size`, {cap} items). " if cap else "")
        + _footer(args, _seed(results, 1)),
    ]
    return "\n".join(lines)


def _qap_gap(result: dict) -> tuple[int | None, float | None]:
    run = _runs(result).get("engine") or {}
    return run.get("objective"), run.get("gap_to_qaplib_percent")


def qaplib_block(results: list[dict], args: argparse.Namespace) -> str:
    gaps = [gap for _, gap in map(_qap_gap, results) if gap is not None]
    missing = [result["instance"] for result in results if result["qaplib_best_known"] is None]
    lines = [
        f"The {len(results)} QAPLIB instances with 30 to 50 facilities. Lower is better; the gap is to the",
        "QAPLIB best-known value. CP-SAT runs only up to the exact-model size cap (`--exact-max-size`);",
        "these instances are above it.",
    ]
    if missing:
        lines += [
            "",
            f"The archive has no solution file for {', '.join(missing)}, so they have no reference and no gap.",
        ]
    lines += [
        "",
        "| Instance | Size | QAPLIB best known | optimaru engine | Gap |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        objective, gap = _qap_gap(result)
        best = result["qaplib_best_known"]
        lines.append(
            f"| {result['instance']} | {result['size']} | {'—' if best is None else f'{best:,}'} | "
            f"{'—' if objective is None else f'{objective:,}'} | {'—' if gap is None else f'{gap:.2f}%'} |"
        )
    summary = (
        f"Mean gap {statistics.fmean(gaps):.2f}%, median {statistics.median(gaps):.2f}%, worst {max(gaps):.2f}%. "
        if gaps
        else ""
    )
    lines += ["", summary + _footer(args, _seed(results, 1), workers=False)]
    return "\n".join(lines)


def salbp_block(sets: dict[str, list[dict]], args: argparse.Namespace) -> str:
    everything = [result for name, _ in SALBP_SETS for result in sets.get(name, [])]
    numbers = _shared_numbers([result["instance"] for result in everything])
    sample = f"instances {_listed(numbers)} of each data set" if numbers else f"{len(everything)} instances"
    cap = _exact_cap(everything, "tasks")
    lines = [
        "## Results",
        "",
        f"SALBP-1: {sample}. Stations used; lower is better and the best value is bold. The upper bound",
        "is the one in the archive's details workbook: the optimum where the workbook's lower bound meets",
        "it, otherwise the best its authors' solver found in 100 s.",
        "",
        "| Data set | Instance | Tasks | Archive upper bound | optimaru engine | CP-SAT |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for name, _ in SALBP_SETS:
        for result in sets.get(name, []):
            cells, texts = _count_cells(result, "stations")
            texts = _bold(cells, texts)
            lines.append(
                f"| {name} | {result['instance']} | {result['tasks']} | {result['best_known']} | "
                f"{texts['engine']} | {texts['ortools']} |"
            )
    lines += [
        "",
        (f"CP-SAT runs only up to the exact-model size cap (`--exact-max-size`, {cap} tasks). " if cap else "")
        + _footer(args, _seed(everything, 1)),
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------- homepage


def _level_label(name: str) -> str:
    return re.sub(r"(\D)(\d+)$", r"\1 \2", name)


def _penalty(value: float) -> float:
    return round(-float(value), 2) + 0.0  # + 0.0 turns -0.0 into 0.0


def planning_row(result: dict) -> dict:
    runs = _runs(result)
    scored = {solver: run for solver, run in runs.items() if run.get("score") is not None}
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
            cells[key] = {"value": _penalty(run["score"][level]), "detail": run["score_text"]}
    return {
        "instance": result["instance"],
        "size": result["size"],
        "level": _level_label(names[level]),
        "cells": cells,
        "best": [SOLVERS[name] for name in _best(result)],
    }


def _dataset(slug: str, use_case: str, kind: str, opponent: str, code: str, command: str, sections: list) -> dict:
    return {
        "slug": slug,
        "useCase": use_case,
        "kind": kind,
        "opponent": opponent,
        "code": code,
        "command": command,
        "sections": sections,
    }


def _section(section_id: str, metric: str, rows: list[dict], *, solvers=("engine", "ortools"), **extra) -> dict:
    return {"id": section_id, "metric": metric, "solvers": list(solvers), **extra, "rows": rows}


def _as_rewards(rows: list[dict]) -> bool:
    """Whether a use case scores a reward (a knapsack's value, a cut's weight): then the
    deciding level of every row is positive, and the table shows scores, higher being better."""
    values = [cell["value"] for row in rows for cell in row["cells"].values() if cell["value"] is not None]
    if not values or max(values) > 0 or min(values) == 0:
        return False
    for row in rows:
        for cell in row["cells"].values():
            if cell["value"] is not None:
                cell["value"] = -cell["value"]
    return True


def compare_dataset(slug: str, results: list[dict], seconds: float) -> dict:
    rows = [planning_row(result) for result in results if "error" not in result]
    metric = "reward" if _as_rewards(rows) else "penalty"
    caveats = list(dict.fromkeys(result["caveat"] for result in results if result.get("caveat")))
    dataset = _dataset(
        slug,
        slug,
        "generated",
        "cp-sat",
        f"{slug}/cpsat",
        f"uv run {COMPARE_COMMANDS.get(slug, slug + '-compare')} --seconds {seconds:g} --workers 8",
        [_section("instances", metric, rows)],
    )
    if caveats:
        dataset["caveats"] = caveats
    return dataset


#: The console script of each comparison project (its ``[project.scripts]``).
COMPARE_COMMANDS = {
    "flight-crew-scheduling": "flight-crew-compare",
    "project-job-scheduling": "project-job-compare",
    "sports-league-scheduling": "sports-league-compare",
}


def vrp_dataset(slug: str, data: dict) -> dict:
    by_instance = _vrp_instances(data)
    series = data["protocol"]["series"]
    rows = []
    for instance, runs in by_instance.items():
        first = next(iter(runs.values()))
        cells = {}
        for backend in ("engine", "ortools"):
            row = runs.get(backend)
            value, text = _vrp_cell(row)
            if row is None:
                cells[backend] = {"value": None, "status": "not run"}
            elif value is None:
                cells[backend] = {
                    "value": None,
                    "status": "incomplete" if row["unassigned"] else "infeasible",
                    "detail": text,
                }
            else:
                cells[backend] = {"value": round(value, 2), "detail": f"{row['distance']:,}"}
        rows.append(
            {
                "instance": instance,
                "size": f"{first['customer_count']} customers",
                "reference": f"{first['best_known_distance']:,}",
                "cells": cells,
                "best": _lowest(cells),
            }
        )
    section = _section("instances", "gap", rows, unit="%")
    if series == "x":
        chart = []
        for label, _, _ in VRP_BUCKETS:
            means = _vrp_means(by_instance, label)
            if not (means["engine"][2] or means["ortools"][2]):
                continue
            cells = {
                backend: (
                    {"value": None, "status": "incomplete"}
                    if mean is None
                    else {"value": round(mean, 2), "detail": f"{solved} of {runs} complete"}
                )
                for backend, (mean, solved, runs) in means.items()
            }
            chart.append(
                {
                    "instance": f"{label.replace('-', '–')} customers",
                    "size": f"{max(m[2] for m in means.values())} instances",
                    "cells": cells,
                    "best": _lowest(cells),
                }
            )
        section["chart"] = chart
    command = "uv run python -m vrp_benchmark --series {} --seconds {:g}".format(series, data["protocol"]["seconds"])
    if series == "x":
        command += " --catalog-source mirror"
    # A run of chosen instances reproduces with the same choice, not as the whole series.
    command += "".join(f" --only {name}" for name in data["protocol"].get("only") or [])
    return _dataset(slug, "vehicle-routing", "public", "routing", "vehicle-routing", command, [section])


def pdp_row(case: dict, *, size: str, reference: bool) -> dict:
    cells, _ = _pdp_cells(case)
    row = {"instance": case["instance"], "size": size, "cells": cells, "best": _lowest(cells)}
    best = case.get("best_known")
    if reference and best is not None:
        row["reference"] = f"{best['vehicles']} vehicles · {_number(best['distance'])}"
        row["ref"] = best["distance"]
    return row


def pdp_li_lim_dataset(cases: list[dict], seconds: float) -> dict:
    full, head = _pdp_split(cases)
    commands = []
    if full:
        commands.append(f"uv run pickup-delivery-public-bench --all --seconds {seconds:g} --backend engine")
    if head:
        # The head-to-head command of one instance, at the sizes the table has it.
        first = head[0]["instance"]
        sizes = [
            0 if case["requests"] == case["total_requests"] else case["requests"]
            for case in head
            if case["instance"] == first
        ]
        commands.append(
            f"uv run pickup-delivery-public-bench --instance {first} "
            + " ".join(f"--requests {size}" for size in sizes)
            + f" --seconds {seconds:g} --workers 8"
        )
    sections = []
    if full:
        rows = []
        for case in full:
            row = pdp_row(case, size=f"{case['requests']} requests", reference=True)
            row["cells"] = {"engine": row["cells"]["engine"]}
            row["best"] = []
            rows.append(row)
        sections.append(_section("all", "distance", rows, solvers=("engine",)))
    if head:
        sections.append(
            _section(
                "head-to-head", "distance", [pdp_row(case, size=_pdp_size(case), reference=False) for case in head]
            )
        )
    return _dataset(
        "li-lim-pdptw",
        "pickup-delivery",
        "public",
        "cp-sat",
        "pickup-delivery/cpsat",
        "\n".join(commands),
        sections,
    )


def pdp_synthetic_dataset(cases: list[dict], seconds: float) -> dict:
    rows = []
    for case in cases:
        row = pdp_row(case, size=f"{case['requests']} requests / {case['vehicles']} vehicles", reference=False)
        row["instance"] = f"{case['requests']} requests"
        row["cells"].setdefault("ortools", {"value": None, "status": "not run"})
        row["best"] = _lowest(row["cells"])
        rows.append(row)
    sizes = " ".join(f"--synthetic {case['requests']}" for case in cases)
    return _dataset(
        "pickup-delivery-synthetic",
        "pickup-delivery",
        "generated",
        "cp-sat",
        "pickup-delivery/cpsat",
        f"uv run pickup-delivery-public-bench {sizes} --seconds {seconds:g} --workers 8",
        [_section("instances", "distance", rows)],
    )


def _count_rows(results: list[dict], field: str, size) -> list[dict]:
    rows = []
    for result in results:
        cells, _ = _count_cells(result, field)
        row = {"instance": result["instance"], "size": size(result), "cells": cells, "best": _lowest(cells)}
        if result.get("best_known") is not None:
            row["reference"] = _number(result["best_known"])
            row["ref"] = result["best_known"]
        rows.append(row)
    return rows


def rcpsp_dataset(results: list[dict], seconds: float) -> dict:
    rows = _count_rows(
        results, "makespan", lambda result: f"{result['tasks'] - 2} activities / {result['resources']} resources"
    )
    return _dataset(
        "rg30-rcpsp",
        "project-job-scheduling",
        "public",
        "cp-sat",
        "project-job-scheduling/cpsat",
        f"uv run project-job-public-bench --all --seconds {seconds:g} --workers 8",
        [_section("instances", "makespan", rows)],
    )


def bpp_dataset(results: list[dict], seconds: float) -> dict:
    rows = _count_rows(results, "bins", lambda result: f"{result['items']} items")
    # `--sample-per-class N` picks N evenly spread instances of the twenty a Falkenauer class has.
    # The same rounding as `sample_per_class` in benchmarks/bin-packing/bench/run_bench.py: if the two
    # ever differ, the command below falls back to naming every instance, which is still correct.
    numbers = _shared_numbers([result["instance"] for result in results]) or []
    count = len(numbers)
    spread = [f"{(i * 19 + (count - 1) // 2) // (count - 1):02d}" for i in range(count)] if count > 1 else None
    selection = (
        f"--sample-per-class {count}"
        if numbers == spread
        else " ".join(f"--instance {result['instance']}" for result in results)
    )
    return _dataset(
        "falkenauer-bin-packing",
        "bin-packing",
        "public",
        "cp-sat",
        "bin-packing",
        f"uv run --project cpsat python -m bench.run_bench {selection} --seconds {seconds:g}",
        [_section("instances", "bins", rows)],
    )


def qaplib_dataset(results: list[dict], seconds: float) -> dict:
    rows = []
    for result in results:
        objective, gap = _qap_gap(result)
        if gap is None:
            continue  # no reference solution in the archive: nothing to measure a gap to
        rows.append(
            {
                "instance": result["instance"],
                "size": f"{result['size']} facilities",
                "reference": f"{result['qaplib_best_known']:,}",
                "cells": {"engine": {"value": round(gap, 2), "detail": f"{objective:,}"}},
                "best": [],
            }
        )
    return _dataset(
        "qaplib",
        "quadratic-assignment",
        "public",
        "cp-sat",
        "quadratic-assignment",
        f"uv run --project cpsat python -m bench.run_bench --all --seconds {seconds:g}",
        [_section("instances", "gap", rows, solvers=("engine",), unit="%")],
    )


def salbp_dataset(sets: dict[str, list[dict]], seconds: float) -> dict:
    rows = []
    for name, _ in SALBP_SETS:
        rows += _count_rows(sets.get(name, []), "stations", lambda result: f"{result['tasks']} tasks")
    # The command of one data set, with the instances the table has from it.
    name, results = next((name, sets[name]) for name in ("large", *sets) if name in sets)
    picked = " ".join(f"--instance {result['instance']}" for result in results)
    return _dataset(
        "salbp-1",
        "assembly-line-balancing",
        "public",
        "cp-sat",
        "assembly-line-balancing",
        f"uv run --project cpsat python -m bench.run_bench --dataset {name} {picked} --seconds {seconds:g}",
        [_section("instances", "stations", rows)],
    )


def timetabling_dataset(extra: dict) -> dict:
    rows = []
    for row in extra["rows"]:
        cells = {
            name: {"value": row[name]} if row.get(name) is not None else {"value": None, "status": "no solution"}
            for name in ("engine", "ortools")
        }
        rows.append({"instance": row["instance"], "size": row["size"], "cells": cells, "best": _lowest(cells)})
    dataset = _dataset(
        "school-timetabling",
        "school-timetabling",
        "generated",
        "cp-sat",
        "school-timetabling",
        f"uv run run_bench.py --track2 --sizes S,M,L --seconds {extra['seconds']:g}",
        [_section("instances", "penalty", rows, chart=rows)],
    )
    dataset["date"] = extra["date"]
    dataset["seconds"] = extra["seconds"]
    return dataset


#: Order of the data sets on the homepage: public instance sets first.
ORDER = (
    "cvrplib-x",
    "cvrplib-a",
    "li-lim-pdptw",
    "rg30-rcpsp",
    "falkenauer-bin-packing",
    "qaplib",
    "salbp-1",
    "school-timetabling",
    "pickup-delivery-synthetic",
    *COMPARED,
)


def _current_datasets(path: Path) -> dict[str, dict]:
    text = path.read_text()
    marker = "export const datasets"
    if marker not in text:
        return {}
    # Reads to the end of the file: write_homepage() writes `datasets` last.
    start = text.index("= [", text.index(marker)) + 2
    return {entry["slug"]: entry for entry in json.loads(text[start:].rstrip().rstrip(";"))}


HEADER = """// GENERATED by optimaru scripts/publish_benchmarks.py — do not edit by hand.
// The prose of each data set (names, descriptions, notes) is in benchmarkText.ts.

export type Solver = 'engine' | 'ortools';

export interface Cell {
  /** The number the row is decided on; null = no usable result. Lower is better, except for a 'reward'. */
  value: number | null;
  /** Why there is no value: 'no solution', 'incomplete', 'infeasible' or 'not run'. */
  status?: string;
  /** A second line under the value: the full score, the distance, the vehicles used. */
  detail?: string;
}

export interface Row {
  instance: string;
  /** Instance size as the harness describes it, e.g. '152 customers'. */
  size?: string;
  /** Best-known or optimal value, as shown beside the solvers. */
  reference?: string;
  /** The same reference as a number, when the cells are in its unit. */
  ref?: number;
  /** Score level the row is decided on ('hard', 'medium', 'soft'), for scored use cases. */
  level?: string;
  cells: Partial<Record<Solver, Cell>>;
  /** The solver(s) with the best value; empty when the solvers were not both run. */
  best: Solver[];
}

/** 'reward' is a score to maximise (a knapsack's value, a cut's weight); for every other metric lower is better. */
export type Metric = 'gap' | 'distance' | 'makespan' | 'bins' | 'stations' | 'penalty' | 'reward';

export interface Section {
  id: string;
  metric: Metric;
  /** Solvers with a column in this table. */
  solvers: Solver[];
  /** '%' for gaps. */
  unit?: string;
  /** Rows of the summary chart above the table, when there is one. */
  chart?: Row[];
  rows: Row[];
}

export interface Dataset {
  slug: string;
  /** Use-case slug, as in the docs and the solutions pages. */
  useCase: string;
  kind: 'public' | 'generated';
  /** The OR-Tools solver the engine is compared with. */
  opponent: 'routing' | 'cp-sat';
  /** Path of the harness under benchmarks/ in the engine repository. */
  code: string;
  /** A command of that harness that produces these tables. */
  command: string;
  /** What the harness says about how the two models differ, if anything. */
  caveats?: string[];
  date: string;
  seconds: number;
  sections: Section[];
}

"""


def write_homepage(path: Path, meta: dict, datasets: list[dict]) -> None:
    def const(name: str, kind: str, value) -> str:
        return f"export const {name}{kind} = {json.dumps(value, indent=2, ensure_ascii=False)};\n"

    path.write_text(HEADER + const("meta", "", meta) + "\n" + const("datasets", ": Dataset[]", datasets))


def _load(results: Path, name: str):
    path = results / name
    return json.loads(path.read_text()) if path.exists() else None


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

    datasets: dict[str, dict] = {}

    compare = {path.stem: json.loads(path.read_text()) for path in sorted((args.results / "compare").glob("*.json"))}

    def compare_footer(results: list[dict]) -> str:
        return (
            f"Measured {args.date} on {args.machine}, {args.seconds:g} s per solver and instance, "
            f"seed {_seed(results, 1)}. The engine runs single-threaded; CP-SAT uses {args.workers} workers "
            "and its budget starts once its model is built. Single runs — treat small differences as noise."
        )

    for slug in COMPARED:
        if slug not in compare:
            continue
        measured = [result for result in compare[slug] if "error" not in result]
        _check_protocol(slug, measured, args)
        datasets[slug] = compare_dataset(slug, compare[slug], args.seconds)
        _replace_block(_readme_of(slug), compare_block(compare[slug], compare_footer(measured)))

    set_x, set_a = _load(args.results, "vrp-x.json"), _load(args.results, "vrp-a.json")
    for name, data in (("vehicle-routing (Set X)", set_x), ("vehicle-routing (Set A)", set_a)):
        if data:
            _check_vrp(name, data, args)
    if set_x or set_a:
        _replace_block(BENCHMARKS / "vehicle-routing" / "README.md", vrp_block(set_x, set_a, args))
    if set_x:
        datasets["cvrplib-x"] = vrp_dataset("cvrplib-x", set_x)
    if set_a:
        datasets["cvrplib-a"] = vrp_dataset("cvrplib-a", set_a)

    li_lim, synthetic = _load(args.results, "pdp-li-lim.json"), _load(args.results, "pdp-synthetic.json")
    pdp = BENCHMARKS / "pickup-delivery"
    if li_lim:
        _check_protocol("pickup-delivery (Li & Lim)", li_lim, args)
        datasets["li-lim-pdptw"] = pdp_li_lim_dataset(li_lim, args.seconds)
        _replace_block(
            pdp / "cpsat" / "PUBLIC_BENCHMARKS.md",
            pdp_li_lim_block(li_lim, args) + "\n\n" + _footer(args, _seed(li_lim, 0)),
        )
    if synthetic:
        _check_protocol("pickup-delivery (synthetic)", synthetic, args)
        datasets["pickup-delivery-synthetic"] = pdp_synthetic_dataset(synthetic, args.seconds)
        _replace_block(
            pdp / "README.md",
            "## Results\n\n"
            + pdp_synthetic_block(synthetic)
            + "\n\nThe Li & Lim results are in [`cpsat/PUBLIC_BENCHMARKS.md`](cpsat/PUBLIC_BENCHMARKS.md).\n\n"
            + _footer(args, _seed(synthetic, 0)),
        )

    rcpsp = _load(args.results, "rcpsp.json")
    if rcpsp:
        _check_protocol("project-job-scheduling (RG30)", rcpsp, args)
        datasets["rg30-rcpsp"] = rcpsp_dataset(rcpsp, args.seconds)
        _replace_block(
            BENCHMARKS / "project-job-scheduling" / "cpsat" / "PUBLIC_BENCHMARKS.md", rcpsp_block(rcpsp, args)
        )

    bpp = _load(args.results, "bpp.json")
    if bpp:
        _check_protocol("bin-packing", bpp, args)
        datasets["falkenauer-bin-packing"] = bpp_dataset(bpp, args.seconds)
        _replace_block(BENCHMARKS / "bin-packing" / "README.md", bpp_block(bpp, args))

    qaplib = _load(args.results, "qaplib.json")
    if qaplib:
        _check_protocol("quadratic-assignment (QAPLIB)", qaplib, args)
        datasets["qaplib"] = qaplib_dataset(qaplib, args.seconds)
        body = qaplib_block(qaplib, args)
        _replace_block(BENCHMARKS / "quadratic-assignment" / "README.md", "## QAPLIB\n\n" + body, "qaplib")
        _replace_block(BENCHMARKS / "quadratic-assignment" / "bench" / "RESULTS.md", body)

    salbp = {name: _load(args.results, f"salbp-{name}.json") for name, _ in SALBP_SETS}
    salbp = {name: results for name, results in salbp.items() if results}
    if salbp:
        for name, results in salbp.items():
            _check_protocol(f"assembly-line-balancing ({name})", results, args)
        datasets["salbp-1"] = salbp_dataset(salbp, args.seconds)
        block = salbp_block(salbp, args)
        _replace_block(BENCHMARKS / "assembly-line-balancing" / "README.md", block)
        _replace_block(
            BENCHMARKS / "assembly-line-balancing" / "bench" / "RESULTS.md", block.removeprefix("## Results\n\n")
        )

    extra = _load(args.results, "extra.json") or {}
    if extra.get("timetabling"):
        datasets["school-timetabling"] = timetabling_dataset(extra["timetabling"])

    for dataset in datasets.values():
        dataset.setdefault("date", args.date)
        dataset.setdefault("seconds", int(args.seconds) if float(args.seconds).is_integer() else args.seconds)
        # Keep the key order of the TypeScript interface: the tables last.
        dataset["sections"] = dataset.pop("sections")

    if args.homepage is not None:
        target = args.homepage / "src" / "data" / "benchmarks.ts"
        merged = {**_current_datasets(target), **datasets}
        ordered = [merged[slug] for slug in ORDER if slug in merged]
        ordered += [entry for slug, entry in merged.items() if slug not in ORDER]
        meta = {
            "ortools": args.ortools,
            "workers": args.workers,
            "machine": {"en": args.machine, "ko": args.machine_ko},
        }
        write_homepage(target, meta, ordered)
        print(f"homepage: {len(ordered)} data sets ({len(datasets)} from this run)")


if __name__ == "__main__":
    main()
