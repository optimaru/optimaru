"""Correctness tests for the order_picking package.

Requires the compiled extension: uv run python setup.py build_ext --inplace
"""

import pytest

import order_picking as op
from order_picking import models


def test_request_solver_parameters_are_applied():
    config = op.SolverConfig()
    models._apply_solver_parameters(config, {"late_acceptance_size": 7, "ruin_recreate": False})
    assert config.late_acceptance_size == 7
    assert config.ruin_recreate is False
    with pytest.raises(ValueError, match="unknown solver parameter"):
        models._apply_solver_parameters(config, {"not_a_parameter": 1})


def location(column, shelving_row, side, row):
    return op.WarehouseLocation(
        column=ord(column) - ord("A"),
        shelving_row=shelving_row - 1,
        side=side,
        row=row,
    )


ORIGIN_ARGS = ("A", 1, op.Side.LEFT, 0)


def small_solution():
    """2 trolleys / 2 orders x 3 picks; capacities force nothing (feasible)."""
    origin = location(*ORIGIN_ARGS)
    orders = [op.Order("order1"), op.Order("order2")]
    picks = []
    spots = [("B", 2), ("C", 3), ("D", 1), ("D", 2), ("E", 1), ("A", 3)]
    for i, (col, srow) in enumerate(spots):
        picks.append(
            op.PickTask(
                id=f"p{i}",
                order_index=i % 2,
                volume=1000,
                location=location(col, srow, op.Side.RIGHT if i % 2 else op.Side.LEFT, (i * 3) % 10 + 1),
            )
        )
    trolleys = [op.Trolley("1", 4, 48000, origin), op.Trolley("2", 4, 48000, origin)]
    return op.OrderPickingSolution(orders, trolleys, picks)


# ---------------------------------------------------------------------------
# Warehouse distance + constraint spot checks (the Java test values).
# ---------------------------------------------------------------------------


def test_warehouse_distance_java_cases():
    cases = [
        (("B", 3, op.Side.LEFT, 1), ("B", 3, op.Side.RIGHT, 1), 4),
        (("B", 3, op.Side.LEFT, 1), ("B", 3, op.Side.LEFT, 10), 9),
        (("C", 2, op.Side.RIGHT, 1), ("D", 2, op.Side.LEFT, 3), 5),
        (("C", 3, op.Side.LEFT, 1), ("E", 3, op.Side.RIGHT, 3), 16),
        (("C", 1, op.Side.LEFT, 1), ("E", 3, op.Side.RIGHT, 3), 40),
        (("E", 1, op.Side.RIGHT, 3), ("C", 3, op.Side.RIGHT, 1), 34),
        (("D", 2, op.Side.LEFT, 0), ("A", 1, op.Side.LEFT, 0), 28),
    ]
    for start, end, expected in cases:
        assert op.calculate_distance(location(*start), location(*end)) == expected
        assert op.calculate_distance(location(*end), location(*start)) == expected


def test_required_number_of_buckets_java_case():
    """The Java requiredNumberOfBucketsWithPenalization case: penalizes by 5."""
    origin = location(*ORIGIN_ARGS)
    orders = [op.Order("order1"), op.Order("order2")]
    volumes = {
        0: [(0, 4), (0, 5), (1, 4), (1, 1)],  # trolley1: 2 buckets, capacity 5
        1: [(0, 9), (0, 8), (1, 8), (1, 6), (1, 10), (1, 9)],  # trolley2: 2 buckets, capacity 10
    }
    picks = []
    trolleys = [op.Trolley("1", 2, 5, origin), op.Trolley("2", 2, 10, origin)]
    assignment = {0: [], 1: []}
    for trolley_index, items in volumes.items():
        for order_index, volume in items:
            assignment[trolley_index].append(len(picks))
            picks.append(op.PickTask(f"p{len(picks)}", order_index, volume, origin))
    solution = op.OrderPickingSolution(orders, trolleys, picks)
    solution.trolleys[0].pick_task_indices = assignment[0]
    solution.trolleys[1].pick_task_indices = assignment[1]

    totals = op.calculate_totals(solution)
    assert totals.required_number_of_buckets == 5
    assert op.calculate_score(solution).hard == -5


def test_order_split_java_case():
    """The Java minimizeOrderSplitByTrolley case: (3 + 1) * 1000."""
    origin = location(*ORIGIN_ARGS)
    orders = [op.Order("order1"), op.Order("order2")]
    picks = [op.PickTask(f"p{i}", 0 if i < 4 else 1, 1, origin) for i in range(8)]
    trolleys = [op.Trolley(str(t), 4, 100, origin) for t in range(4)]
    solution = op.OrderPickingSolution(orders, trolleys, picks)
    solution.trolleys[0].pick_task_indices = [0, 1]
    solution.trolleys[1].pick_task_indices = [2]
    solution.trolleys[2].pick_task_indices = [3]
    solution.trolleys[3].pick_task_indices = [4, 5, 6, 7]

    totals = op.calculate_totals(solution)
    assert totals.order_split_by_trolley == 4000


def test_distance_constraints_and_shadows():
    solution = small_solution()
    solution.trolleys[0].pick_task_indices = [0, 2, 4]
    solution.trolleys[1].pick_task_indices = [1, 3]
    op.calculate_score(solution)

    # Shadow variables mirror the lists.
    assert solution.pick_tasks[0].trolley_index == 0
    assert solution.pick_tasks[0].previous_pick_task == -1
    assert solution.pick_tasks[2].previous_pick_task == 0
    assert solution.pick_tasks[4].next_pick_task == -1
    assert solution.pick_tasks[4].is_last()
    assert solution.pick_tasks[5].trolley_index == -1  # unassigned

    # The two distance constraints add up to the round trips.
    totals = op.calculate_totals(solution)
    round_trips = op.distance_to_travel(solution, 0) + op.distance_to_travel(solution, 1)
    assert totals.distance_from_previous_pick_task + totals.distance_from_last_pick_task_to_path_origin == round_trips


# ---------------------------------------------------------------------------
# Demo data + solving.
# ---------------------------------------------------------------------------


def test_demo_data_matches_java_generator():
    solution = op.build_demo_data()
    assert len(solution.trolleys) == 5
    assert len(solution.orders) == 8
    assert len(solution.pick_tasks) == 114  # java.util.Random(37) replay
    assert all(t.bucket_count == 4 and t.bucket_capacity == 48000 for t in solution.trolleys)
    assert all(t.location.shelving_id == "(A,1)" for t in solution.trolleys)
    with pytest.raises(ValueError):
        op.build_demo_data(bucket_capacity=100)  # smaller than the largest product


def test_solve_demo_reaches_feasible():
    solution = op.build_demo_data()
    op.solve(solution, time_limit_seconds=3.0, random_seed=0, stop_at_first_feasible=True)
    assert solution.score.is_feasible()
    assigned = [i for t in solution.trolleys for i in t.pick_task_indices]
    assert sorted(assigned) == list(range(len(solution.pick_tasks)))
    # The stored score matches a recompute.
    final = str(solution.score)
    assert str(op.calculate_score(solution)) == final


def test_parallel_solver_count_workers():
    """parallel_solver_count > 1 fans out to this use case's own worker
    threads rather than the engine's built-in multi-start, because the
    consolidation move operator binds to a per-worker calculator. Each worker
    must finish cleanly and the best solution must be adopted."""
    for workers in (2, 4):
        solution = op.build_demo_data()
        op.solve(
            solution,
            time_limit_seconds=2.0,
            random_seed=42,
            parallel_solver_count=workers,
            stop_at_first_feasible=True,
        )
        assert solution.score.is_feasible()
        assigned = [i for t in solution.trolleys for i in t.pick_task_indices]
        assert sorted(assigned) == list(range(len(solution.pick_tasks)))
        assert str(op.calculate_score(solution)) == str(solution.score)


def test_seeded_runs_are_reproducible():
    def run():
        config = op.SolverConfig()
        # Step-count termination (not wall clock) so both runs stop at the
        # exact same point of the same seeded trajectory.
        config.step_count_limit = 5000
        config.spent_limit_seconds = 30.0
        config.random_seed = 3
        return op.solve(op.build_demo_data(), config=config)

    a = run()
    b = run()
    assert str(a.score) == str(b.score)
    assert a.routes() == b.routes()


# ---------------------------------------------------------------------------
# Pydantic models: round trip + solve.
# ---------------------------------------------------------------------------


def test_pydantic_models_round_trip():
    models = pytest.importorskip("order_picking.models")

    request = models.OrderPickingRequest(
        timelimit=1.0,
        random_seed=1,
        num_workers=2,
        stop_at_first_feasible=True,
        trolleys=[
            models.TrolleyModel(
                id=str(t),
                bucket_count=4,
                bucket_capacity=48000,
                location=models.LocationModel(column="A", shelving_row=1, side="LEFT", row=0),
            )
            for t in (1, 2)
        ],
        orders=[
            models.OrderModel(
                id=f"order{o}",
                items=[
                    models.ProductModel(
                        id=f"p{o}-{i}",
                        name=f"Product {o}-{i}",
                        volume=1000 * (i + 1),
                        location=models.LocationModel(
                            column="BCDE"[i % 4],
                            shelving_row=1 + (i % 3),
                            side="LEFT" if i % 2 == 0 else "RIGHT",
                            row=1 + i % 10,
                        ),
                    )
                    for i in range(5)
                ],
            )
            for o in (1, 2)
        ],
    )
    clone = models.OrderPickingRequest.model_validate_json(request.model_dump_json())
    assert clone.num_workers == 2
    solution = models.solve_request(clone)

    assert solution.feasible and not solution.unassigned
    assert sum(len(stops) for stops in solution.routes.values()) == 10
    assert solution.total_distance > 0
    assert solution.order_split_count >= 2  # at least one trolley per order
    assert set(solution.distance_by_trolley) == {"1", "2"}
    round_tripped = models.OrderPickingResponse.model_validate_json(solution.model_dump_json())
    assert round_tripped == solution


def test_duplicate_ids_rejected():
    models = pytest.importorskip("order_picking.models")
    loc = models.LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    with pytest.raises(ValueError, match="duplicate"):
        models.OrderPickingRequest(
            trolleys=[
                models.TrolleyModel(id="1", bucket_count=1, bucket_capacity=10, location=loc),
                models.TrolleyModel(id="1", bucket_count=1, bucket_capacity=10, location=loc),
            ],
            orders=[
                models.OrderModel(id="o", items=[models.ProductModel(id="p", volume=1, location=loc)]),
            ],
        )


def test_invalid_location_rejected():
    with pytest.raises(ValueError):
        op.WarehouseLocation(column=5, shelving_row=0, side=op.Side.LEFT, row=0)
    with pytest.raises(ValueError):
        op.WarehouseLocation(column=0, shelving_row=3, side=op.Side.LEFT, row=0)
    with pytest.raises(ValueError):
        op.WarehouseLocation(column=0, shelving_row=0, side=op.Side.LEFT, row=11)
