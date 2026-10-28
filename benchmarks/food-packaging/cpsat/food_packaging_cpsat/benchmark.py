"""Engine vs CP-SAT comparison for food-packaging.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`food_packaging.constraint_costs`).
Run: `uv run python -m food_packaging_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "food-packaging"
PACKAGE = "food_packaging"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import food_packaging

    return {"demo": lambda: food_packaging.generate_demo_data()}


def describe(problem: Any) -> str:
    return "Java quickstart demo plant"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import food_packaging

    food_packaging.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from food_packaging.models import PackagingScheduleModel

    from food_packaging_cpsat import solve

    solved = solve(
        PackagingScheduleModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "food-packaging"


def to_timefold(problem: Any) -> dict:
    from food_packaging.models import minute_to_datetime

    def when(minute: int) -> str:
        return minute_to_datetime(minute).isoformat(timespec="seconds")

    products, operators, lines, jobs = problem.products, problem.operators, problem.lines, problem.jobs
    return {
        "workCalendar": {
            "fromDate": minute_to_datetime(problem.from_minute).date().isoformat(),
            "toDate": minute_to_datetime(problem.to_minute).date().isoformat(),
        },
        "products": [
            {
                "id": p.id,
                "name": p.name,
                "cleaningDurations": {
                    previous.id: f"PT{problem.cleanup_of(i, j)}M" for i, previous in enumerate(products)
                },
            }
            for j, p in enumerate(products)
        ],
        "operators": [{"id": o.id} for o in operators],
        "lines": [
            {
                "id": line.id,
                "name": line.name,
                "startDateTime": when(line.start_minute),
                "operator": operators[line.operator_index].id if line.operator_index >= 0 else None,
            }
            for line in lines
        ],
        "jobs": [
            {
                "id": job.id,
                "name": job.name,
                "product": {"id": products[job.product_index].id},
                "duration": f"PT{job.duration_minutes}M",
                "minStartTime": when(job.min_start_minute),
                "idealEndTime": when(job.ideal_end_minute),
                "maxEndTime": when(job.max_end_minute),
                "priority": job.priority,
                "pinned": job.pinned,
            }
            for job in jobs
        ],
        # Line.jobs is not part of the quickstart's JSON.
        "__lists": {"Line": {line.id: {"jobs": [jobs[j].id for j in line.jobs]} for line in lines}},
    }


def from_timefold(problem: Any, result: dict) -> None:
    job_index = {job.id: i for i, job in enumerate(problem.jobs)}
    operator_index = {o.id: i for i, o in enumerate(problem.operators)}
    chosen = result["assignment"]["Line"]
    lines = problem.lines
    assigned: set[int] = set()
    for line in lines:
        variables = chosen[line.id]
        line.jobs = [job_index[job] for job in variables["jobs"]]
        line.operator_index = operator_index[variables["operator"]] if variables["operator"] else -1
        assigned.update(line.jobs)
    problem.lines = lines
    problem.unassigned_jobs = [i for i in range(len(problem.jobs)) if i not in assigned]
    problem.update_shadow_variables()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
