"""Engine vs CP-SAT comparison for task-assigning.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`task_assigning.constraint_costs`).
Run: `uv run python -m task_assigning_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import random
import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "task-assigning"
PACKAGE = "optimaru.task_assigning"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None


SKILLS = [
    "Problem Solving",
    "Team Building",
    "Business Storytelling",
    "Risk Management",
    "Creative Thinking",
    "Strategic Planning",
]
AFFINITIES = ["NONE", "LOW", "MEDIUM", "HIGH"]
PRIORITIES = ["MINOR"] * 5 + ["MAJOR"] * 3 + ["CRITICAL"] * 2


def _generate(task_count: int, employee_count: int, customer_count: int, seed: int) -> Any:
    """A seeded instance in the shape of the demo data: employees with one to three of six
    skills and an affinity to every customer, task types that need one or two skills some
    employee has together, and tasks of mixed priority."""
    from optimaru.task_assigning.models import (
        CustomerModel,
        EmployeeModel,
        TaskAssigningSolutionModel,
        TaskModel,
        TaskTypeModel,
    )

    rng = random.Random(seed)
    customers = [CustomerModel(id=str(index + 1), name=f"Customer {index + 1}") for index in range(customer_count)]
    employees = [
        EmployeeModel(
            id=str(index + 1),
            full_name=f"Employee {index + 1}",
            # Every skill is somebody's first skill, so no skill goes uncovered; a draw of
            # the first skill again is dropped.
            skills=list(dict.fromkeys([SKILLS[index % len(SKILLS)], *rng.sample(SKILLS, rng.randint(1, 2))])),
            customer_to_affinity={customer.id: rng.choice(AFFINITIES) for customer in customers},
        )
        for index in range(employee_count)
    ]
    task_types = []
    for index in range(4 + task_count // 60):
        # The skills of a task type are skills one employee has, so somebody can do it.
        able = rng.choice(employees).skills
        task_types.append(
            TaskTypeModel(
                code=f"T{index + 1}",
                title=f"Task type {index + 1}",
                base_duration=rng.randint(30, 90),
                required_skills=rng.sample(able, min(len(able), rng.randint(1, 2))),
            )
        )
    counts = dict.fromkeys((task_type.code for task_type in task_types), 0)
    tasks = []
    for index in range(task_count):
        code = rng.choice(task_types).code
        counts[code] += 1
        tasks.append(
            TaskModel(
                id=str(index + 1),
                task_type_code=code,
                index_in_task_type=counts[code],
                customer_id=rng.choice(customers).id,
                priority=rng.choice(PRIORITIES),
            )
        )
    return TaskAssigningSolutionModel(
        task_types=task_types, customers=customers, tasks=tasks, employees=employees
    ).to_core()


def instances() -> dict[str, Callable[[], Any]]:
    from optimaru.task_assigning import demo

    return {
        "small": lambda: _generate(12, 4, 3, seed=1),
        # The demo data: 28 tasks, 8 employees, 4 customers.
        "demo": lambda: demo.generate_demo_data().to_core(),
        "medium": lambda: _generate(60, 12, 6, seed=2),
        "large": lambda: _generate(120, 20, 8, seed=3),
        "xlarge": lambda: _generate(240, 32, 12, seed=4),
    }


def describe(problem: Any) -> str:
    from optimaru.task_assigning.models import TaskAssigningSolutionModel

    model = TaskAssigningSolutionModel.from_core(problem)
    return f"{len(model.tasks)} tasks / {len(model.employees)} employees / {len(model.customers)} customers"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    from optimaru import task_assigning

    task_assigning.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from external_solver.task_assigning import solve

    from optimaru.task_assigning.models import TaskAssigningSolutionModel

    solved = solve(
        TaskAssigningSolutionModel.from_core(problem),
        time_limit_seconds=seconds,
        random_seed=seed,
        num_workers=workers,
    )
    return solved.to_core()


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
