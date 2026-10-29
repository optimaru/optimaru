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
        if len(self.self_report) != len(self.score.levels):
            return f"LEVEL SHAPE self={self.self_report} rescore={self.score.levels}"
        for reported, rescored in zip(self.self_report, self.score.levels, strict=True):
            if abs(reported - rescored) > 1e-6 * max(1.0, abs(reported)):
                return f"MISMATCH self={self.self_report} rescore={self.score.levels}"
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


class BenchmarkCase(Protocol):
    NAME: str
    PACKAGE: str
    CPSAT_EXACT: bool
    CPSAT_CAVEAT: str

    def instances(self) -> dict[str, Callable[[], Any]]: ...
    def describe(self, problem: Any) -> str: ...
    def solve_engine(self, problem: Any, seconds: float, seed: int) -> None: ...
    def solve_cpsat(
        self, problem: Any, seconds: float, seed: int, workers: int
    ) -> Any: ...


def score_solution(package: str, solution: Any, constraints=None) -> Score:
    module = importlib.import_module(package)
    specs = importlib.import_module(f"{package}.constraints")
    result = specs.score(module.constraint_costs(solution), constraints)
    return Score(tuple(result), tuple(result._fields))


def _self_report(case: Any, problem: Any) -> tuple[float, ...] | None:
    reader = getattr(case, "self_report", None)
    if reader is not None:
        return reader(problem)
    score = getattr(problem, "score", None)
    if score is None:
        return None
    levels = [getattr(score, name, None) for name in ("hard", "medium", "soft")]
    return tuple(level for level in levels if level is not None)


def _solve_cpsat(
    case: BenchmarkCase, problem: Any, seconds: float, seed: int, workers: int
) -> SolverRun:
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


def _cpsat_child(
    module: str, instance: str, seconds: float, seed: int, workers: int, conn: Any
) -> None:
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
            raise RuntimeError(
                f"CP-SAT child process died (exit code {child.exitcode})"
            ) from None
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
) -> InstanceResult:
    engine_problem = factory()
    started = time.perf_counter()
    case.solve_engine(engine_problem, seconds, seed)
    engine_elapsed = time.perf_counter() - started
    engine_run = SolverRun(
        "engine",
        score_solution(case.PACKAGE, engine_problem),
        engine_elapsed,
        self_report=_self_report(case, engine_problem),
    )

    # Only an importable case module can be rebuilt in a child process.
    if deadline is not None and getattr(case, "__spec__", None) is not None:
        cpsat_run = _solve_cpsat_bounded(
            case, instance, seconds, seed, workers, deadline
        )
    else:
        cpsat_run = _solve_cpsat(case, factory(), seconds, seed, workers)

    runs = [engine_run, cpsat_run]

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
    return next(
        (run for run in result.get("runs", []) if run["solver"] == solver), None
    )


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


def _gap(result: dict, solver: str = "engine", reference: str = "cp-sat") -> str:
    """The engine's last-level gap to `reference`, when the higher levels tie."""
    engine, other = _run(result, solver), _run(result, reference)
    if (
        not engine
        or not other
        or engine.get("score") is None
        or other.get("score") is None
    ):
        return ""
    left, right = list(engine["score"]), list(other["score"])
    if left[:-1] != right[:-1]:
        return ""
    base = abs(right[-1])
    if base < 1e-9:
        return "" if abs(left[-1]) < 1e-9 else "n/a"
    return f"{(abs(left[-1]) - base) / base * 100:+.2f}%"


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
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args(argv)

    wanted = set(args.instance) if args.instance else None
    results: list[dict] = []
    for name, factory in case.instances().items():
        if wanted is not None and name not in wanted:
            continue
        if not args.as_json:
            print(f"running {case.NAME}/{name} ...", file=sys.stderr)
        try:
            results.append(
                _as_dict(
                    run_instance(
                        case,
                        name,
                        factory,
                        args.seconds,
                        args.seed,
                        args.workers,
                        args.seconds + args.cpsat_grace,
                    )
                )
            )
        except Exception:  # noqa: BLE001 - one broken instance should still yield a report.
            results.append(
                {
                    "case": case.NAME,
                    "instance": name,
                    "error": traceback.format_exc(limit=6),
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


class JavaRandom:
    """Small java.util.Random replica used by deterministic demo adapters."""

    _MULTIPLIER = 0x5DEECE66D
    _ADDEND = 0xB
    _MASK = (1 << 48) - 1

    def __init__(self, seed: int = 0) -> None:
        self._seed = (seed ^ self._MULTIPLIER) & self._MASK

    def _next(self, bits: int) -> int:
        self._seed = (self._seed * self._MULTIPLIER + self._ADDEND) & self._MASK
        return self._seed >> (48 - bits)

    def next_int(self, bound: int) -> int:
        if bound <= 0:
            raise ValueError("bound must be positive")
        if bound & -bound == bound:
            return (bound * self._next(31)) >> 31
        while True:
            bits = self._next(31)
            value = bits % bound
            if bits - value + (bound - 1) < (1 << 31):
                return value

    def shuffle(self, items: list) -> None:
        for index in range(len(items) - 1, 0, -1):
            swap = self.next_int(index + 1)
            items[index], items[swap] = items[swap], items[index]
