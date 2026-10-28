"""Engine vs CP-SAT comparison for order-picking.

Both solvers get the same instances and wall-clock budget; both answers are
rescored by the use case's own evaluator (`order_picking.constraint_costs`).
Run: `uv run python -m order_picking_cpsat.benchmark --seconds 10`.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from typing import Any

from cpsat_plugins.benchmark import run_cli

NAME = "order-picking"
PACKAGE = "order_picking"
CPSAT_EXACT = True
CPSAT_CAVEAT = ""


def self_report(problem: Any) -> None:
    # Compare rescored answers only; the engine folds medium levels into its
    # two-level score, so its own report is not level-for-level comparable.
    return None

import random

# to_solution() builds a fresh core solution per call; remember which request
# each one came from so the CP-SAT side can solve the same request.
_REQUESTS: dict[int, Any] = {}


def _build(order_count: int, trolley_count: int, seed: int) -> Any:
    from order_picking.models import (
        LocationModel, OrderModel, OrderPickingRequest, ProductModel, TrolleyModel, to_solution,
    )

    rng = random.Random(seed)

    def location() -> LocationModel:
        return LocationModel(
            column=rng.choice("ABCDE"), shelving_row=rng.randint(1, 3),
            side=rng.choice(["LEFT", "RIGHT"]), row=rng.randint(0, 10),
        )

    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    request = OrderPickingRequest(
        trolleys=[
            TrolleyModel(id=f"T{t}", bucket_count=6, bucket_capacity=50_000, location=origin)
            for t in range(trolley_count)
        ],
        orders=[
            OrderModel(
                id=f"O{o}",
                items=[
                    ProductModel(id=f"O{o}-P{i}", volume=rng.randint(500, 8_000), location=location())
                    for i in range(rng.randint(1, 5))
                ],
            )
            for o in range(order_count)
        ],
    )
    solution = to_solution(request)
    _REQUESTS[id(solution)] = request
    return solution


def instances() -> dict[str, Callable[[], Any]]:
    return {
        "small": lambda: _build(8, 2, seed=0),
        "medium": lambda: _build(20, 4, seed=1),
    }


def describe(problem: Any) -> str:
    return f"{len(problem.pick_tasks)} picks / {len(problem.trolleys)} trolleys"


def solve_engine(problem: Any, seconds: float, seed: int) -> None:
    import order_picking

    order_picking.solve(problem, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1)


def solve_cpsat(problem: Any, seconds: float, seed: int, workers: int) -> Any:
    from order_picking_cpsat import solve_request

    request = _REQUESTS[id(problem)].model_copy(
        update={"timelimit": seconds, "random_seed": seed, "num_workers": workers}
    )
    response = solve_request(request)
    task_by_id = {task.id: index for index, task in enumerate(problem.pick_tasks)}
    for trolley in problem.trolleys:
        trolley.pick_task_indices = [task_by_id[stop.pick_task_id] for stop in response.routes.get(trolley.id, [])]
    return problem



# --- Timefold quickstart (benchmarks/timefold-quickstarts) ---------------------

TIMEFOLD = "order-picking"


def to_timefold(problem: Any) -> dict:
    orders, tasks = problem.orders, problem.pick_tasks

    def location(loc: Any) -> dict:
        return {"shelvingId": loc.shelving_id, "side": loc.side.name, "row": loc.row}

    return {
        "trolleys": [
            {
                "id": t.id,
                "bucketCount": t.bucket_count,
                "bucketCapacity": t.bucket_capacity,
                "location": location(t.location),
                "pickTasks": [],
            }
            for t in problem.trolleys
        ],
        "pickTasks": [
            {
                "id": task.id,
                "orderItem": {
                    # "<order id>/<index>": the runner rebuilds the orders from it.
                    "id": f"{orders[task.order_index].id}/{i}",
                    "product": {
                        "id": task.id,
                        "name": task.product_name,
                        "volume": task.volume,
                        "location": location(task.location),
                    },
                },
            }
            for i, task in enumerate(tasks)
        ],
        "__lists": {
            "Trolley": {t.id: {"pickTasks": [tasks[i].id for i in t.pick_task_indices]} for t in problem.trolleys}
        },
    }


def from_timefold(problem: Any, result: dict) -> None:
    index = {task.id: i for i, task in enumerate(problem.pick_tasks)}
    chosen = result["assignment"]["Trolley"]
    trolleys = problem.trolleys
    for trolley in trolleys:
        trolley.pick_task_indices = [index[task] for task in chosen[trolley.id]["pickTasks"]]
    problem.trolleys = trolleys


def main(argv: list[str] | None = None) -> int:
    return run_cli(sys.modules[__name__], argv)


if __name__ == "__main__":
    raise SystemExit(main())
