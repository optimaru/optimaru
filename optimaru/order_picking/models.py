"""Pydantic request/response models for the order-picking solver.

JSON-friendly in/out (snake_case fields, PascalCase classes), same pattern as
``optimaru.vehicle_routing.models``:

    from optimaru.order_picking.models import (LocationModel, OrderModel,
                                      OrderPickingRequest, ProductModel,
                                      TrolleyModel, solve_request)

    request = OrderPickingRequest(
        time_limit_seconds=2,
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

The layout is part of the request, in one of two kinds:

- a grid of shelvings (the default). A location is a `LocationModel`: the column
  letters (A, B, ..., Z, AA, ... as spreadsheets write columns) + the shelving row
  1.. (the id "(A,1)"), a side (LEFT/RIGHT) and a row 0..shelving_height within the
  shelving. `warehouse` (`WarehouseModel`) gives the size of the grid; without it,
  5 columns (A..E) x 3 shelving rows of shelvings 2 m wide and 10 m tall, 3 m apart.
- named locations with a walking-distance table: `locations` lists the ids and
  `distances` the meters from one to another, and every `location` of a trolley or
  an order item is then one of those ids::

      OrderPickingRequest(
          locations=["dock", "aisle-1", "aisle-2"],
          distances={"dock": {"aisle-1": 12, "aisle-2": 20}, "aisle-1": {"aisle-2": 9}},
          trolleys=[TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000, location="dock")],
          orders=[OrderModel(id="order1", items=[ProductModel(id="p0", volume=1232, location="aisle-2")])],
      )

One request uses one kind: a grid location in a request with `locations`, or an id in
one without, is refused.
"""

from typing import Any, Literal

from pydantic import Field, model_serializer, model_validator

from .. import _api
from .._constraint import ParameterUse
from .._models import SolveRequest, StrictModel, apply_solver_parameters
from .._solve import check_parameters
from . import _core
from .constraints import get_constraints

__all__ = [
    "LocationModel",
    "OrderModel",
    "OrderPickingRequest",
    "OrderPickingResponse",
    "PickStop",
    "ProductModel",
    "TrolleyModel",
    "WarehouseModel",
    "parameter_uses",
    "solve_request",
    "to_solution",
    "with_initial_solution",
]


def _pick_task_id(order_id: str, item_position: int) -> str:
    return f"{order_id}-{item_position}"


#: The limits of a layout: the compiled core's (`Warehouse` in op/model/domain.hpp).
_MAX_COLUMN_COUNT: int = _core.Warehouse.MAX_COLUMN_COUNT  # 18278: A..ZZZ
_MAX_SHELVING_ROW_COUNT: int = _core.Warehouse.MAX_SHELVING_ROW_COUNT  # 10000
_MAX_SHELVING_SIZE: int = _core.Warehouse.MAX_SHELVING_SIZE  # 1000: width, height and padding, meters
_MAX_DISTANCE: int = _core.Warehouse.MAX_DISTANCE  # 1000000 meters


def _column_index(letters: str) -> int:
    """The 0-based column of its letters, read as a spreadsheet column: A = 0, Z = 25, AA = 26."""
    index = 0
    for letter in letters:
        index = index * 26 + ord(letter) - ord("A") + 1
    return index - 1


def _column_letters(index: int) -> str:
    """The letters of a 0-based column: 0 = A, 25 = Z, 26 = AA."""
    letters = ""
    while index >= 0:
        letters = chr(ord("A") + index % 26) + letters
        index = index // 26 - 1
    return letters


class LocationModel(StrictModel):
    """A location of a grid warehouse: shelving (column, row), side and in-shelving row.

    Whether it is inside the grid is the request's to check, which knows the grid's size
    (`OrderPickingRequest.warehouse`; by default columns A..E, shelving rows 1..3, rows 0..10).
    """

    column: str = Field(
        pattern=r"^[A-Z]{1,3}$",
        description="The shelving column, in letters as spreadsheets write columns: A, B, ..., Z, AA, AB, ...",
    )
    shelving_row: int = Field(ge=1, description="The shelving row, from 1.")
    side: Literal["LEFT", "RIGHT"]
    row: int = Field(ge=0, description="Meters from the top edge of the shelving, 0..shelving_height.")

    def to_core(self) -> "_core.WarehouseLocation":
        return _core.WarehouseLocation(
            column=_column_index(self.column),
            shelving_row=self.shelving_row - 1,
            side=_core.Side.LEFT if self.side == "LEFT" else _core.Side.RIGHT,
            row=self.row,
        )

    @classmethod
    def from_core(cls, location: "_core.WarehouseLocation") -> "LocationModel":
        return cls(
            column=_column_letters(location.column),
            shelving_row=location.shelving_row + 1,
            side="LEFT" if location.side == _core.Side.LEFT else "RIGHT",
            row=location.row,
        )


class WarehouseModel(StrictModel):
    """The size of a grid warehouse. Every field defaults to the grid a request without one gets.

    `column_count` columns (A, B, ...) by `shelving_row_count` rows (1, 2, ...) of shelvings, each
    `shelving_width` wide and `shelving_height` tall, `shelving_padding` meters of corridor between
    neighbors, all in whole meters. A shelving side holds the rows 0..`shelving_height`.
    """

    column_count: int = Field(default=5, ge=1, le=_MAX_COLUMN_COUNT)
    shelving_row_count: int = Field(default=3, ge=1, le=_MAX_SHELVING_ROW_COUNT)
    shelving_width: int = Field(default=2, ge=1, le=_MAX_SHELVING_SIZE, description="meters")
    shelving_height: int = Field(
        default=10, ge=1, le=_MAX_SHELVING_SIZE, description="meters; also the last row of a shelving side"
    )
    shelving_padding: int = Field(default=3, ge=1, le=_MAX_SHELVING_SIZE, description="meters")

    def to_core(self) -> "_core.Warehouse":
        return _core.Warehouse(
            column_count=self.column_count,
            shelving_row_count=self.shelving_row_count,
            shelving_width=self.shelving_width,
            shelving_height=self.shelving_height,
            shelving_padding=self.shelving_padding,
        )


#: Where a trolley parks or a product is stored: a grid location, or the id of a named location.
Location = LocationModel | str


class ProductModel(StrictModel):
    """One order item: an indivisible product at a warehouse location."""

    id: str
    volume: int = Field(gt=0, description="cm3")
    location: Location = Field(
        description="A grid location, or the id of one of the request's `locations` when it has them."
    )
    name: str | None = None


class OrderModel(StrictModel):
    id: str
    items: list[ProductModel] = Field(min_length=1)


class TrolleyModel(StrictModel):
    id: str
    bucket_count: int = Field(ge=0)
    bucket_capacity: int = Field(gt=0, description="cm3")
    location: Location = Field(
        description=(
            "Where the trolley's walk starts and ends: a grid location, or the id of one of the "
            "request's `locations` when it has them."
        )
    )
    initial_route: list[str] = Field(
        default_factory=list,
        description=(
            'Pick task ids ("<order>-<item position>") this trolley\'s route starts '
            "from. The solve keeps them as its starting point instead of constructing "
            "them, and places only what no route holds."
        ),
    )


def _distance_rows(locations: list[str], distances: dict[str, dict[str, int]]) -> list[list[int]]:
    """The square table of `distances` over `locations`, row = from, column = to.

    A location is 0 meters from itself. A pair given one way only is as long the other way; a
    pair given neither way is refused (ValueError), as is an id that is not in `locations`, a
    distance outside 0..1,000,000 and a distance from a location to itself other than 0.
    """
    known = set(locations)
    for start, row in distances.items():
        if start not in known:
            raise ValueError(f"distances: unknown location {start!r}")
        for end, meters in row.items():
            if end not in known:
                raise ValueError(f"distances from {start!r}: unknown location {end!r}")
            if not 0 <= meters <= _MAX_DISTANCE:
                raise ValueError(f"distance from {start!r} to {end!r} must be 0..{_MAX_DISTANCE}, not {meters}")
            if start == end and meters != 0:
                raise ValueError(f"distance from {start!r} to itself must be 0, not {meters}")
    empty: dict[str, int] = {}
    rows = []
    missing = []
    for start in locations:
        given = distances.get(start, empty)
        row = []
        for end in locations:
            meters = 0 if start == end else given.get(end)
            if meters is None:
                meters = distances.get(end, empty).get(start)
            if meters is None:
                missing.append((start, end))
                meters = 0
            row.append(meters)
        rows.append(row)
    if missing:
        pairs = sorted({tuple(sorted(pair)) for pair in missing})
        shown = ", ".join(f"{a!r} and {b!r}" for a, b in pairs[:5])
        more = f" (and {len(pairs) - 5} more pairs)" if len(pairs) > 5 else ""
        raise ValueError(f"distances: no distance between {shown}{more}")
    return rows


class OrderPickingRequest(SolveRequest):
    """The trolleys and orders with the settings of one solve (see `SolveRequest` for those).

    The layout is a grid unless `locations` and `distances` are given (see the module docstring):
    `warehouse` sizes the grid, and a request without it has the default one.
    """

    warehouse: WarehouseModel | None = Field(
        default=None,
        description=(
            "The size of the grid the locations are in. Omitted: 5 columns (A..E) x 3 shelving rows, "
            "shelvings 2 m x 10 m, 3 m apart. Not with `locations`."
        ),
    )
    locations: list[str] | None = Field(
        default=None,
        min_length=1,
        description=(
            "Ids of the named locations of a warehouse that is not a grid. With them, `distances` is "
            "required, and every `location` of a trolley or an order item is one of these ids."
        ),
    )
    distances: dict[str, dict[str, int]] | None = Field(
        default=None,
        description=(
            "Walking distances between the `locations`, whole meters: {from id: {to id: meters}}. "
            "A location is 0 from itself. The table may be asymmetric; a pair given one way only is "
            "as long the other way, and a pair of `locations` given neither way is refused."
        ),
    )
    trolleys: list[TrolleyModel] = Field(min_length=1)
    orders: list[OrderModel] = Field(min_length=1)

    @model_serializer(mode="wrap")
    def _without_a_layout_left_out(self, handler):
        """A request that gives no layout dumps without the three layout fields, as it did
        before they existed: what it sends to the optimaru API is then what an API that does
        not know them yet reads."""
        data = handler(self)
        for name in ("warehouse", "locations", "distances"):
            if data.get(name) is None:
                data.pop(name, None)
        return data

    @model_validator(mode="after")
    def _one_layout(self):
        places = [(f"trolley {t.id}", t.location) for t in self.trolleys]
        places += [(f"order {o.id}, item {item.id}", item.location) for o in self.orders for item in o.items]
        if self.locations is None:
            if self.distances is not None:
                raise ValueError("distances needs locations: the ids of the places it measures between")
            grid = self.warehouse or WarehouseModel()
            last_column = _column_letters(grid.column_count - 1)
            for owner, place in places:
                if isinstance(place, str):
                    raise ValueError(
                        f"{owner}: location {place!r} is an id, and the request has no locations. A grid "
                        "location is {column, shelving_row, side, row}; a request uses one kind of layout"
                    )
                if _column_index(place.column) >= grid.column_count:
                    raise ValueError(
                        f"{owner}: column {place.column!r} is outside the warehouse's columns A..{last_column}"
                    )
                if place.shelving_row > grid.shelving_row_count:
                    raise ValueError(
                        f"{owner}: shelving_row {place.shelving_row} is outside the warehouse's shelving rows "
                        f"1..{grid.shelving_row_count}"
                    )
                if place.row > grid.shelving_height:
                    raise ValueError(
                        f"{owner}: row {place.row} is outside a shelving side's rows 0..{grid.shelving_height}"
                    )
            return self
        if self.warehouse is not None:
            raise ValueError(
                "warehouse sizes a grid and locations names the places of a warehouse that is not one: "
                "a request uses one kind of layout"
            )
        if self.distances is None:
            raise ValueError("locations needs distances: the meters between them")
        if len(self.locations) != len(set(self.locations)):
            raise ValueError("duplicate location ids")
        known = set(self.locations)
        for owner, place in places:
            if not isinstance(place, str):
                raise ValueError(
                    f"{owner}: a grid location in a request with locations. Its location is one of their "
                    "ids; a request uses one kind of layout"
                )
            if place not in known:
                raise ValueError(f"{owner}: unknown location {place!r}")
        _distance_rows(self.locations, self.distances)
        return self

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


class PickStop(StrictModel):
    pick_task_id: str
    order_id: str
    product_name: str
    volume: int
    location: Location  # as the request gave it: a grid location, or the id of a named one


class OrderPickingResponse(StrictModel):
    routes: dict[str, list[PickStop]]  # trolley id -> ordered stops
    distance_by_trolley: dict[str, int]  # meters, incl. the return leg
    unassigned: list[str] = Field(default_factory=list)  # pick task ids
    score: str
    feasible: bool
    total_distance: int  # meters
    order_split_count: int  # (order, trolley) pairs = sum over orders of the distinct trolleys carrying it


def _to_warehouse(request: OrderPickingRequest) -> "_core.Warehouse":
    """The compiled layout of the request: its named locations, or its grid (the default one
    without `warehouse`).

    The table is laid out again here, after the request's validator did it once to check it:
    a request can be changed after it was validated, so the rows are not kept on it."""
    if request.locations is not None:
        return _core.Warehouse.from_distances(
            request.locations, _distance_rows(request.locations, request.distances or {})
        )
    return (request.warehouse or WarehouseModel()).to_core()


def to_solution(request: OrderPickingRequest) -> "_core.OrderPickingSolution":
    """Build the C++ OrderPickingSolution from the pydantic request: its warehouse, one
    Order per OrderModel and one PickTask per order item (ids "<order>-<i>")."""
    warehouse = _to_warehouse(request)
    named = {name: warehouse.location(name) for name in request.locations or ()}

    def place(location: Location) -> "_core.WarehouseLocation":
        return named[location] if isinstance(location, str) else location.to_core()

    orders = [_core.Order(o.id) for o in request.orders]
    pick_tasks = []
    for order_index, order in enumerate(request.orders):
        for i, item in enumerate(order.items):
            pick_tasks.append(
                _core.PickTask(
                    id=_pick_task_id(order.id, i),
                    order_index=order_index,
                    volume=item.volume,
                    location=place(item.location),
                    product_name=item.name or item.id,
                )
            )
    trolleys = [_core.Trolley(t.id, t.bucket_count, t.bucket_capacity, place(t.location)) for t in request.trolleys]
    task_index = {task.id: i for i, task in enumerate(pick_tasks)}
    for trolley, model in zip(trolleys, request.trolleys):
        trolley.pick_task_indices = [task_index[task_id] for task_id in model.initial_route]
    return _core.OrderPickingSolution(orders, trolleys, pick_tasks, warehouse)


def parameter_uses(request: OrderPickingRequest) -> list[ParameterUse]:
    """The parameters `request` sets away from their defaults that act only through a cost.

    None here: every field that a cost reads is required (bucket count and capacity,
    volumes, locations), so none has a default to leave it off, and `initial_route` is a
    starting point. The layout (`warehouse`, or `locations` and `distances`) is not one
    either: the response reports the meters walked from it whatever the constraints. Kept so
    `solve_request` checks this use case as it checks the others; the table is "Parameters
    and their costs" in docs/use-cases/README.md."""
    return []


@_api.request_solve("order-picking", "OrderPickingResponse")
def solve_request(request: OrderPickingRequest, *, constraints=None) -> OrderPickingResponse:
    """Solve the request and shape the response (routes per trolley plus the
    per-trolley travel distance).

    A parameter set away from its default whose costs the constraints leave out would be
    refused with ValueError (`parameter_uses`, empty for this use case)."""
    constraints = check_parameters(parameter_uses(request), constraints, get_constraints)
    solution = to_solution(request)
    config = _core.SolverConfig()
    config.stop.time_limit_seconds = request.time_limit_seconds
    config.run.random_seed = request.random_seed
    config.run.parallel_solver_count = request.parallel_solver_count
    config.run.full_assert = request.full_assert
    apply_solver_parameters(config, request.solver_parameters)
    if request.stop_at_first_feasible:
        config.stop.target_score = (0, float("-inf"))
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
                    location=(
                        LocationModel.from_core(task.location)
                        if solution.warehouse.is_grid
                        else solution.warehouse.location_id(task.location)
                    ),
                )
            )
        routes[trolley.id] = stops
        distance_by_trolley[trolley.id] = _core.distance_to_travel(solution, trolley_index)

    # The raw per-trolley costs of the default constraints (the only
    # per-constraint surface): the travel distance is the outbound legs plus
    # the walks back, the split count the (order, trolley) pairs.
    raw = _core.constraint_costs(solution)
    return OrderPickingResponse(
        routes=routes,
        distance_by_trolley=distance_by_trolley,
        unassigned=[task.id for i, task in enumerate(solution.pick_tasks) if i not in assigned],
        score=str(solution.score),
        feasible=solution.score.is_feasible(),
        total_distance=_raw_total(raw, "distance_from_previous_pick_task_cost")
        + _raw_total(raw, "distance_from_last_pick_task_to_path_origin_cost"),
        order_split_count=_raw_total(raw, "order_split_by_trolley_cost"),
    )


def _raw_total(raw: dict[str, dict[str, Any]], name: str) -> int:
    """One constraint's raw cost summed over the trolleys (integral here)."""
    return int(sum(raw[name]["costs"]))


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
