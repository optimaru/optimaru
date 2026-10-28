"""facility_location.constraints — an API for picking the cost function declaratively."""

import pytest

import facility_location as fl
from facility_location.constraints import (
    Constraint,
    DistanceFromFacility,
    FacilityCapacity,
    capacity,
    distance,
    get_constraints,
    score,
    setup_cost,
)


def build_problem():
    """A cheap far facility vs an expensive near one. Which constraints are on decides the answer."""
    return fl.FacilityLocationProblem(
        [
            fl.Facility("near", fl.Location(0.0, 0.0), 1000, 5),
            fl.Facility("far", fl.Location(0.1, 0.0), 10, 100),
        ],
        [fl.Consumer(f"C{i}", fl.Location(0.0, 0.0), 3) for i in range(4)],
    )


def solve(constraints=None):
    problem = build_problem()
    fl.solve(problem, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return problem.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = fl.list_constraints()
    assert available == [
        "facility_capacity",
        "facility_setup_cost",
        "distance_from_facility",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == fl.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    hard_only = solve([capacity])
    assert hard_only.soft == 0.0


def test_reweighting_one_constraint_scales_only_that_term():
    """Distance weight x10 only: soft grows while hard stays put."""
    base = solve([capacity, distance])
    heavier = solve([capacity, DistanceFromFacility(weight=60)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft empties the hard level."""
    problem = build_problem()
    # Push demand 12 into a capacity-5 facility to force a violation.
    for consumer in problem.consumers:
        consumer.facility = 0
    soft_capacity = FacilityCapacity(hard=False)
    assert fl.calculate_score(problem).hard < 0
    fl.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=[soft_capacity])
    assert problem.score.hard == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "facility_setup_costs"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        fl.solve(build_problem(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_setup_cost_instance_is_the_default_weight():
    """Module-level instances must keep the defaults (weight=None)."""
    assert setup_cost.weight is None
    assert setup_cost.hard is None
    assert setup_cost.exponent == 1.0


def test_constraint_costs_match_capacity_setup_and_distance_by_hand():
    """Each raw component is checked without running a solver.

    Facility 0 carries demand 4 + 3 against capacity 5, is opened once for
    100, and serves consumers 0.003 and 0.004 degrees away.  With the domain's
    111,000 m/degree metric those distances are exactly 333 m and 444 m.
    The unassigned consumer and unused facility contribute nothing.
    """
    problem = fl.FacilityLocationProblem(
        [
            fl.Facility("used", fl.Location(0.0, 0.0), 100, 5),
            fl.Facility("unused", fl.Location(1.0, 1.0), 999, 100),
        ],
        [
            fl.Consumer("c1", fl.Location(0.003, 0.0), 4),
            fl.Consumer("c2", fl.Location(0.0, 0.004), 3),
            fl.Consumer("unassigned", fl.Location(1.0, 1.0), 50),
        ],
    )
    problem.set_assignment([0, 0, -1])

    raw = fl.constraint_costs(problem)

    assert raw["facility_capacity"]["costs"] == pytest.approx([2.0, 0.0])
    assert raw["facility_setup_cost"]["costs"] == pytest.approx([100.0, 0.0])
    assert raw["distance_from_facility"]["costs"] == pytest.approx([777.0, 0.0])
    # Default weights: capacity=1 hard, setup=2 soft, distance=6 soft.
    assert score(raw).hard == -2
    assert score(raw).soft == pytest.approx(-(2 * 100 + 6 * 777))


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _solved(constraints=None):
    problem = build_problem()
    fl.solve(problem, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return problem


def test_score_equals_the_engine_for_the_default_assembly():
    """A two-level domain, so hard/soft can be compared directly."""
    problem = _solved()
    engine = fl.calculate_score(problem)
    python = score(fl.constraint_costs(problem))
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [capacity],
        [capacity, setup_cost, DistanceFromFacility(weight=60)],
        [FacilityCapacity(hard=False), setup_cost, distance],
        [capacity, setup_cost, DistanceFromFacility(exponent=2.0)],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per unit, so reproducing it needs the per-unit raw costs —
    totals alone are not enough."""
    problem = _solved(specs)
    python = score(fl.constraint_costs(problem), specs)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)


def test_score_is_additive_over_the_picked_constraints():
    problem = _solved()
    raw = fl.constraint_costs(problem)
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw = fl.constraint_costs(_solved())

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


def test_naming_a_constraint_twice_is_rejected():
    """Passing the same constraint twice must not let the later one silently win.

    It is the same class of mistake as a typo, so it is an error — write the
    desired metadata exactly once."""
    with pytest.raises(ValueError, match="picked more than once"):
        fl.solve(
            build_problem(),
            time_limit_seconds=0.2,
            constraints=[capacity, DistanceFromFacility(weight=10), distance],
        )
