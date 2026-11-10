"""Shared mechanics for use-case-local engine/CP-SAT comparisons.

Each use case owns its instances and solver adapters.  This module only times
the two calls, re-scores both answers with the domain evaluator, and renders a
small report. Keeping the runner local avoids a central use-case registry.
"""

from __future__ import annotations

import argparse
import importlib
import json
import multiprocessing
import sys
import time
import traceback
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from typing import Any, Protocol


@dataclass(frozen=True, order=True)
class Score:
    levels: tuple[float, ...]
    names: tuple[str, ...] = field(default=(), compare=False)
    #: The engine's own levels when a medium level was unfolded for display (`_unfold_medium`).
    folded: tuple[float, ...] | None = field(default=None, compare=False)

    def __str__(self) -> str:
        names = self.names or ("hard", "medium", "soft")[-len(self.levels) :]
        parts = []
        for name, value in zip(names, self.levels, strict=True):
            text = str(int(value)) if float(value).is_integer() else f"{value:.4f}"
            parts.append(f"{text}{name}")
        return "/".join(parts)


@dataclass
class SolverRun:
    solver: str
    score: Score | None
    seconds: float
    status: str = ""
    self_report: tuple[float, ...] | None = None

    @property
    def crosscheck(self) -> str:
        if self.self_report is None or self.score is None:
            return ""
        # A solver reports the engine's levels, so compare with those, not the unfolded display.
        rescore = self.score.folded or self.score.levels
        if len(self.self_report) != len(rescore):
            return f"LEVEL SHAPE self={self.self_report} rescore={rescore}"
        for reported, rescored in zip(self.self_report, rescore, strict=True):
            if abs(reported - rescored) > 1e-6 * max(1.0, abs(reported)):
                return f"MISMATCH self={self.self_report} rescore={rescore}"
        return ""


@dataclass
class InstanceResult:
    case: str
    instance: str
    size: str
    budget: float
    runs: list[SolverRun]
    exact: bool = True
    caveat: str = ""


#: The solvers of a comparison, in the order of its columns.
SOLVERS = ("engine", "cp-sat")


class BenchmarkCase(Protocol):
    NAME: str
    PACKAGE: str
    CPSAT_EXACT: bool
    CPSAT_CAVEAT: str

    def instances(self) -> dict[str, Callable[[], Any]]: ...
    def describe(self, problem: Any) -> str: ...
    def solve_engine(self, problem: Any, seconds: float, seed: int) -> None: ...
    def solve_cpsat(self, problem: Any, seconds: float, seed: int, workers: int) -> Any: ...


def score_solution(package: str, solution: Any) -> Score:
    module = importlib.import_module(package)
    specs = importlib.import_module(f"{package}.constraints")
    costs = module.constraint_costs(solution)
    result = specs.score(costs)
    return _unfold_medium(Score(tuple(result), tuple(result._fields)), costs)


def _unfold_medium(score: Score, costs: Any) -> Score:
    """Show a folded medium level on its own, as ``hard/medium/soft``.

    A domain with a medium level reports a two-level engine score with medium
    folded into soft (``into_soft``) or below hard (``below_hard``) at a fixed
    ``unit``; every cost entry carries that ``medium_fold``. Splitting it back
    keeps the order of the scores and makes the table readable: a folded soft
    of -9000000020 at unit 1e9 is medium -9, soft -20.
    """
    folds = [entry.get("medium_fold") for entry in costs.values() if isinstance(entry, dict)]
    fold = next((item for item in folds if item), None)
    if fold is None or score.names != ("hard", "soft"):
        return score
    hard, soft = score.levels
    unit = float(fold["unit"])
    if fold["kind"] == "into_soft":
        medium = int(soft / unit)
        return Score((hard, medium, soft - medium * unit), ("hard", "medium", "soft"), score.levels)
    if fold["kind"] == "below_hard":
        # The hard number holds hard * unit + medium.
        whole = int(hard / unit)
        return Score((whole, hard - whole * unit, soft), ("hard", "medium", "soft"), score.levels)
    return score


def _self_report(case: Any, problem: Any) -> tuple[float, ...] | None:
    reader = getattr(case, "self_report", None)
    if reader is not None:
        return reader(problem)
    score = getattr(problem, "score", None)
    if score is None:
        return None
    levels = [getattr(score, name, None) for name in ("hard", "medium", "soft")]
    return tuple(level for level in levels if level is not None)


def _solve_cpsat(case: BenchmarkCase, problem: Any, seconds: float, seed: int, workers: int) -> SolverRun:
    started = time.perf_counter()
    try:
        solved = case.solve_cpsat(problem, seconds, seed, workers)
    except RuntimeError as error:
        message = str(error)
        if "status=" not in message:
            raise
        status = message.split("status=", 1)[1].split(")", 1)[0]
        return SolverRun("cp-sat", None, time.perf_counter() - started, status)
    return SolverRun(
        "cp-sat",
        score_solution(case.PACKAGE, solved),
        time.perf_counter() - started,
        # Public CP-SAT backends return a solution, not CpSolverStatus.
        "FEASIBLE",
    )


def _cpsat_child(module: str, instance: str, seconds: float, seed: int, workers: int, conn: Any) -> None:
    case = importlib.import_module(module)
    try:
        problem = case.instances()[instance]()
        conn.send(("run", _solve_cpsat(case, problem, seconds, seed, workers)))
    except BaseException:  # noqa: BLE001 - reported by the parent.
        conn.send(("error", traceback.format_exc(limit=6)))


def _solve_cpsat_bounded(
    case: Any, instance: str, seconds: float, seed: int, workers: int, deadline: float
) -> SolverRun:
    """Run CP-SAT in a child process and stop it at `deadline` seconds.

    CP-SAT's own time limit starts only after the model is built, and some
    models (pairwise encodings of large instances) take minutes and gigabytes
    to build. Without a wall-clock bound one such instance stalls or kills
    the whole run, so a child still going at the deadline is reported as
    "no solution in time".
    """
    context = multiprocessing.get_context("spawn")
    receive, send = context.Pipe(duplex=False)
    child = context.Process(
        target=_cpsat_child,
        args=(case.__spec__.name, instance, seconds, seed, workers, send),
        daemon=True,
    )
    child.start()
    send.close()
    try:
        if not receive.poll(deadline):
            return SolverRun("cp-sat", None, deadline, "no solution in time")
        try:
            kind, payload = receive.recv()
        except EOFError:
            child.join(5)
            raise RuntimeError(f"CP-SAT child process died (exit code {child.exitcode})") from None
    finally:
        child.kill()
        child.join()
        receive.close()
    if kind == "error":
        raise RuntimeError(f"CP-SAT run failed:\n{payload}")
    return payload


def run_instance(
    case: BenchmarkCase,
    instance: str,
    factory: Callable[[], Any],
    seconds: float,
    seed: int,
    workers: int,
    deadline: float | None = None,
    solvers: Sequence[str] = SOLVERS,
) -> InstanceResult:
    """Solve one instance with each of `solvers` and re-score the answers.

    The two solvers never share state, so a run of one solver alone gives the run
    it would have in a comparison; that lets the single-threaded engine runs and
    the CP-SAT runs of a long list be measured side by side on separate cores.
    """
    # Built for a CP-SAT-only run too: the size of the result is described from it.
    engine_problem = factory()
    runs = []
    if "engine" in solvers:
        started = time.perf_counter()
        case.solve_engine(engine_problem, seconds, seed)
        engine_elapsed = time.perf_counter() - started
        runs.append(
            SolverRun(
                "engine",
                score_solution(case.PACKAGE, engine_problem),
                engine_elapsed,
                self_report=_self_report(case, engine_problem),
            )
        )
    if "cp-sat" in solvers:
        # Only an importable case module can be rebuilt in a child process.
        if deadline is not None and getattr(case, "__spec__", None) is not None:
            runs.append(_solve_cpsat_bounded(case, instance, seconds, seed, workers, deadline))
        else:
            runs.append(_solve_cpsat(case, factory(), seconds, seed, workers))

    return InstanceResult(
        case=case.NAME,
        instance=instance,
        size=case.describe(engine_problem),
        budget=seconds,
        runs=runs,
        exact=getattr(case, "CPSAT_EXACT", True),
        caveat=getattr(case, "CPSAT_CAVEAT", ""),
    )


def _run(result: dict, solver: str) -> dict | None:
    return next((run for run in result.get("runs", []) if run["solver"] == solver), None)


def _score_text(run: dict | None) -> str:
    if not run or run.get("score") is None:
        return f"— ({(run or {}).get('status', 'no solution')})"
    return run["score_text"]


def _verdict(result: dict) -> str:
    """The best solver by the use case's own score; ties are named together."""
    scored = [run for run in result.get("runs", []) if run.get("score") is not None]
    if not scored:
        return "—"
    best = max(tuple(run["score"]) for run in scored)
    winners = [run["solver"] for run in scored if tuple(run["score"]) == best]
    return "tie" if len(winners) == len(result["runs"]) > 1 else " = ".join(winners)


def _as_dict(result: InstanceResult) -> dict:
    runs = []
    for run in result.runs:
        runs.append(
            {
                "solver": run.solver,
                "score": None if run.score is None else list(run.score.levels),
                "score_names": None if run.score is None else list(run.score.names),
                "score_text": None if run.score is None else str(run.score),
                "seconds": run.seconds,
                "status": run.status,
                "self_report": run.self_report,
                "crosscheck": run.crosscheck,
            }
        )
    return {
        "case": result.case,
        "instance": result.instance,
        "size": result.size,
        "budget": result.budget,
        "runs": runs,
        "exact": result.exact,
        "caveat": result.caveat,
    }


def _solvers(results: list[dict]) -> list[str]:
    names: list[str] = []
    for result in results:
        for run in result.get("runs", []):
            if run["solver"] not in names:
                names.append(run["solver"])
    return names or ["engine", "cp-sat"]


def render_console(results: list[dict], seconds: float, workers: int, seed: int) -> str:
    solvers = _solvers(results)
    lines = [
        f"{' vs '.join(solvers)} — {seconds:g}s solve budget each, seed {seed}",
        f"engine: 1 thread   cp-sat: {workers} workers (time includes model build)",
        "",
        f"{'instance':<10} {'size':<28} " + " ".join(f"{name:>24}" for name in solvers) + "  verdict",
        "-" * (42 + 25 * len(solvers) + 10),
    ]
    for result in results:
        if "error" in result:
            lines.append(f"{result['instance']:<10} FAILED — {result['error'].splitlines()[-1]}")
            continue
        cells = " ".join(f"{_score_text(_run(result, name)):>24}" for name in solvers)
        lines.append(f"{result['instance']:<10} {result['size']:<28} {cells}  {_verdict(result)}")
        if result.get("caveat"):
            lines.append(f"  note: {result['caveat']}")
        engine = _run(result, "engine")
        if engine and engine.get("crosscheck"):
            lines.append(f"  crosscheck: {engine['crosscheck']}")
    return "\n".join(lines)


def render_markdown(results: list[dict], seconds: float, workers: int, seed: int) -> str:
    solvers = _solvers(results)
    lines = [
        (
            f"Every solver got a **{seconds:g}s** budget, seed {seed}. The engine "
            f"uses one thread; CP-SAT uses {workers} workers and its time "
            "includes model construction. Scores are the use case's own evaluator "
            "(higher is better, levels compared left to right)."
        ),
        "",
        "| instance | size | " + " | ".join(solvers) + " | best |",
        "| --- | --- | " + " | ".join("---" for _ in solvers) + " | --- |",
    ]
    for result in results:
        if "error" in result:
            lines.append(f"| {result['instance']} | — | " + " | ".join("FAILED" for _ in solvers) + " | — |")
            continue
        cells = " | ".join(_score_text(_run(result, name)) for name in solvers)
        lines.append(f"| {result['instance']} | {result['size']} | {cells} | {_verdict(result)} |")
    return "\n".join(lines)


def run_cli(case: BenchmarkCase, argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog=f"{case.NAME}-compare")
    parser.add_argument("--instance", action="append", default=None)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument(
        "--cpsat-grace",
        type=float,
        default=60.0,
        help="seconds past the budget before a CP-SAT run (model build included) "
        "is stopped and reported as no solution in time",
    )
    parser.add_argument(
        "--solver",
        choices=(*SOLVERS, "both"),
        default="both",
        help="run one solver only; the JSON of an engine-only and a cp-sat-only run of the "
        "same instances add up to a comparison",
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args(argv)
    solvers = SOLVERS if args.solver == "both" else (args.solver,)

    wanted = set(args.instance) if args.instance else None
    results: list[dict] = []
    for name, factory in case.instances().items():
        if wanted is not None and name not in wanted:
            continue
        if not args.as_json:
            print(f"running {case.NAME}/{name} ...", file=sys.stderr)
        try:
            result = _as_dict(
                run_instance(
                    case,
                    name,
                    factory,
                    args.seconds,
                    args.seed,
                    args.workers,
                    args.seconds + args.cpsat_grace,
                    solvers,
                )
            )
            # What the run was given, so a table can state it and a publisher can check it.
            result["seed"] = args.seed
            result["workers"] = args.workers
            results.append(result)
        except Exception:  # noqa: BLE001 - one broken instance should still yield a report.
            results.append(
                {
                    "case": case.NAME,
                    "instance": name,
                    "error": traceback.format_exc(limit=6),
                    "seed": args.seed,
                    "workers": args.workers,
                }
            )

    if args.as_json:
        json.dump(results, sys.stdout, indent=2)
        print()
    elif args.markdown:
        print(render_markdown(results, args.seconds, args.workers, args.seed))
    else:
        print(render_console(results, args.seconds, args.workers, args.seed))
    # A broken instance is still reported above, but fails the run.
    return 1 if any("error" in result for result in results) else 0
