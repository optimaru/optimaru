from order_picking.models import (
    LocationModel,
    OrderModel,
    OrderPickingRequest,
    ProductModel,
    TrolleyModel,
    to_solution,
)
import pytest

import order_picking as domain
from order_picking import calculate_score, constraint_costs
from order_picking.constraints import (
    OrderSplitByTrolleyCost,
    TrolleyCapacityCost,
    get_constraints,
    score,
)
from order_picking_cpsat import catalog, solve


def test_smoke_and_constraint_selection():
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=2, bucket_capacity=100, location=origin)
        ],
        orders=[
            OrderModel(
                id="O1",
                items=[ProductModel(id="P1", volume=50, location=origin)],
            )
        ],
    )
    result = solve(request, constraints=["trolley_capacity_cost"])
    assert result.feasible
    assert result.unassigned == []
    assert result.routes["T1"][0].pick_task_id == "O1-0"
    assert result.total_distance == 0
    assert len(catalog()) == 4


def test_default_score_matches_the_engine_on_the_same_nonzero_route():
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    pick = LocationModel(column="B", shelving_row=2, side="RIGHT", row=4)
    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=2, bucket_capacity=100, location=origin)
        ],
        orders=[
            OrderModel(id="O1", items=[ProductModel(id="P1", volume=50, location=pick)])
        ],
    )
    result = solve(request)
    assert result.total_distance > 0

    canonical = to_solution(request)
    task_by_id = {task.id: i for i, task in enumerate(canonical.pick_tasks)}
    canonical.trolleys[0].pick_task_indices = [
        task_by_id[stop.pick_task_id] for stop in result.routes["T1"]
    ]
    assert result.score == str(calculate_score(canonical))


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_cpsat_constraint_matches_shared_raw_cost(constraint):
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    locations = [
        LocationModel(column="B", shelving_row=2, side="RIGHT", row=3),
        LocationModel(column="C", shelving_row=1, side="LEFT", row=5),
        LocationModel(column="D", shelving_row=3, side="RIGHT", row=1),
    ]
    request = OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=1, bucket_capacity=50, location=origin),
            TrolleyModel(id="T2", bucket_count=1, bucket_capacity=50, location=origin),
        ],
        orders=[
            OrderModel(
                id="O1",
                items=[
                    ProductModel(id="P1", volume=40, location=locations[0]),
                    ProductModel(id="P2", volume=40, location=locations[1]),
                ],
            ),
            OrderModel(
                id="O2",
                items=[ProductModel(id="P3", volume=40, location=locations[2])],
            ),
        ],
    )
    fixed_routes = [[0, 2], [1]]
    proxy: dict[str, int] = {}

    solve(
        request,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_routes=fixed_routes,
        _proxy_score=proxy,
    )

    canonical = to_solution(request)
    canonical.trolleys[0].pick_task_indices = fixed_routes[0]
    canonical.trolleys[1].pick_task_indices = fixed_routes[1]
    expected = -sum(constraint_costs(canonical)[constraint.name]["costs"])
    assert sum(proxy.values()) == expected


def test_cpsat_defaults_are_the_engine_defaults():
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every order-picking constraint is flippable
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0}
    # The order split weighs 1000 on both paths (it was restated by hand before).
    assert engine["order_split_by_trolley_cost"].default_weight == 1000.0


def _short_on_buckets_request():
    """One trolley with one bucket of 50 for two orders of 40: always 1 bucket short."""
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    return OrderPickingRequest(
        timelimit=2,
        trolleys=[
            TrolleyModel(id="T1", bucket_count=1, bucket_capacity=50, location=origin)
        ],
        orders=[
            OrderModel(
                id="O1",
                items=[
                    ProductModel(
                        id="P1",
                        volume=40,
                        location=LocationModel(
                            column="B", shelving_row=2, side="RIGHT", row=3
                        ),
                    )
                ],
            ),
            OrderModel(
                id="O2",
                items=[
                    ProductModel(
                        id="P2",
                        volume=40,
                        location=LocationModel(
                            column="D", shelving_row=3, side="RIGHT", row=1
                        ),
                    )
                ],
            ),
        ],
    )


def _engine_score(request, result, selection):
    canonical = to_solution(request)
    task_by_id = {task.id: i for i, task in enumerate(canonical.pick_tasks)}
    for trolley in canonical.trolleys:
        trolley.pick_task_indices = [
            task_by_id[stop.pick_task_id] for stop in result.routes[trolley.id]
        ]
    return score(constraint_costs(canonical), selection)


def _reported(result):
    hard, soft = result.score.removesuffix("soft").split("hard/")
    return int(hard), float(soft)


def test_default_score_parity_on_a_violated_capacity():
    """The default selection scores the CP-SAT solution like the engine does."""
    request = _short_on_buckets_request()
    result = solve(request)
    engine = _engine_score(request, result, None)
    assert _reported(result) == (engine.hard, pytest.approx(engine.soft))
    assert engine.hard == -1


def test_demoting_capacity_scores_like_the_engine():
    """level="soft" lands on soft in both solvers: CP-SAT's reported score is the
    engine's score of the same solution under the same selection."""
    request = _short_on_buckets_request()
    selection = [TrolleyCapacityCost(level="soft"), *get_constraints()[1:]]
    result = solve(request, constraints=selection)
    engine = _engine_score(request, result, selection)
    assert _reported(result) == (engine.hard, pytest.approx(engine.soft))
    assert engine.hard == 0 and result.feasible


def test_promoting_the_split_scores_like_the_engine():
    """level="hard" on a soft constraint lands on hard in both solvers."""
    request = _short_on_buckets_request()
    selection = [*get_constraints()[:3], OrderSplitByTrolleyCost(level="hard", weight=2)]
    result = solve(request, constraints=selection)
    engine = _engine_score(request, result, selection)
    assert _reported(result) == (engine.hard, pytest.approx(engine.soft))
    # One bucket short + 2 x two (order, trolley) pairs.
    assert engine.hard == -(1 + 2 * 2)
