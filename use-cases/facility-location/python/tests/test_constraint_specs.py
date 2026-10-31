"""facility_location.constraints — an API for picking the cost function declaratively."""

import pickle
import re
from types import SimpleNamespace

import pytest

import facility_location as fl
from facility_location.constraints import (
    Constraint,
    DistanceFromFacilityCost,
    FacilityCapacityCost,
    FacilitySetupCost,
    distance_from_facility_cost,
    facility_capacity_cost,
    facility_setup_cost,
    get_constraints,
    score,
)

capacity = facility_capacity_cost
setup_cost = facility_setup_cost
distance = distance_from_facility_cost


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
        "facility_capacity_cost",
        "facility_setup_cost",
        "distance_from_facility_cost",
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
    heavier = solve([capacity, DistanceFromFacilityCost(weight=60)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft empties the hard level."""
    problem = build_problem()
    # Push demand 12 into a capacity-5 facility to force a violation.
    for consumer in problem.consumers:
        consumer.facility = 0
    soft_capacity = FacilityCapacityCost(hard=False)
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


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, hard=None, exponent=1)."""
    for spec in (facility_capacity_cost, facility_setup_cost, distance_from_facility_cost):
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in get_constraints()] == [
        "facility_capacity_cost",
        "facility_setup_cost",
        "distance_from_facility_cost",
    ]


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = fl.constraint_info()
    assert [row.name for row in rows] == fl.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("soft", 2.0, True, False, True),
        ("soft", 6.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = fl.constraint_info()[0]
    assert isinstance(row, fl.ConstraintInfo)
    assert row._asdict() == {
        "name": "facility_capacity_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='facility_capacity_cost'")
    assert pickle.loads(pickle.dumps(fl.constraint_info())) == fl.constraint_info()
    assert fl.ConstraintInfo.__module__ == fl._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = fl.constraint_costs(build_problem())
    for row in fl.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"], entry["global"]) == (True, False, False)
        assert (entry["name"], entry["label"]) == (row.name, "")  # one instance per name, no label
        assert "medium_fold" not in entry  # no medium level in this domain


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights 1 / 2 / 6."""
    explicit = [
        FacilityCapacityCost(weight=1),
        FacilitySetupCost(weight=2),
        DistanceFromFacilityCost(weight=6),
    ]
    problem = _solved()
    raw = fl.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (problem.score.hard, problem.score.soft)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": float("inf")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"exponent": -1.0}, ValueError),
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        FacilitySetupCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "facility_setup_cost"
        weight = -2.0
        hard = None
        exponent = 1.0

    class Flat:
        name = "facility_setup_cost"
        weight = None
        hard = None
        exponent = 0.0

    for pick, message in ((Negative(), "weight must be finite and >= 0"), (Flat(), "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            fl.solve(build_problem(), time_limit_seconds=0.1, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="facility_setup_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'facility_setup_cost': {re.escape(message)}"):
        fl.solve(build_problem(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        FacilitySetupCost(**knobs)


def test_core_accepts_numpy_scalars_like_the_python_base():
    numpy = pytest.importorskip("numpy")
    pick = SimpleNamespace(name="facility_capacity_cost", weight=numpy.float64(3.0), hard=numpy.False_, exponent=1.0)
    problem = _solved([pick])
    spec = FacilityCapacityCost(weight=numpy.float64(3.0), hard=numpy.False_)
    assert score(fl.constraint_costs(problem), [spec]) == (problem.score.hard, pytest.approx(problem.score.soft))


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in ("facility_capacity", "distance_from_facility"):

        class Old:
            name = old
            weight = None
            hard = None
            exponent = 1.0

        with pytest.raises(ValueError, match="unknown constraint"):
            solve([Old()])


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

    assert raw["facility_capacity_cost"]["costs"] == pytest.approx([2.0, 0.0])
    assert raw["facility_setup_cost"]["costs"] == pytest.approx([100.0, 0.0])
    assert raw["distance_from_facility_cost"]["costs"] == pytest.approx([777.0, 0.0])
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
        [capacity, setup_cost, DistanceFromFacilityCost(weight=60)],
        [FacilityCapacityCost(hard=False), setup_cost, distance],
        [capacity, setup_cost, DistanceFromFacilityCost(exponent=2.0)],
        [FacilityCapacityCost(exponent=2.0), FacilitySetupCost(hard=True, weight=0.5), distance],
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
            constraints=[capacity, DistanceFromFacilityCost(weight=10), distance],
        )
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([capacity, capacity])


# --- The range of a score (an int64 hard level) ---------------------------------


def _three_overloaded():
    """Three facilities of capacity 1, two consumers of demand 2 on each: raw capacity cost [3, 3, 3]."""
    problem = fl.FacilityLocationProblem(
        [fl.Facility(f"f{i}", fl.Location(0.0, 0.0), 0, 1) for i in range(3)],
        [fl.Consumer(f"c{i}", fl.Location(0.1, 0.0), 2) for i in range(6)],
    )
    problem.set_assignment([0, 0, 1, 1, 2, 2])
    return problem


def _zero_budget_solve(problem, picks):
    config = fl.SolverConfig()
    config.spent_limit_seconds = 0
    fl.Solver(config).solve_with_constraints(problem, picks)
    return problem


def test_a_hard_total_beyond_int64_saturates_on_both_paths():
    """3 x 4.5e18 used to wrap to a positive, feasible-looking hard score; it saturates now."""
    picks = [FacilityCapacityCost(weight=1.5e18)]
    assert fl.constraint_costs(_three_overloaded())["facility_capacity_cost"]["costs"] == [3.0, 3.0, 3.0]
    assert score(fl.constraint_costs(_three_overloaded()), picks).hard == -(2**63 - 1)
    problem = _zero_budget_solve(_three_overloaded(), picks)
    assert problem.assignment() == [0, 0, 1, 1, 2, 2]
    assert problem.score.hard == -(2**63 - 1)
    assert not problem.score.is_feasible()
    exact = [FacilityCapacityCost(weight=1e18)]
    assert _zero_budget_solve(_three_overloaded(), exact).score.hard == -9 * 10**18
    assert score(fl.constraint_costs(_three_overloaded()), exact).hard == -9 * 10**18


@pytest.mark.parametrize(
    "pick",
    [FacilityCapacityCost(weight=1e19), DistanceFromFacilityCost(weight=1e300, exponent=50)],
    ids=["hard-beyond-2^62", "soft-not-finite"],
)
def test_an_out_of_range_contribution_raises_overflow_error_on_both_paths(pick):
    """Python (score) and C++ (the solve) refuse the same contribution, naming the constraint."""
    with pytest.raises(OverflowError, match=pick.name):
        score(fl.constraint_costs(_three_overloaded()), [pick])
    with pytest.raises(OverflowError, match=re.sub(r"_(\w)", lambda m: m.group(1).upper(), pick.name)):
        _zero_budget_solve(_three_overloaded(), [pick])
