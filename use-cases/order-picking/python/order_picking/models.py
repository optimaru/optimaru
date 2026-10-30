"""Pydantic request/response models for the order-picking solver.

JSON-friendly in/out (snake_case fields, PascalCase classes), same pattern as
``vrp_routing.models``:

    from order_picking.models import (LocationModel, OrderModel,
                                      OrderPickingRequest, ProductModel,
                                      TrolleyModel, solve_request)

    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000,
                               location=LocationModel(column="A", shelving_row=1,
                                                      side="LEFT", row=0))],
        orders=[OrderModel(id="order1", items=[
            ProductModel(id="p0", name="Milk Brick", volume=1232,
                         location=LocationModel(column="D", shelving_row=1,
                                                side="RIGHT", row=4)),
        ])],
    )
    solution = solve_request(request)
    print(solution.routes)                  # {"1": [stops...]}
    print(solution.distance_by_trolley)     # {"1": meters}

A shelving is addressed the Java way: column letter A..E + shelving row 1..3
(the id "(A,1)"), a side (LEFT/RIGHT) and a row 0..10 within the shelving.
Requires the optional ``pydantic`` dependency.
"""

from typing import Any, Literal, cast

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "LocationModel",
    "OrderModel",
    "OrderPickingRequest",
    "OrderPickingResponse",
    "PickStop",
    "ProductModel",
    "TrolleyModel",
    "solve_request",
    "to_solution",
    "with_initial_solution",
]


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


def _pick_task_id(order_id: str, item_position: int) -> str:
    return f"{order_id}-{item_position}"


class LocationModel(_Model):
    """A warehouse location: shelving (column, row), side and in-shelving row."""

    column: Literal["A", "B", "C", "D", "E"]
    shelving_row: int = Field(ge=1, le=3)
    side: Literal["LEFT", "RIGHT"]
    row: int = Field(ge=0, le=10)

    def to_core(self) -> "_core.WarehouseLocation":
        return _core.WarehouseLocation(
            column=ord(self.column) - ord("A"),
            shelving_row=self.shelving_row - 1,
            side=_core.Side.LEFT if self.side == "LEFT" else _core.Side.RIGHT,
            row=self.row,
        )

    @classmethod
    def from_core(cls, location: "_core.WarehouseLocation") -> "LocationModel":
        return cls(
            column=cast(Literal["A", "B", "C", "D", "E"], chr(ord("A") + location.column)),
            shelving_row=location.shelving_row + 1,
            side="LEFT" if location.side == _core.Side.LEFT else "RIGHT",
            row=location.row,
        )


class ProductModel(_Model):
    """One order item: an indivisible product at a warehouse location."""

    id: str
    volume: int = Field(gt=0, description="cm3")
    location: LocationModel
    name: str | None = None


class OrderModel(_Model):
    id: str
    items: list[ProductModel] = Field(min_length=1)


class TrolleyModel(_Model):
    id: str
    bucket_count: int = Field(ge=0)
    bucket_capacity: int = Field(gt=0, description="cm3")
    location: LocationModel
    initial_route: list[str] = Field(
        default_factory=list,
        description=(
            'Pick task ids ("<order>-<item position>") this trolley\'s route starts '
            "from. The solve keeps them as its starting point instead of constructing "
            "them, and places only what no route holds."
        ),
    )


class OrderPickingRequest(_Model):
    name: str | None = None
    timelimit: float = Field(default=30.0, ge=0, description="seconds")
    trolleys: list[TrolleyModel] = Field(min_length=1)
    orders: list[OrderModel] = Field(min_length=1)
    random_seed: int = 0
    num_workers: int = Field(default=1, ge=1)
    stop_at_first_feasible: bool = False
    solver_parameters: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="after")
    def _unique_ids(self):
        for kind, ids in (
            ("trolley", [t.id for t in self.trolleys]),
            ("order", [o.id for o in self.orders]),
        ):
            if len(ids) != len(set(ids)):
                raise ValueError(f"duplicate {kind} ids")
        known = {_pick_task_id(o.id, i) for o in self.orders for i in range(len(o.items))}
        seen: set[str] = set()
        for trolley in self.trolleys:
            for task_id in trolley.initial_route:
                if task_id not in known:
                    raise ValueError(f"trolley {trolley.id}: unknown pick task {task_id!r}")
                if task_id in seen:
                    raise ValueError(f"trolley {trolley.id}: pick task {task_id!r} appears twice in the initial routes")
                seen.add(task_id)
        return self


class PickStop(_Model):
    pick_task_id: str
    order_id: str
    product_name: str
    volume: int
    location: LocationModel


class OrderPickingResponse(_Model):
    routes: dict[str, list[PickStop]]  # trolley id -> ordered stops
    distance_by_trolley: dict[str, int]  # meters, incl. the return leg
    unassigned: list[str] = Field(default_factory=list)  # pick task ids
    score: str
    feasible: bool
    total_distance: int  # meters
    order_split_count: int  # sum of countDistinct(trolley) over orders


def to_solution(request: OrderPickingRequest) -> "_core.OrderPickingSolution":
    """Build the C++ OrderPickingSolution from the pydantic request: one
    Order per OrderModel and one PickTask per order item (ids "<order>-<i>",
    like the Java DemoDataGenerator.buildPickTasks)."""
    orders = [_core.Order(o.id) for o in request.orders]
    pick_tasks = []
    for order_index, order in enumerate(request.orders):
        for i, item in enumerate(order.items):
            pick_tasks.append(
                _core.PickTask(
                    id=_pick_task_id(order.id, i),
                    order_index=order_index,
                    volume=item.volume,
                    location=item.location.to_core(),
                    product_name=item.name or item.id,
                )
            )
    trolleys = [_core.Trolley(t.id, t.bucket_count, t.bucket_capacity, t.location.to_core()) for t in request.trolleys]
    task_index = {task.id: i for i, task in enumerate(pick_tasks)}
    for trolley, model in zip(trolleys, request.trolleys):
        trolley.pick_task_indices = [task_index[task_id] for task_id in model.initial_route]
    return _core.OrderPickingSolution(orders, trolleys, pick_tasks)


def solve_request(request: OrderPickingRequest, *, constraints=None) -> OrderPickingResponse:
    """Solve the request and shape the response (routes per trolley plus the
    per-trolley travel distance, mirroring the Java OrderPickingPlanning)."""
    solution = to_solution(request)
    config = _core.SolverConfig()
    config.spent_limit_seconds = request.timelimit
    config.random_seed = request.random_seed
    config.parallel_solver_count = request.num_workers
    _apply_solver_parameters(config, request.solver_parameters)
    if request.stop_at_first_feasible:
        config.best_score_limit = (0, -1e18)
    solver = _core.Solver(config)
    if constraints is None:
        solver.solve(solution)
    else:
        solver.solve_with_constraints(solution, constraints)

    routes: dict[str, list[PickStop]] = {}
    distance_by_trolley: dict[str, int] = {}
    assigned: set[int] = set()
    for trolley_index, trolley in enumerate(solution.trolleys):
        stops = []
        for pick_index in trolley.pick_task_indices:
            assigned.add(pick_index)
            task = solution.pick_tasks[pick_index]
            stops.append(
                PickStop(
                    pick_task_id=task.id,
                    order_id=solution.orders[task.order_index].id,
                    product_name=task.product_name,
                    volume=task.volume,
                    location=LocationModel.from_core(task.location),
                )
            )
        routes[trolley.id] = stops
        distance_by_trolley[trolley.id] = _core.distance_to_travel(solution, trolley_index)

    totals = _core.calculate_totals(solution)
    return OrderPickingResponse(
        routes=routes,
        distance_by_trolley=distance_by_trolley,
        unassigned=[task.id for i, task in enumerate(solution.pick_tasks) if i not in assigned],
        score=str(solution.score),
        feasible=solution.score.is_feasible(),
        total_distance=totals.distance_from_previous_pick_task + totals.distance_from_last_pick_task_to_path_origin,
        order_split_count=totals.order_split_by_trolley // 1000,
    )


def with_initial_solution(
    problem: OrderPickingRequest, solution: OrderPickingResponse | dict[str, Any]
) -> OrderPickingRequest:
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what `solve_request` returned, as the model or as its JSON
    dict. Each trolley's `initial_route` becomes the pick task ids of its route
    there, replacing what `problem` had; a trolley the solution leaves out
    starts empty. Trolleys and pick tasks are matched by id.

    A pick task id is "<order id>-<item position>", so it depends on where the
    item sits in `order.items`: reorder an order's items and the same id names
    a different item. Keep the order as it was when `solution` was solved.

    Tasks in no route are placed by construction. A solution that routes every
    task is the exact starting point. Raises ValueError on an unknown trolley or
    pick task, or a task routed twice.
    """
    routes = OrderPickingResponse.model_validate(solution).routes
    if unknown := routes.keys() - {trolley.id for trolley in problem.trolleys}:
        raise ValueError(f"unknown trolley {sorted(unknown)}")
    data = problem.model_dump()
    for trolley in data["trolleys"]:
        trolley["initial_route"] = [stop.pick_task_id for stop in routes.get(trolley["id"], [])]
    return OrderPickingRequest.model_validate(data)


def _apply_solver_parameters(config, parameters: dict[str, Any]) -> None:
    for name, value in parameters.items():
        if name in {"spent_limit_seconds", "random_seed", "parallel_solver_count", "on_new_best"}:
            raise ValueError(f"solver parameter {name!r} is controlled by the solve request")
        if name.startswith("_") or not hasattr(config, name) or callable(getattr(config, name)):
            raise ValueError(f"unknown solver parameter: {name}")
        if value is None:
            continue
        if name == "best_score_limit" and isinstance(value, list):
            value = tuple(value)
        current = getattr(config, name)
        if isinstance(value, str) and hasattr(type(current), value):
            value = getattr(type(current), value)
        try:
            setattr(config, name, value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid solver parameter {name!r}: {exc}") from exc
