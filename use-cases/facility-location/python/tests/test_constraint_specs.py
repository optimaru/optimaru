"""facility_location.constraints — an API for picking the cost function declaratively."""

import pickle
import random
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
    soft_capacity = FacilityCapacityCost(level="soft")
    assert fl.calculate_score(problem).hard < 0
    fl.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=[soft_capacity])
    assert problem.score.hard == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "facility_setup_costs"  # trailing s
        weight = None
        level = None
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
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in (facility_capacity_cost, facility_setup_cost, distance_from_facility_cost):
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
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
        level = None
        exponent = 1.0

    class Flat:
        name = "facility_setup_cost"
        weight = None
        level = None
        exponent = 0.0

    class Unbounded:
        name = "facility_setup_cost"
        weight = None
        level = None
        exponent = 1.0
        offset = float("inf")

    for pick, message in (
        (Negative(), "weight must be finite and >= 0"),
        (Flat(), "exponent must be finite and > 0"),
        (Unbounded(), "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            fl.solve(build_problem(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        FacilitySetupCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="facility_setup_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'facility_setup_cost': {re.escape(message)}"):
        fl.solve(build_problem(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        FacilitySetupCost(**knobs)


def test_core_accepts_numpy_scalars_like_the_python_base():
    numpy = pytest.importorskip("numpy")
    pick = SimpleNamespace(name="facility_capacity_cost", weight=numpy.float64(3.0), level="soft", exponent=1.0)
    problem = _solved([pick])
    spec = FacilityCapacityCost(weight=numpy.float64(3.0), level="soft")
    assert score(fl.constraint_costs(problem), [spec]) == (problem.score.hard, pytest.approx(problem.score.soft))


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in ("facility_capacity", "distance_from_facility"):

        class Old:
            name = old
            weight = None
            level = None
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
        [FacilityCapacityCost(level="soft"), setup_cost, distance],
        [capacity, setup_cost, DistanceFromFacilityCost(exponent=2.0)],
        [FacilityCapacityCost(exponent=2.0), FacilitySetupCost(level="hard", weight=0.5), distance],
        [capacity, setup_cost, distance, DistanceFromFacilityCost(weight=0.5, exponent=2.0)],
        [capacity, setup_cost, DistanceFromFacilityCost(offset=10000.0), DistanceFromFacilityCost(offset=-50.0)],
        [capacity, FacilitySetupCost(offset=500.0, clip=False), DistanceFromFacilityCost(offset=20000.0, clip=False)],
        [
            FacilityCapacityCost(offset=2.0),
            FacilityCapacityCost(level="soft", offset=1.0, clip=False, weight=3),
            setup_cost,
            distance,
        ],
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


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [FacilityCapacityCost(), DistanceFromFacilityCost(weight=10), DistanceFromFacilityCost()]
    problem = _zero_budget_solve(_two_assigned(), picks)
    assert problem.score.hard == -2  # demand 7 on capacity 5
    assert problem.score.soft == -(10 + 6) * 777  # both distance picks
    assert get_constraints([FacilityCapacityCost(), FacilityCapacityCost()]) == [
        FacilityCapacityCost(),
        FacilityCapacityCost(),
    ]


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
    """Python (score) and C++ (the solve) refuse the same contribution with the same message,
    naming the constraint by its Python name (the C++ path used to name the registry id)."""
    with pytest.raises(OverflowError, match=f"^constraint '{pick.name}': ") as in_python:
        score(fl.constraint_costs(_three_overloaded()), [pick])
    with pytest.raises(OverflowError) as in_engine:
        _zero_budget_solve(_three_overloaded(), [pick])
    assert str(in_engine.value) == str(in_python.value)


def test_the_overflow_message_reads_the_same_on_both_paths():
    hard = [FacilityCapacityCost(weight=1e19)]
    expected = (
        "constraint 'facility_capacity_cost': a hard contribution of 3e+19 exceeds the engine's "
        "limit of 2**62 (about 4.6e18) per unit; lower its weight or exponent"
    )
    with pytest.raises(OverflowError) as in_engine:
        _zero_budget_solve(_three_overloaded(), hard)
    assert str(in_engine.value) == expected
    # A weight of 0 is exactly 0 at any exponent on both paths: cost**400 beyond a double is
    # never computed, so 0 x inf can no longer turn an opt-in term left at 0 into nan.
    zero = [FacilityCapacityCost(), DistanceFromFacilityCost(weight=0.0, exponent=400)]
    assert score(fl.constraint_costs(_three_overloaded()), zero) == (-9, 0.0)
    solved = _zero_budget_solve(_three_overloaded(), zero)
    assert (solved.score.hard, solved.score.soft) == (-9, 0.0)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _two_assigned():
    """The hand-calculated instance with both consumers on facility 0, facility 1 unused.

    Raw per facility: capacity [2, 0] (demand 7 on capacity 5), setup [100, 0],
    distance [777, 0] (333 m + 444 m).
    """
    problem = fl.FacilityLocationProblem(
        [
            fl.Facility("used", fl.Location(0.0, 0.0), 100, 5),
            fl.Facility("unused", fl.Location(1.0, 1.0), 999, 100),
        ],
        [
            fl.Consumer("c1", fl.Location(0.003, 0.0), 4),
            fl.Consumer("c2", fl.Location(0.0, 0.004), 3),
        ],
    )
    problem.set_assignment([0, 0])
    return problem


REPEAT = [DistanceFromFacilityCost(), DistanceFromFacilityCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """distance x 6 plus 0.5 x distance^2 per facility: 6 x 777 + 0.5 x 777^2 = 306526.5."""
    raw = fl.constraint_costs(_two_assigned(), REPEAT)
    assert list(raw) == ["distance_from_facility_cost", "distance_from_facility_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [777.0, 0.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (6.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "distance_from_facility_cost"
    assert second["default_weight"] == 6.0
    expected = (0, -(6 * 777 + 0.5 * 777**2))
    assert expected == (0, -306526.5)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(fl.constraint_costs(_two_assigned()), REPEAT) == expected
    engine = _zero_budget_solve(_two_assigned(), REPEAT)
    assert engine.assignment() == [0, 0]
    assert (engine.score.hard, engine.score.soft) == expected


def test_identical_picks_count_twice():
    picks = [FacilitySetupCost(), FacilitySetupCost()]
    assert score(fl.constraint_costs(_two_assigned()), picks) == (0, -400.0)
    assert _zero_budget_solve(_two_assigned(), picks).score.soft == -400.0
    assert list(fl.constraint_costs(_two_assigned(), picks)) == ["facility_setup_cost", "facility_setup_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        FacilitySetupCost(weight=1),
        DistanceFromFacilityCost(),
        FacilitySetupCost(weight=2),
        FacilityCapacityCost(),
        FacilitySetupCost(weight=3),
    ]
    raw = fl.constraint_costs(_two_assigned(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "facility_capacity_cost": 1.0,
        "facility_setup_cost": 1.0,
        "facility_setup_cost#2": 2.0,
        "facility_setup_cost#3": 3.0,
        "distance_from_facility_cost": 6.0,
    }
    assert score(raw, picks) == (-2, -(600 + 6 * 777))
    engine = _zero_budget_solve(_two_assigned(), picks)
    assert (engine.score.hard, engine.score.soft) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Unit constraints, shaped per facility; facility 1 is unused (raw 0).
        (DistanceFromFacilityCost(offset=100), (0, -6 * 677)),
        (DistanceFromFacilityCost(offset=100, clip=False), (0, -6 * (677 - 100))),  # the unused one is rewarded
        (DistanceFromFacilityCost(offset=1000), (0, 0.0)),
        (DistanceFromFacilityCost(offset=1000, clip=False), (0, -6 * (-223 - 1000))),
        (DistanceFromFacilityCost(offset=700, exponent=2), (0, -6 * 77**2)),
        (DistanceFromFacilityCost(offset=800, exponent=3, clip=False), (0, -6 * (-(23**3) - 800**3))),
        (FacilitySetupCost(offset=-10), (0, -2 * 110)),  # a negative offset adds to every non-zero facility
        (FacilitySetupCost(offset=-10, clip=False), (0, -2 * (110 + 10))),  # ...or to every facility
        (FacilityCapacityCost(offset=1), (-1, 0.0)),
        (FacilityCapacityCost(offset=1.5), (-1, 0.0)),  # hard rounds per facility: 0.5 -> 1, 0 -> 0
        # The literal formula with an offset is refused on hard; on soft it charges
        # (2 - 3) + (0 - 3) = -4 (a reward below 3) and 0.5 - 1.5 = -1, not rounded.
        (FacilityCapacityCost(level="soft", offset=3, clip=False), (0, 4.0)),
        (FacilityCapacityCost(level="soft", offset=1.5, clip=False, weight=1), (0, 1.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(fl.constraint_costs(_two_assigned()), [pick]) == expected
    raw = fl.constraint_costs(_two_assigned(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    engine = _zero_budget_solve(_two_assigned(), [pick])
    assert engine.assignment() == [0, 0]
    assert (engine.score.hard, engine.score.soft) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [FacilityCapacityCost(), FacilitySetupCost(), DistanceFromFacilityCost(exponent=2)]
    shifted = [
        FacilityCapacityCost(offset=0.0, clip=False),
        FacilitySetupCost(offset=0),
        DistanceFromFacilityCost(exponent=2, offset=0.0, clip=False),
    ]
    problem = _solved()
    raw = fl.constraint_costs(problem)
    assert score(raw, shifted) == score(raw, plain)
    engines = []
    for picks in (plain, shifted):
        copy = build_problem()
        copy.set_assignment(problem.assignment())
        engines.append(_zero_budget_solve(copy, picks))
    assert engines[0].assignment() == engines[1].assignment() == problem.assignment()
    assert (engines[0].score.hard, engines[0].score.soft) == (engines[1].score.hard, engines[1].score.soft)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)
        assert "units" not in entry  # facility location has no scoped constraint


def _random_instance(seed, facilities=6, consumers=24):
    rng = random.Random(seed)
    return fl.FacilityLocationProblem(
        [
            fl.Facility(
                f"F{i}",
                fl.Location(rng.uniform(51.44, 51.56), rng.uniform(-0.16, -0.01)),
                rng.randint(20000, 80000),
                90,
            )
            for i in range(facilities)
        ],
        [
            fl.Consumer(f"C{i}", fl.Location(rng.uniform(51.44, 51.56), rng.uniform(-0.16, -0.01)), 15)
            for i in range(consumers)
        ],
    )


@pytest.mark.parametrize(
    "picks",
    [
        [FacilityCapacityCost(), FacilitySetupCost(), *REPEAT],
        [
            FacilityCapacityCost(),
            FacilityCapacityCost(level="soft", offset=1, clip=False, weight=2),
            FacilityCapacityCost(offset=1, weight=2),
            FacilitySetupCost(offset=30000),
            FacilitySetupCost(offset=-1000, clip=False, weight=0.25),
            DistanceFromFacilityCost(),
            DistanceFromFacilityCost(offset=4000, exponent=2, weight=0.001, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    problem = _random_instance(5)
    config = fl.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 20000
    config.spent_limit_seconds = 30
    fl.Solver(config).solve_with_constraints(problem, picks)
    raw = fl.constraint_costs(problem, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)
    assert score(fl.constraint_costs(problem), picks) == python  # the default costs score the repeats too


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [3.0, -3.0])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: FacilityCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: FacilityCapacityCost(level="hard", offset=offset, clip=False),
        lambda offset: DistanceFromFacilityCost(level="hard", offset=offset, clip=False),
    ],
    ids=["capacity-default-hard", "capacity-level-hard", "distance-moved-to-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward the facilities below it, so one facility's spare
    capacity would cancel another's overload and an infeasible solution could score
    hard 0; a negative one would charge every facility, an unused one included. The
    engine (solve, Solver, constraint_costs), the score() mirror and the CP-SAT
    reference refuse it with the same message."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    picks = [FacilityCapacityCost(), spec] if spec.name == "facility_capacity_cost" else [spec]
    assert _refusal(lambda: fl.solve(build_problem(), time_limit_seconds=0.1, constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget_solve(_two_assigned(), picks)) == message  # a repeat too
    assert _refusal(lambda: fl.constraint_costs(_two_assigned(), [spec])) == message
    assert _refusal(lambda: score(fl.constraint_costs(_two_assigned()), [spec])) == message
    assert _refusal(lambda: score(fl.constraint_costs(_two_assigned()), picks)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """(2 - offset) + (0 - offset) per facility, on soft, on every path."""
    pick = FacilityCapacityCost(level="soft", offset=offset, clip=False)
    expected = (0, -(2 - offset + 0 - offset))
    assert score(fl.constraint_costs(_two_assigned()), [pick]) == expected
    raw = fl.constraint_costs(_two_assigned(), [pick])
    assert (raw["facility_capacity_cost"]["level"], raw["facility_capacity_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    engine = _zero_budget_solve(_two_assigned(), [pick])
    assert (engine.score.hard, engine.score.soft) == expected


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (FacilityCapacityCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold
        (FacilityCapacityCost(offset=-3.0), (-5, 0.0)),  # 2 + 3 on the overloaded facility only
        (FacilityCapacityCost(clip=False), (-2, 0.0)),  # clip=False with offset 0: the 0.2.0 shaping
        (FacilityCapacityCost(offset=0.0, clip=False), (-2, 0.0)),
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(fl.constraint_costs(_two_assigned()), [pick]) == expected
    engine = _zero_budget_solve(_two_assigned(), [pick])
    assert (engine.score.hard, engine.score.soft) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(
        name="facility_capacity_cost", weight=None, level="soft", exponent=1.0, offset=3.0, clip=False
    )
    assert _zero_budget_solve(_two_assigned(), [moved]).score.soft == 4.0
    kept = SimpleNamespace(name="facility_capacity_cost", weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q="'facility_capacity_cost'")
    assert _refusal(lambda: _zero_budget_solve(_two_assigned(), [kept])) == message
    assert _refusal(lambda: score(fl.constraint_costs(_two_assigned()), [kept])) == message
