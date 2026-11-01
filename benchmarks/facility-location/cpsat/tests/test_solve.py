from fractions import Fraction
from itertools import product

import numpy as np
import pytest

import facility_location as domain
from facility_location.constraints import (
    DistanceFromFacilityCost,
    FacilityCapacityCost,
    FacilitySetupCost,
    get_constraints,
    score,
)
from facility_location.models import (
    ConsumerModel,
    FacilityLocationProblemModel,
    FacilityModel,
    LocationModel,
)
from facility_location_cpsat import solve
from facility_location_cpsat.solve import build_model
from facility_location import calculate_score
from ortools.sat.python import cp_model


def test_prefers_capacity_feasibility_before_distance() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=1,
                capacity=1,
            ),
            FacilityModel(
                id="far",
                location=LocationModel(latitude=1, longitude=1),
                setup_cost=1,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
            ConsumerModel(
                id="b", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert {consumer.facility for consumer in solved.consumers} == {0, 1}
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def test_uses_one_facility_when_setup_cost_dominates() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="a",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=10,
            ),
            FacilityModel(
                id="b",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
            ConsumerModel(
                id="b", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert len({consumer.facility for consumer in solved.consumers}) == 1


def test_explicit_constraint_list_replaces_defaults() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=0,
            ),
            FacilityModel(
                id="far",
                location=LocationModel(latitude=1, longitude=1),
                setup_cost=1,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            )
        ],
    )

    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"type": "distance_from_facility_cost"}],
    )

    assert solved.consumers[0].facility == 0
    assert solved.score_hard == 0  # capacity was not selected


def test_accepts_the_same_domain_constraint_object_as_the_engine() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=0,
            ),
            FacilityModel(
                id="far",
                location=LocationModel(latitude=1, longitude=1),
                setup_cost=1,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            )
        ],
    )

    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=[DistanceFromFacilityCost(weight=10)],
    )

    assert solved.consumers[0].facility == 0
    assert solved.score_hard == 0


def _parity_problem() -> FacilityLocationProblemModel:
    return FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="small-near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=9,
                capacity=3,
            ),
            FacilityModel(
                id="large-far",
                location=LocationModel(latitude=0.01, longitude=0),
                setup_cost=2,
                capacity=8,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=3
            ),
            ConsumerModel(
                id="b", location=LocationModel(latitude=0, longitude=0), demand=2
            ),
        ],
    )


def _brute_force_score(problem, constraint):
    best = None
    for assignment in product(
        range(len(problem.facilities)), repeat=len(problem.consumers)
    ):
        candidate = problem.model_copy(deep=True)
        for consumer, facility in zip(candidate.consumers, assignment, strict=True):
            consumer.facility = facility
        value = score(domain.constraint_costs(candidate.to_core()), [constraint])
        best = max(best, value) if best is not None else value
    return best


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_brute_force_independently(constraint) -> None:
    problem = _parity_problem()
    expected = _brute_force_score(problem, constraint)

    solved = solve(problem, time_limit_seconds=2, constraints=[constraint])

    assert (solved.score_hard, solved.score_soft) == pytest.approx(expected)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    from facility_location_cpsat import catalog

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
        assert entry["kind"] == "either"  # every facility-location constraint is flippable
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0, "offset": 0.0, "clip": True}


def test_demoting_capacity_scores_like_the_engine() -> None:
    """level="soft" lands on soft in both solvers: CP-SAT's reported score is the
    engine's score of the same solution under the same selection."""
    problem = _parity_problem()
    selection = [FacilityCapacityCost(level="soft"), *get_constraints()[1:]]
    solved = solve(problem, time_limit_seconds=2, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert solved.score_hard == 0


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _three_by_three() -> FacilityLocationProblemModel:
    """Three facilities (one far, one small) and three consumers: 27 assignments."""
    return FacilityLocationProblemModel(
        facilities=[
            FacilityModel(id="a", location=LocationModel(latitude=0, longitude=0), setup_cost=9, capacity=3),
            FacilityModel(id="b", location=LocationModel(latitude=0.01, longitude=0), setup_cost=2, capacity=8),
            FacilityModel(id="c", location=LocationModel(latitude=0, longitude=0.02), setup_cost=4, capacity=2),
        ],
        consumers=[
            ConsumerModel(id="p", location=LocationModel(latitude=0, longitude=0), demand=3),
            ConsumerModel(id="q", location=LocationModel(latitude=0.005, longitude=0), demand=2),
            ConsumerModel(id="r", location=LocationModel(latitude=0, longitude=0.01), demand=1),
        ],
    )


SELECTIONS = {
    "repeat": [DistanceFromFacilityCost(), DistanceFromFacilityCost(weight=2, offset=500)],
    "repeat-literal": [
        FacilityCapacityCost(),
        FacilitySetupCost(),
        DistanceFromFacilityCost(offset=1000, clip=False),
        DistanceFromFacilityCost(weight=3, offset=-200),
    ],
    "hard-offsets": [
        FacilityCapacityCost(offset=1),
        # The literal formula with an offset is refused on hard; its copy sits on soft.
        FacilityCapacityCost(level="soft", weight=2, offset=2, clip=False),
        FacilitySetupCost(offset=-3, clip=False),
    ],
    "identical-twice": [FacilitySetupCost(), FacilitySetupCost(), DistanceFromFacilityCost()],
}


def _assignments(problem):
    for assignment in product(range(len(problem.facilities)), repeat=len(problem.consumers)):
        candidate = problem.model_copy(deep=True)
        for consumer, facility in zip(candidate.consumers, assignment, strict=True):
            consumer.facility = facility
        yield assignment, candidate


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_assignment(selection) -> None:
    """Pin each of the 27 assignments: the CP-SAT objective's per-level value is the
    engine's score of that assignment under the same repeated / shifted picks, exactly."""
    problem = _three_by_three()
    for assignment, candidate in _assignments(problem):
        model, x, objective = build_model(problem, selection)
        for (consumer, facility), literal in x.items():
            model.add(literal == int(assignment[consumer] == facility))
        objective.minimize()
        solver = cp_model.CpSolver()
        assert solver.solve(model) == cp_model.OPTIMAL
        core = candidate.to_core()
        engine = score(domain.constraint_costs(core, selection), selection)
        assert objective.score(solver) == {"hard": engine.hard, "soft": engine.soft}, assignment
        # Re-pricing the default costs (each name once) gives the same per instance key.
        assert objective.score_costs(domain.constraint_costs(core)) == {"hard": engine.hard, "soft": engine.soft}


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _three_by_three()
    best = max(
        tuple(score(domain.constraint_costs(candidate.to_core(), selection), selection))
        for _, candidate in _assignments(problem)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(best)
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    problem = _three_by_three()
    specs = [FacilitySetupCost(), FacilitySetupCost(weight=3, offset=4, clip=False)]
    dicts = [{"name": "facility_setup_cost"}, {"name": "facility_setup_cost", "weight": 3, "offset": 4, "clip": False}]
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(_three_by_three(), time_limit_seconds=1, constraints=[DistanceFromFacilityCost(offset=0.5)])


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


@pytest.mark.parametrize("offset", [2, -2])
@pytest.mark.parametrize(
    "selection",
    [
        lambda offset: FacilityCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: {"name": "facility_capacity_cost", "offset": offset, "clip": False},
        lambda offset: DistanceFromFacilityCost(level="hard", offset=offset, clip=False),
        lambda offset: {"name": "distance_from_facility_cost", "level": "hard", "offset": offset, "clip": False},
    ],
    ids=["capacity-spec", "capacity-mapping", "distance-moved-to-hard", "distance-mapping"],
)
def test_the_literal_offset_is_refused_on_hard_like_the_engine(selection, offset) -> None:
    """The CP-SAT reference refuses the pick with the engine's and score()'s message."""
    pick = selection(offset)
    name = pick["name"] if isinstance(pick, dict) else pick.name
    message = HARD_REFUSAL.format(q=repr(name))
    for picks in ([pick], [FacilityCapacityCost(), DistanceFromFacilityCost(), pick]):
        with pytest.raises(ValueError) as refused:
            solve(_three_by_three(), time_limit_seconds=1, constraints=picks)
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            build_model(_three_by_three(), picks)
        assert str(refused.value) == message
    if not isinstance(pick, dict):
        # The engine and the score() mirror say the same.
        core = _three_by_three().to_core()
        with pytest.raises(ValueError) as refused:
            domain.constraint_costs(core, [pick])
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            score(domain.constraint_costs(core), [pick])
        assert str(refused.value) == message


@pytest.mark.parametrize("offset", [2, -2])
def test_the_same_literal_pick_on_soft_is_accepted(offset) -> None:
    picks = [FacilityCapacityCost(), FacilityCapacityCost(level="soft", offset=offset, clip=False)]
    problem = _three_by_three()
    best = max(
        tuple(score(domain.constraint_costs(candidate.to_core(), picks), picks)) for _, candidate in _assignments(problem)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=picks)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(best)


@pytest.mark.parametrize(
    "offset", [Fraction(1, 10**400), np.longdouble("1e-4000")], ids=["Fraction", "longdouble"]
)
def test_an_offset_that_is_zero_as_a_double_is_accepted_like_the_engine(offset) -> None:
    """The engine reads float(offset): 0.0, so the literal pick on hard is the plain shaping.
    The CP-SAT reference and score() read the same double instead of refusing it."""
    pick = FacilityCapacityCost(offset=offset, clip=False)
    mapping = {"name": "facility_capacity_cost", "offset": offset, "clip": False}
    build_model(_three_by_three(), [pick])
    build_model(_three_by_three(), [mapping])
    core = _three_by_three().to_core()
    assert score(domain.constraint_costs(core), [pick]) == score(domain.constraint_costs(core, [pick]), [pick])
    solved = solve(_three_by_three(), time_limit_seconds=5, constraints=[pick])
    plain = solve(_three_by_three(), time_limit_seconds=5, constraints=[FacilityCapacityCost()])
    assert (solved.score_hard, solved.score_soft) == (plain.score_hard, plain.score_soft)

