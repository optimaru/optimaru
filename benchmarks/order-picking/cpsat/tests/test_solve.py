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
from itertools import permutations, product

from order_picking.constraints import (
    DistanceFromLastPickTaskToPathOriginCost,
    DistanceFromPreviousPickTaskCost,
    Level,
    OrderSplitByTrolleyCost,
    TrolleyCapacityCost,
    get_constraints,
    score,
)
from order_picking_cpsat import catalog, solve
from order_picking_cpsat.constraints import REGISTRY
from ortools.sat.python import cp_model


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
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
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


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _three_picks(timelimit: float = 5) -> OrderPickingRequest:
    """Two trolleys (one bucket of 50 each) and three picks: O1 (40 + 40, two buckets) and
    O2 (40). Every assignment of the picks with every picking order: 24 solutions."""
    origin = LocationModel(column="A", shelving_row=1, side="LEFT", row=0)
    spots = [
        LocationModel(column="B", shelving_row=2, side="RIGHT", row=3),
        LocationModel(column="C", shelving_row=1, side="LEFT", row=5),
        LocationModel(column="D", shelving_row=3, side="RIGHT", row=1),
    ]
    return OrderPickingRequest(
        timelimit=timelimit,
        trolleys=[
            TrolleyModel(id=name, bucket_count=1, bucket_capacity=50, location=origin)
            for name in ("T1", "T2")
        ],
        orders=[
            OrderModel(
                id="O1",
                items=[
                    ProductModel(id="P1", volume=40, location=spots[0]),
                    ProductModel(id="P2", volume=40, location=spots[1]),
                ],
            ),
            OrderModel(
                id="O2", items=[ProductModel(id="P3", volume=40, location=spots[2])]
            ),
        ],
    )


def _routes():
    for trolleys in product(range(2), repeat=3):
        on = [
            [task for task, t in enumerate(trolleys) if t == trolley]
            for trolley in range(2)
        ]
        for first in permutations(on[0]):
            for second in permutations(on[1]):
                yield [list(first), list(second)]


def _routed(request, routes):
    canonical = to_solution(request)
    for trolley, route in zip(canonical.trolleys, routes, strict=True):
        trolley.pick_task_indices = route
    return canonical


def _engine_of(request, routes, selection):
    """The Python score() mirror of `routes` under `selection`."""
    canonical = _routed(request, routes)
    return score(constraint_costs(canonical, selection), selection)


def _engine_solve(request, routes, selection):
    """The engine's own score of `routes` under `selection`: a solve with no time keeps them."""
    solved = domain.solve(
        _routed(request, routes),
        time_limit_seconds=0,
        random_seed=1,
        constraints=selection,
    )
    assert [list(trolley.pick_task_indices) for trolley in solved.trolleys] == routes
    return solved.score.hard, solved.score.soft


SELECTIONS = {
    "repeat": [
        TrolleyCapacityCost(),
        DistanceFromPreviousPickTaskCost(),
        DistanceFromPreviousPickTaskCost(weight=2, offset=5),
    ],
    "repeat-literal": [
        DistanceFromPreviousPickTaskCost(offset=10, clip=False),
        DistanceFromPreviousPickTaskCost(weight=3, offset=-2),
        OrderSplitByTrolleyCost(),
        OrderSplitByTrolleyCost(weight=7, offset=1, clip=False),  # the empty trolley -7
    ],
    # A threshold on hard; the literal copy (refused on hard) sits on soft.
    "hard-offsets": [
        TrolleyCapacityCost(offset=1),
        TrolleyCapacityCost(level="soft", weight=2, offset=2, clip=False),
        DistanceFromLastPickTaskToPathOriginCost(offset=-3, clip=False),
    ],
    "identical-twice": [
        OrderSplitByTrolleyCost(),
        OrderSplitByTrolleyCost(),
        DistanceFromPreviousPickTaskCost(),
    ],
    # Each pick on its own level: a soft literal capacity next to the hard default, the
    # walk on hard beyond 4 next to the soft default, the order split moved to hard.
    "levels": [
        TrolleyCapacityCost(),
        TrolleyCapacityCost(level="soft", weight=3, offset=1, clip=False),
        DistanceFromPreviousPickTaskCost(),
        DistanceFromPreviousPickTaskCost(level=Level.HARD, offset=4),
        OrderSplitByTrolleyCost(level="hard", weight=2),
    ],
}


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_solution(selection):
    """Pin each of the 24 solutions: the CP-SAT objective's own levels are the engine's
    score of those routes under the same repeated / shifted picks (its own and the
    score() mirror's), exactly; the reported score re-prices the default costs (each name
    once) to the same value."""
    request = _three_picks()
    routes = list(_routes())
    assert len(routes) == 24
    for fixed in routes:
        proxy: dict[str, int] = {}
        result = solve(
            request, constraints=selection, _fixed_routes=fixed, _proxy_score=proxy
        )
        engine = _engine_of(request, fixed, selection)
        assert _engine_solve(request, fixed, selection) == tuple(engine), fixed
        assert (proxy["hard"], proxy["soft"]) == (engine.hard, engine.soft), fixed
        assert _reported(result) == (engine.hard, engine.soft), fixed


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection):
    request = _three_picks()
    best = max(tuple(_engine_of(request, fixed, selection)) for fixed in _routes())
    assert _reported(solve(request, constraints=selection)) == best


def test_mapping_selections_repeat_and_shift_like_specs():
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [
        OrderSplitByTrolleyCost(),
        OrderSplitByTrolleyCost(weight=3, offset=1, clip=False),
    ]
    dicts = [
        {"name": "order_split_by_trolley_cost"},
        {
            "name": "order_split_by_trolley_cost",
            "weight": 3,
            "offset": 1,
            "clip": False,
        },
    ]
    assert solve(_three_picks(), constraints=dicts) == solve(
        _three_picks(), constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat():
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _three_picks(timelimit=1), constraints=[OrderSplitByTrolleyCost(offset=0.5)]
        )


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_like_the_engine(level):
    """The domain has no medium level: CP-SAT refuses the pick as the engine does, a repeat too."""
    message = (
        "constraint 'order_split_by_trolley_cost': this use case has no medium level"
    )
    for selection in (
        [OrderSplitByTrolleyCost(level=level)],
        [OrderSplitByTrolleyCost(), OrderSplitByTrolleyCost(level=level)],
    ):
        with pytest.raises(ValueError, match=message):
            solve(_three_picks(timelimit=1), constraints=selection)
        with pytest.raises(ValueError, match=message):
            domain.solve(
                to_solution(_three_picks()), time_limit_seconds=0, constraints=selection
            )


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


@pytest.mark.parametrize("offset", [2, -2])
@pytest.mark.parametrize(
    "selection",
    [
        # The default level, hard.
        lambda offset: TrolleyCapacityCost(offset=offset, clip=False),
        lambda offset: {
            "name": "trolley_capacity_cost",
            "offset": offset,
            "clip": False,
        },
        lambda offset: DistanceFromPreviousPickTaskCost(
            level="hard", offset=offset, clip=False
        ),
        lambda offset: {
            "name": "distance_from_previous_pick_task_cost",
            "level": "hard",
            "offset": offset,
            "clip": False,
        },
    ],
    ids=["buckets-spec", "buckets-mapping", "walk-moved-to-hard", "walk-mapping"],
)
def test_the_literal_offset_is_refused_on_hard_like_the_engine(selection, offset):
    """The CP-SAT reference refuses the pick with the engine's and score()'s message."""
    pick = selection(offset)
    name = pick["name"] if isinstance(pick, dict) else pick.name
    message = HARD_REFUSAL.format(q=repr(name))
    for picks in ([pick], [TrolleyCapacityCost(), OrderSplitByTrolleyCost(), pick]):
        with pytest.raises(ValueError) as refused:
            solve(_three_picks(timelimit=1), constraints=picks)
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            REGISTRY.objective(cp_model.CpModel(), ("hard", "soft"), picks)
        assert str(refused.value) == message
    if not isinstance(pick, dict):
        # The engine and the score() mirror say the same.
        canonical = to_solution(_three_picks())
        with pytest.raises(ValueError) as refused:
            constraint_costs(canonical, [pick])
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            score(constraint_costs(canonical), [pick])
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            domain.solve(
                to_solution(_three_picks()), time_limit_seconds=0, constraints=[pick]
            )
        assert str(refused.value) == message


@pytest.mark.parametrize("offset", [2, -2])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    picks = [
        TrolleyCapacityCost(),
        OrderSplitByTrolleyCost(),
        TrolleyCapacityCost(level="soft", offset=offset, clip=False),
    ]
    request = _three_picks()
    best = max(tuple(_engine_of(request, fixed, picks)) for fixed in _routes())
    assert _reported(solve(request, constraints=picks)) == best
