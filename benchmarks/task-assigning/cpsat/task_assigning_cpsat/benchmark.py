"""Engine vs CP-SAT comparison for task-assigning.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`task_assigning.constraint_costs`).
Run: `uv run python -m task_assigning_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "task-assigning"
PACKAGE = "task_assigning"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


def instances() -> dict[str, Callable[[], Any]]:
    import task_assigning.demo

    return {"demo": lambda: task_assigning.demo.generate_demo_data().to_core()}


def describe(problem: Any) -> str:
    return "Java quickstart demo"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import task_assigning

    task_assigning.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from task_assigning.models import TaskAssigningSolutionModel

    from task_assigning_cpsat import solve

    solved = solve(
        TaskAssigningSolutionModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "task-assigning"


def to_timefold(problem: Any) -> dict:
    # Task types and customers come first, so later references to them can be
    # plain ids (the quickstart's @JsonIdentityInfo).
    task_types, customers, tasks = problem.task_types, problem.customers, problem.tasks
    return {
        "taskTypes": [
            {"code": t.code, "title": t.title, "baseDuration": t.base_duration, "requiredSkills": list(t.required_skills)}
            for t in task_types
        ],
        "customers": [{"id": c.id, "name": c.name} for c in customers],
        "tasks": [
            {
                "id": t.id,
                "taskType": task_types[t.task_type].code,
                "indexInTaskType": t.index_in_task_type,
                "customer": customers[t.customer].id,
                "minStartTime": t.min_start_time,
                "priority": t.priority.name,
            }
            for t in tasks
        ],
        "employees": [
            {
                "id": e.id,
                "fullName": e.full_name,
                "skills": list(e.skills),
                "customerToAffinity": {
                    customers[i].id: affinity.name for i, affinity in enumerate(e.affinity_by_customer)
                },
                "tasks": [tasks[i].id for i in e.tasks],
            }
            for e in problem.employees
        ],
    }


def from_timefold(problem: Any, result: dict) -> None:
    index = {t.id: i for i, t in enumerate(problem.tasks)}
    chosen = result["assignment"]["Employee"]
    problem.set_task_lists([[index[task] for task in chosen[e.id]["tasks"]] for e in problem.employees])


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
