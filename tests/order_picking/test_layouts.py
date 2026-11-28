"""Order picking on a warehouse that is not the default one: a grid of another size, and named
locations with a walking-distance table."""

import json

import pytest

import optimaru.order_picking as op
from optimaru.order_picking.constraints import get_constraints
from optimaru.order_picking.models import (
    LocationModel,
    OrderModel,
    OrderPickingRequest,
    OrderPickingResponse,
    ProductModel,
    TrolleyModel,
    WarehouseModel,
    solve_request,
    to_solution,
    with_initial_solution,
)

PATHS = [pytest.param(lambda: None, id="default"), pytest.param(get_constraints, id="constraints")]

# ---------------------------------------------------------------------------
# A grid of another size: 30 columns (A..AD) x 2 shelving rows, shelvings 4 m wide and 6 m
# tall, 2 m apart. A shelving is 6 m from the next one to the right and 8 m from the one below.
# ---------------------------------------------------------------------------

GRID = WarehouseModel(column_count=30, shelving_row_count=2, shelving_width=4, shelving_height=6, shelving_padding=2)
START = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)  # x 0, y 0
# The walk of GRID_ROUTE, by hand:
#   start -> (AB,2) RIGHT row 3   x 27*6+4 = 166, y 8+3 = 11; another shelving row: 166 + 11  = 177
#   -> (AB,2) LEFT row 5          the other side of the shelving: 4 + min(3+5, 3+1)           =   8
#   -> (AA,2) RIGHT row 6         x 26*6+4 = 160 faces x 162 across the corridor: 2 + |13-14| =   3
#   -> start                      another shelving row: 160 + 14                              = 174
GRID_STOPS = [
    LocationModel(column="AB", shelving_row=2, side="RIGHT", row=3),
    LocationModel(column="AB", shelving_row=2, side="LEFT", row=5),
    LocationModel(column="AA", shelving_row=2, side="RIGHT", row=6),
]
GRID_LEGS = [177, 8, 3, 174]
GRID_ROUTE = ["order-0", "order-1", "order-2"]


def grid_request(**settings) -> OrderPickingRequest:
    items = [ProductModel(id=f"p{i}", volume=1000, location=place) for i, place in enumerate(GRID_STOPS)]
    return OrderPickingRequest(
        warehouse=GRID,
        trolleys=[TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000, location=START)],
        orders=[OrderModel(id="order", items=items)],
        **settings,
    )


def test_a_grid_walk_by_hand():
    warehouse = GRID.to_core()
    places = [START.to_core()] + [stop.to_core() for stop in GRID_STOPS] + [START.to_core()]
    legs = [op.calculate_distance(a, b, warehouse) for a, b in zip(places, places[1:])]
    assert legs == GRID_LEGS
    assert [op.calculate_distance(b, a, warehouse) for a, b in zip(places, places[1:])] == GRID_LEGS

    solution = to_solution(grid_request())
    solution.trolleys[0].pick_task_indices = [0, 1, 2]
    assert op.distance_to_travel(solution, 0) == sum(GRID_LEGS) == 362
    assert op.calculate_score(solution).soft == -(362 + 1000)
    raw = op.constraint_costs(solution)
    assert raw["distance_from_previous_pick_task_cost"]["costs"] == [177 + 8 + 3]
    assert raw["distance_from_last_pick_task_to_path_origin_cost"]["costs"] == [174]


@pytest.mark.parametrize("constraints", PATHS)
def test_a_grid_request_solves_and_reports_its_locations(constraints):
    solved = solve_request(
        grid_request(time_limit_seconds=0.3, random_seed=1, full_assert=True), constraints=constraints()
    )

    assert solved.feasible and not solved.unassigned
    # Three stops and the start: both directions of the one best cycle walk 362 m.
    assert solved.total_distance == solved.distance_by_trolley["1"] == 362
    assert solved.score == "0hard/-1362soft"
    assert sorted(stop.location.column for stop in solved.routes["1"]) == ["AA", "AB", "AB"]


def test_column_letters_are_spreadsheet_columns():
    def column_of(letters):
        return LocationModel(column=letters, shelving_row=1, side="LEFT", row=0).to_core().column

    assert [column_of(letters) for letters in ("A", "E", "Z", "AA", "AZ", "BA", "ZZ", "AAA", "ZZZ")] == [
        0, 4, 25, 26, 51, 52, 701, 702, 18277,
    ]  # fmt: skip
    for column in (0, 4, 25, 26, 51, 52, 701, 702, 18277):
        place = op.WarehouseLocation(column, 0, op.Side.LEFT, 0)
        assert LocationModel.from_core(place).to_core().column == column
    assert op.WarehouseLocation(26, 1, op.Side.LEFT, 0).shelving_id == "(AA,2)"
    assert op.WarehouseLocation(0, 0, op.Side.LEFT, 0).shelving_id == "(A,1)"
    for bad in ("a", "A1", "", "AAAA", "1"):
        with pytest.raises(ValueError):
            LocationModel(column=bad, shelving_row=1, side="LEFT", row=0)


def test_the_default_warehouse_is_the_grid_requests_always_had():
    default = op.Warehouse()
    assert default.is_grid and default.location_ids == []
    assert (default.column_count, default.shelving_row_count) == (5, 3)
    assert (default.shelving_width, default.shelving_height, default.shelving_padding) == (2, 10, 3)
    assert WarehouseModel().model_dump() == {
        "column_count": 5,
        "shelving_row_count": 3,
        "shelving_width": 2,
        "shelving_height": 10,
        "shelving_padding": 3,
    }
    demo = op.generate_demo_data()
    assert demo.warehouse.is_grid and demo.warehouse.column_count == 5

    # A request that spells the default out solves as one that leaves it out.
    def request(**layout):
        spots = [("B", 2, 3), ("C", 3, 1), ("D", 1, 9), ("D", 2, 4), ("E", 1, 10), ("A", 3, 0), ("E", 3, 7)]
        items = [
            ProductModel(
                id=f"p{i}",
                volume=1000 + i,
                location=LocationModel(column=column, shelving_row=shelf, side="LEFT" if i % 2 else "RIGHT", row=row),
            )
            for i, (column, shelf, row) in enumerate(spots)
        ]
        origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
        return OrderPickingRequest(
            random_seed=3,
            solver_parameters={"stop": {"step_limit": 2000}},
            trolleys=[TrolleyModel(id=t, bucket_count=1, bucket_capacity=5000, location=origin) for t in "12"],
            orders=[OrderModel(id="first", items=items[:4]), OrderModel(id="second", items=items[4:])],
            **layout,
        )

    assert solve_request(request(warehouse=WarehouseModel())) == solve_request(request())

    # And it is sent as it always was: without the layout fields it leaves out.
    for dumped in (request().model_dump(), request().model_dump(mode="json")):
        assert not {"warehouse", "locations", "distances"} & dumped.keys()
    assert "warehouse" in request(warehouse=WarehouseModel()).model_dump()
    assert {"locations", "distances"} <= table_request().model_dump(mode="json").keys()
    assert "warehouse" not in table_request().model_dump()


def test_a_seeded_demo_solve_is_the_same_with_the_warehouse_given():
    """The solve reads its distances from a table over the locations in use; the result is the
    one the grid arithmetic gives (the final score is recomputed without the table)."""

    def run(solution):
        config = op.SolverConfig()
        config.stop.step_limit = 20000
        config.run.random_seed = 5
        return op.solve(solution, config=config)

    demo = run(op.generate_demo_data())
    plain = op.generate_demo_data()
    explicit = run(op.OrderPickingSolution(plain.orders, plain.trolleys, plain.pick_tasks, op.Warehouse()))

    assert str(explicit.score) == str(demo.score)
    assert explicit.routes() == demo.routes()
    assert str(op.calculate_score(demo)) == str(demo.score)
    assert all(task.location.index == -1 for task in demo.pick_tasks)


@pytest.mark.parametrize(
    "change, message",
    [
        ({"warehouse": None}, "order order, item p0: column 'AB' is outside the warehouse's columns A..E"),
        ({"warehouse": GRID.model_copy(update={"column_count": 27})}, "item p0: column 'AB' is outside .* A..AA"),
        (
            {"warehouse": GRID.model_copy(update={"shelving_row_count": 1})},
            "item p0: shelving_row 2 is outside .* 1..1",
        ),
        ({"warehouse": GRID.model_copy(update={"shelving_height": 5})}, "item p2: row 6 is outside .* 0..5"),
        ({"distances": {"a": {"b": 1}}}, "distances needs locations"),
        ({"locations": ["a"]}, "a request uses one kind of layout"),
    ],
)
def test_a_grid_request_outside_its_grid_is_refused(change, message):
    data = grid_request().model_dump()
    data.update({key: value.model_dump() if hasattr(value, "model_dump") else value for key, value in change.items()})
    with pytest.raises(ValueError, match=message):
        OrderPickingRequest.model_validate(data)


@pytest.mark.parametrize(
    "field, value",
    [
        ("column_count", 0),
        ("column_count", 18279),
        ("shelving_row_count", 0),
        ("shelving_row_count", 10001),
        ("shelving_width", 0),
        ("shelving_height", 0),
        ("shelving_height", 1001),
        ("shelving_padding", 0),
    ],
)
def test_a_grid_size_out_of_range_is_refused(field, value):
    with pytest.raises(ValueError):
        WarehouseModel(**{field: value})
    with pytest.raises(ValueError, match=field):
        op.Warehouse(**{field: value})


def test_a_solution_checks_its_locations_against_its_warehouse():
    start = START.to_core()
    far = op.WarehouseLocation(27, 1, op.Side.RIGHT, 3)

    def solution_in(warehouse):
        return op.OrderPickingSolution(
            [op.Order("o")], [op.Trolley("1", 1, 10, start)], [op.PickTask("p", 0, 1, far)], warehouse
        )

    solution_in(GRID.to_core())
    with pytest.raises(ValueError, match="pick task 'p': column 27 is outside the warehouse's columns 0..4 \\(A..E\\)"):
        solution_in(None)

    # A location swapped in afterwards is caught by the solve.
    solution = solution_in(GRID.to_core())
    solution.trolleys = [op.Trolley("1", 1, 10, op.WarehouseLocation(40, 0, op.Side.LEFT, 0))]
    with pytest.raises(ValueError, match="trolley '1': column 40 is outside"):
        op.solve(solution, time_limit_seconds=0.1)


# ---------------------------------------------------------------------------
# Named locations with a distance table.
# ---------------------------------------------------------------------------

LOCATIONS = ["dock", "a", "b", "c"]
# One way where the other is missing (a -> dock is dock -> a), two ways where they differ (a <-> b).
DISTANCES = {
    "dock": {"a": 5, "b": 9, "c": 10},
    "a": {"b": 7, "c": 4},
    "b": {"a": 3, "c": 2},
    "c": {"dock": 11},
}
# dock -> a -> b -> c -> dock = 5 + 7 + 2 + 11 = 25. The six orders of the three stops walk
# 25, 20 (a c b: 5 + 4 + 2 + 9), 27 (b a c: 9 + 3 + 4 + 11), 20 (b c a: 9 + 2 + 4 + 5),
# 30 (c a b: 10 + 4 + 7 + 9) and 20 (c b a: 10 + 2 + 3 + 5).
TABLE_ROUTE = ["order-0", "order-1", "order-2"]


def table_request(**settings) -> OrderPickingRequest:
    items = [ProductModel(id=f"p{i}", volume=1000, location=place) for i, place in enumerate(["a", "b", "c"])]
    fields = {"locations": LOCATIONS, "distances": DISTANCES} | settings
    return OrderPickingRequest(
        trolleys=[TrolleyModel(id="1", bucket_count=4, bucket_capacity=48000, location="dock")],
        orders=[OrderModel(id="order", items=items)],
        **fields,
    )


def test_a_table_walk_by_hand():
    solution = to_solution(table_request())
    warehouse = solution.warehouse
    assert not warehouse.is_grid and warehouse.location_ids == LOCATIONS
    dock, a, b, c = (warehouse.location(name) for name in LOCATIONS)
    assert [place.index for place in (dock, a, b, c)] == [0, 1, 2, 3]
    assert warehouse.location_id(b) == "b"

    def meters(start, end):
        return op.calculate_distance(start, end, warehouse)

    assert (meters(dock, a), meters(a, dock)) == (5, 5)  # given one way
    assert (meters(a, b), meters(b, a)) == (7, 3)  # given both ways
    assert (meters(dock, c), meters(c, dock)) == (10, 11)
    assert meters(b, b) == 0

    for order, walked in (([0, 1, 2], 25), ([0, 2, 1], 20), ([1, 0, 2], 27), ([1, 2, 0], 20), ([2, 0, 1], 30)):
        solution.trolleys[0].pick_task_indices = order
        assert op.distance_to_travel(solution, 0) == walked
        assert op.calculate_score(solution).soft == -(walked + 1000)
    solution.trolleys[0].pick_task_indices = [0, 1, 2]
    raw = op.constraint_costs(solution)
    assert raw["distance_from_previous_pick_task_cost"]["costs"] == [5 + 7 + 2]
    assert raw["distance_from_last_pick_task_to_path_origin_cost"]["costs"] == [11]


@pytest.mark.parametrize("constraints", PATHS)
def test_a_table_request_solves_to_the_shortest_walk(constraints):
    solved = solve_request(
        table_request(time_limit_seconds=0.3, random_seed=1, full_assert=True), constraints=constraints()
    )

    assert solved.feasible and not solved.unassigned
    assert solved.total_distance == solved.distance_by_trolley["1"] == 20
    assert solved.score == "0hard/-1020soft"
    assert sorted(stop.location for stop in solved.routes["1"]) == ["a", "b", "c"]


def test_a_larger_table_request_solves_on_every_path():
    """Twelve places on a line, 3 m apart, the walk between two of them their gap: several
    trolleys, parallel starts, the order-level moves and the plugin path all read the table."""
    names = [f"bay-{i:02d}" for i in range(12)]
    distances = {a: {b: 3 * (j - i) for j, b in enumerate(names) if j > i} for i, a in enumerate(names)}
    orders = [
        OrderModel(
            id=f"o{o}",
            items=[
                ProductModel(id=f"o{o}-{i}", volume=900, location=names[1 + (o * 3 + i * 5) % 11]) for i in range(4)
            ],
        )
        for o in range(5)
    ]

    def request(**settings):
        return OrderPickingRequest(
            locations=names,
            distances=distances,
            trolleys=[
                TrolleyModel(id=str(t), bucket_count=2, bucket_capacity=4000, location=names[0]) for t in range(3)
            ],
            orders=orders,
            random_seed=2,
            full_assert=True,
            **settings,
        )

    for settings, constraints in (
        ({"time_limit_seconds": 0.3}, None),
        ({"time_limit_seconds": 0.3, "parallel_solver_count": 2}, None),
        ({"time_limit_seconds": 0.3}, get_constraints()),
    ):
        solved = solve_request(request(**settings), constraints=constraints)
        assert solved.feasible and not solved.unassigned
        # A trolley walking out to its farthest bay and back walks twice that far at least.
        for trolley, stops in solved.routes.items():
            farthest = max((names.index(stop.location) for stop in stops), default=0)
            assert solved.distance_by_trolley[trolley] >= 2 * 3 * farthest
        assert solved.total_distance == sum(solved.distance_by_trolley.values())

    # Seeded and step-limited, it is the same solve twice.
    limited = {"solver_parameters": {"stop": {"step_limit": 3000}}}
    assert solve_request(request(**limited)) == solve_request(request(**limited))


@pytest.mark.parametrize(
    "change, message",
    [
        ({"distances": None}, "locations needs distances"),
        ({"warehouse": {}}, "a request uses one kind of layout"),
        ({"locations": ["dock", "a", "b", "c", "a"]}, "duplicate location ids"),
        ({"locations": ["dock", "a", "b"]}, "order order, item p2: unknown location 'c'"),
        ({"locations": LOCATIONS + ["d"]}, "no distance between 'a' and 'd', 'b' and 'd', 'c' and 'd', 'd' and 'dock'"),
        ({"distances": DISTANCES | {"nowhere": {"a": 1}}}, "distances: unknown location 'nowhere'"),
        ({"distances": DISTANCES | {"c": {"dock": 11, "x": 1}}}, "distances from 'c': unknown location 'x'"),
        ({"distances": DISTANCES | {"c": {"dock": -1}}}, "distance from 'c' to 'dock' must be 0..1000000, not -1"),
        ({"distances": DISTANCES | {"c": {"dock": 1000001}}}, "must be 0..1000000"),
        ({"distances": DISTANCES | {"c": {"dock": 11, "c": 2}}}, "distance from 'c' to itself must be 0"),
        ({"distances": DISTANCES | {"c": {"dock": 1.5}}}, "valid integer"),
        ({"locations": []}, "at least 1 item"),
    ],
)
def test_a_bad_table_is_refused(change, message):
    data = table_request().model_dump() | change
    with pytest.raises(ValueError, match=message):
        OrderPickingRequest.model_validate(data)


def test_a_request_uses_one_kind_of_layout():
    data = table_request().model_dump()
    data["trolleys"][0]["location"] = START.model_dump()
    with pytest.raises(ValueError, match="trolley 1: a grid location in a request with locations"):
        OrderPickingRequest.model_validate(data)

    data = grid_request().model_dump()
    data["orders"][0]["items"][1]["location"] = "a"
    with pytest.raises(
        ValueError, match="order order, item p1: location 'a' is an id, and the request has no locations"
    ):
        OrderPickingRequest.model_validate(data)


def test_the_compiled_warehouse_of_named_locations_checks_its_table():
    with pytest.raises(ValueError, match="no locations"):
        op.Warehouse.from_distances([], [])
    with pytest.raises(ValueError, match="duplicate location id 'a'"):
        op.Warehouse.from_distances(["a", "a"], [[0, 1], [1, 0]])
    with pytest.raises(ValueError, match="2 locations need 2 rows"):
        op.Warehouse.from_distances(["a", "b"], [[0, 1]])
    with pytest.raises(ValueError, match="one value per location"):
        op.Warehouse.from_distances(["a", "b"], [[0, 1], [1]])
    with pytest.raises(ValueError, match="from 'a' to 'b' must be 0..1000000"):
        op.Warehouse.from_distances(["a", "b"], [[0, -1], [1, 0]])
    with pytest.raises(ValueError, match="'b' to itself must be 0"):
        op.Warehouse.from_distances(["a", "b"], [[0, 1], [1, 4]])

    warehouse = op.Warehouse.from_distances(["a", "b"], [[0, 6], [2, 0]])
    a, b = warehouse.location("a"), warehouse.location("b")
    assert (op.calculate_distance(a, b, warehouse), op.calculate_distance(b, a, warehouse)) == (6, 2)
    with pytest.raises(ValueError, match="unknown location 'z'"):
        warehouse.location("z")
    with pytest.raises(ValueError, match="no named locations"):
        op.Warehouse().location("a")
    with pytest.raises(ValueError, match="needs its warehouse"):
        op.calculate_distance(a, b)
    grid_place = op.WarehouseLocation(0, 0, op.Side.LEFT, 0)
    with pytest.raises(ValueError, match="not a location of this warehouse"):
        op.calculate_distance(a, grid_place, warehouse)

    # One solution, one kind of layout.
    with pytest.raises(ValueError, match="pick task 'p': its location is not one of the warehouse's 2 named"):
        op.OrderPickingSolution(
            [op.Order("o")], [op.Trolley("1", 1, 10, a)], [op.PickTask("p", 0, 1, grid_place)], warehouse
        )
    with pytest.raises(ValueError, match="trolley '1': a named location in a grid warehouse"):
        op.OrderPickingSolution([op.Order("o")], [op.Trolley("1", 1, 10, a)], [op.PickTask("p", 0, 1, grid_place)])
    other = op.Warehouse.from_distances(["a"], [[0]])
    with pytest.raises(ValueError, match="pick task 'p'"):
        op.OrderPickingSolution([op.Order("o")], [op.Trolley("1", 1, 10, a)], [op.PickTask("p", 0, 1, b)], other)

    # The compiled solve on named locations, checked step by step.
    solution = op.OrderPickingSolution(
        [op.Order("o")],
        [op.Trolley("1", 1, 10, a)],
        [op.PickTask("p", 0, 1, b), op.PickTask("q", 0, 1, a)],
        warehouse,
    )
    op.solve(solution, time_limit_seconds=0.1, random_seed=0, full_assert=True)
    assert op.distance_to_travel(solution, 0) == 8
    assert str(solution.score) == "0hard/-1008soft"


# ---------------------------------------------------------------------------
# Both layouts: warm start and the JSON round trip.
# ---------------------------------------------------------------------------

LAYOUTS = [
    pytest.param(grid_request, GRID_ROUTE, 362, id="grid"),
    pytest.param(table_request, TABLE_ROUTE, 25, id="table"),
]


@pytest.mark.parametrize("request_of, route, walked", LAYOUTS)
@pytest.mark.parametrize("constraints", PATHS)
def test_a_seed_comes_back_unchanged_on_both_layouts(request_of, route, walked, constraints):
    """With no time to search, the routes given are the routes returned, at their own distance."""
    seeded = request_of(time_limit_seconds=0)
    seeded.trolleys[0].initial_route = route
    start = solve_request(OrderPickingRequest.model_validate(seeded.model_dump()), constraints=constraints())

    assert [stop.pick_task_id for stop in start.routes["1"]] == route
    assert start.total_distance == walked

    again = solve_request(with_initial_solution(request_of(time_limit_seconds=0), start), constraints=constraints())
    assert again == start

    improved = solve_request(
        with_initial_solution(request_of(time_limit_seconds=0.2, random_seed=0), start), constraints=constraints()
    )
    assert improved.total_distance <= walked


@pytest.mark.parametrize("request_of, route, walked", LAYOUTS)
def test_the_models_round_trip_as_json_on_both_layouts(request_of, route, walked):
    request = request_of(time_limit_seconds=0.1, random_seed=4)
    as_json = request.model_dump_json()
    clone = OrderPickingRequest.model_validate_json(as_json)
    assert clone == request
    assert OrderPickingRequest.model_validate(json.loads(as_json)) == request

    solved = solve_request(clone)
    back = OrderPickingResponse.model_validate_json(solved.model_dump_json())
    assert back == solved
    # A stop names its location as the request did.
    kinds = {type(stop.location) for stop in back.routes["1"]}
    assert kinds == ({str} if request.locations else {LocationModel})
    assert with_initial_solution(request, json.loads(solved.model_dump_json())).trolleys[0].initial_route == [
        stop.pick_task_id for stop in solved.routes["1"]
    ]
