"""flight_crew.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the five knobs
(the level included) and repeated picks.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import flight_crew as fc
from flight_crew._constraint import instance_keys
from flight_crew.constraints import (
    Constraint,
    EmployeeUnavailabilityCost,
    FirstAssignmentNotDepartingFromHomeCost,
    FlightConflictCost,
    LastAssignmentNotArrivingAtHomeCost,
    Level,
    RequiredSkillCost,
    TransferBetweenTwoFlightsCost,
    employee_unavailability_cost,
    first_assignment_not_departing_from_home_cost,
    flight_conflict_cost,
    get_constraints,
    last_assignment_not_arriving_at_home_cost,
    required_skill_cost,
    score,
    transfer_between_two_flights_cost,
)

HOUR = 60  # in minutes


def build_schedule():
    """Two NRT-based pilots, while every round trip starts and ends at ICN.

    So whatever the assignment, the first departure and last arrival are not the home
    airport and both soft terms always remain — a small instance where dropping a term or
    changing a value shows a difference.
    """
    airports = [
        fc.Airport("ICN", "Incheon", 37.46, 126.44),
        fc.Airport("NRT", "Narita", 35.77, 140.39),
    ]
    employees = [
        fc.Employee("P1", "Pilot 1", home_airport=1, skills=["Pilot"]),
        fc.Employee("P2", "Pilot 2", home_airport=1, skills=["Pilot"]),
        fc.Employee("X1", "No skill", home_airport=1, skills=[]),
    ]
    # Day one: ICN->NRT, NRT->ICN. Day two: the same round trip once more.
    flights = [
        fc.Flight("KE001", 0, 9 * HOUR, 1, 11 * HOUR),
        fc.Flight("KE002", 1, 13 * HOUR, 0, 15 * HOUR),
        fc.Flight("KE003", 0, 33 * HOUR, 1, 35 * HOUR),
        fc.Flight("KE004", 1, 37 * HOUR, 0, 39 * HOUR),
    ]
    assignments = [fc.FlightAssignment(f"a{i}", i, required_skill="Pilot") for i in range(4)]
    return fc.FlightCrewSchedule(airports, employees, flights, assignments)


def solve(constraints=None):
    schedule = build_schedule()
    fc.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = fc.list_constraints()
    assert available == [
        "required_skill_cost",
        "flight_conflict_cost",
        "transfer_between_two_flights_cost",
        "employee_unavailability_cost",
        "first_assignment_not_departing_from_home_cost",
        "last_assignment_not_arriving_at_home_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == fc.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the home airport terms disappear entirely."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve([required_skill_cost, flight_conflict_cost, employee_unavailability_cost])
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the skill weight grows hard while the soft makeup stays put."""
    picks = [first_assignment_not_departing_from_home_cost]
    base = solve([*picks, required_skill_cost])
    heavier = solve([*picks, RequiredSkillCost(weight=10_000)])
    assert heavier.hard <= base.hard  # penalty, so equal or more negative


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft moves its penalty to the soft level."""
    hard_level = solve([flight_conflict_cost, last_assignment_not_arriving_at_home_cost])
    soft_level = solve([FlightConflictCost(level="soft"), last_assignment_not_arriving_at_home_cost])
    # Only the level carrying the same cost differs.
    assert hard_level.hard <= 0
    assert soft_level.hard == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "required_skill_costs"  # trailing s
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        fc.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    instances = (
        required_skill_cost,
        flight_conflict_cost,
        transfer_between_two_flights_cost,
        employee_unavailability_cost,
        first_assignment_not_departing_from_home_cost,
        last_assignment_not_arriving_at_home_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert list(instances) == get_constraints()
    assert [spec.name for spec in instances] == fc.list_constraints()


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    fc.solve(schedule, time_limit_seconds=0.3, random_seed=1)
    return fc.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = fc.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out.

    Hard rounding happens per unit contribution, so splitting by constraint and
    summing gives the same value. "How much does this term add to the score"
    can be read without rescoring."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """Changing only the weight scales that term linearly — no rescoring."""
    raw, _ = _raw()
    base = score(raw, [required_skill_cost])
    ten = score(raw, [RequiredSkillCost(weight=10)])
    default_weight = raw[required_skill_cost.name]["weight"]
    factor = 10 / default_weight
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [required_skill_cost],
        [RequiredSkillCost(weight=500), flight_conflict_cost, last_assignment_not_arriving_at_home_cost],
        [
            FlightConflictCost(level="soft"),
            TransferBetweenTwoFlightsCost(level="soft"),
            last_assignment_not_arriving_at_home_cost,
        ],
        [required_skill_cost, FirstAssignmentNotDepartingFromHomeCost(level="hard", weight=0.5)],
        [LastAssignmentNotArrivingAtHomeCost(exponent=2.0), EmployeeUnavailabilityCost(exponent=2.0)],
        [*get_constraints(), FlightConflictCost(weight=3, exponent=2.0)],
        [
            RequiredSkillCost(),
            RequiredSkillCost(level="soft", offset=1, clip=False, weight=40),
            RequiredSkillCost(offset=1, weight=40),
            FlightConflictCost(offset=-1),
            TransferBetweenTwoFlightsCost(level="soft", offset=1, clip=False, weight=5),
            TransferBetweenTwoFlightsCost(offset=1, weight=5),
            FirstAssignmentNotDepartingFromHomeCost(),
            FirstAssignmentNotDepartingFromHomeCost(weight=250, offset=0.5, clip=False),
            LastAssignmentNotArrivingAtHomeCost(offset=1),
        ],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per employee, so reproducing it needs the per-employee raw costs —
    totals alone are not enough."""
    schedule = build_schedule()
    fc.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=specs)
    python = score(fc.constraint_costs(schedule), specs)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    if specs is not None:
        picked = fc.constraint_costs(schedule, specs)
        assert len(picked) == len(specs)
        assert score(picked, specs) == python  # the picks' own costs, repeats keyed name#k


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [RequiredSkillCost(), RequiredSkillCost(weight=10)]
    assert _zero_budget(picks) == (-(100 + 10), 0.0)  # X1's one unqualified seat, twice
    assert get_constraints([FlightConflictCost(), FlightConflictCost()]) == [
        FlightConflictCost(),
        FlightConflictCost(),
    ]


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = fc.constraint_info()
    assert [row.name for row in rows] == fc.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 100.0, True, False, True),
        ("hard", 10.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("hard", 10.0, True, False, True),
        ("soft", 1000.0, True, False, True),
        ("soft", 1000.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = fc.constraint_info()[0]
    assert isinstance(row, fc.ConstraintInfo)
    assert row._asdict() == {
        "name": "required_skill_cost",
        "default_level": "hard",
        "default_weight": 100.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='required_skill_cost'")
    assert pickle.loads(pickle.dumps(fc.constraint_info())) == fc.constraint_info()
    assert fc.ConstraintInfo.__module__ == fc._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = fc.constraint_costs(build_schedule())
    assert list(raw) == fc.list_constraints()
    for row in fc.constraint_info():
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
    """weight=None scores exactly like the explicit default weights 100 / 10 / 1 / 10 / 1000 / 1000."""
    explicit = [
        RequiredSkillCost(weight=100),
        FlightConflictCost(weight=10),
        TransferBetweenTwoFlightsCost(weight=1),
        EmployeeUnavailabilityCost(weight=10),
        FirstAssignmentNotDepartingFromHomeCost(weight=1000),
        LastAssignmentNotArrivingAtHomeCost(weight=1000),
    ]
    raw, schedule = _raw()
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (schedule.score.hard, schedule.score.soft)


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
        RequiredSkillCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "flight_conflict_cost"
        weight = -2.0
        level = None
        exponent = 1.0

    class Flat:
        name = "flight_conflict_cost"
        weight = None
        level = None
        exponent = 0.0

    class Unbounded:
        name = "flight_conflict_cost"
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
            fc.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        FlightConflictCost(offset=float("nan"))


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
    pick = SimpleNamespace(name="flight_conflict_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'flight_conflict_cost': {re.escape(message)}"):
        fc.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        FlightConflictCost(**knobs)


@pytest.mark.parametrize(
    "old",
    [
        "required_skill",
        "flight_conflict",
        "transfer_between_two_flights",
        "employee_unavailability",
        "first_assignment_not_departing_from_home",
        "last_assignment_not_arriving_at_home",
    ],
)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve([pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(fc.constraint_costs(build_schedule()), [pick])


# --- Raw costs by hand ------------------------------------------------------------


def test_constraint_costs_match_every_constraint_by_hand():
    """Each raw component is checked without running a solver (per employee P1, P2, X1).

    P1 (home NRT) flies F0 ICN->NRT [9h, 11h) and F1 NRT->ICN [10h, 12h): one overlapping
    pair, a connected chain, the first leaves ICN and the last lands at ICN (both not home).
    P2 (home ICN, unavailable on day 1) flies F2 ICN->KIX [33h, 35h) and F3 ICN->NRT
    [37h, 39h), both on day 1: two unavailable assignments, one broken connection (lands
    at KIX, leaves from ICN), starts at home, ends at NRT.
    X1 (home NRT, no skill) flies F4 NRT->NRT on day 2: one seat without the skill.
    """
    airports = [fc.Airport("ICN"), fc.Airport("NRT"), fc.Airport("KIX")]
    employees = [
        fc.Employee("P1", home_airport=1, skills=["Pilot"]),
        fc.Employee("P2", home_airport=0, skills=["Pilot"], unavailable_days={1}),
        fc.Employee("X1", home_airport=1, skills=[]),
    ]
    flights = [
        fc.Flight("F0", 0, 9 * HOUR, 1, 11 * HOUR),
        fc.Flight("F1", 1, 10 * HOUR, 0, 12 * HOUR),
        fc.Flight("F2", 0, 33 * HOUR, 2, 35 * HOUR),
        fc.Flight("F3", 0, 37 * HOUR, 1, 39 * HOUR),
        fc.Flight("F4", 1, 50 * HOUR, 1, 52 * HOUR),
    ]
    assignments = [fc.FlightAssignment(f"a{i}", i, required_skill="Pilot") for i in range(5)]
    schedule = fc.FlightCrewSchedule(airports, employees, flights, assignments)
    schedule.set_assignment([0, 0, 1, 1, 2])

    raw = fc.constraint_costs(schedule)

    assert raw["required_skill_cost"]["costs"] == [0.0, 0.0, 1.0]
    assert raw["flight_conflict_cost"]["costs"] == [1.0, 0.0, 0.0]
    assert raw["transfer_between_two_flights_cost"]["costs"] == [0.0, 1.0, 0.0]
    assert raw["employee_unavailability_cost"]["costs"] == [0.0, 2.0, 0.0]
    assert raw["first_assignment_not_departing_from_home_cost"]["costs"] == [1.0, 0.0, 0.0]
    assert raw["last_assignment_not_arriving_at_home_cost"]["costs"] == [1.0, 1.0, 0.0]
    # Default weights: 100 / 10 / 1 / 10 hard, 1000 / 1000 soft.
    assert score(raw) == (-(100 * 1 + 10 * 1 + 1 * 1 + 10 * 2), -(1000 * 1 + 1000 * 2))
    engine = fc.calculate_score(schedule)
    assert (engine.hard, engine.soft) == score(raw)

    # The zero case: an unassigned schedule violates nothing.
    schedule.set_assignment([-1] * 5)
    for name, entry in fc.constraint_costs(schedule).items():
        assert entry["costs"] == [0.0, 0.0, 0.0], name


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _hand_schedule():
    """The by-hand schedule above, every seat assigned (P1, P2, X1).

    Raw per employee: skill [0, 0, 1], conflicts [1, 0, 0], transfers [0, 1, 0],
    unavailable [0, 2, 0], first not home [1, 0, 0], last not home [1, 1, 0].
    """
    airports = [fc.Airport("ICN"), fc.Airport("NRT"), fc.Airport("KIX")]
    employees = [
        fc.Employee("P1", home_airport=1, skills=["Pilot"]),
        fc.Employee("P2", home_airport=0, skills=["Pilot"], unavailable_days={1}),
        fc.Employee("X1", home_airport=1, skills=[]),
    ]
    flights = [
        fc.Flight("F0", 0, 9 * HOUR, 1, 11 * HOUR),
        fc.Flight("F1", 1, 10 * HOUR, 0, 12 * HOUR),
        fc.Flight("F2", 0, 33 * HOUR, 2, 35 * HOUR),
        fc.Flight("F3", 0, 37 * HOUR, 1, 39 * HOUR),
        fc.Flight("F4", 1, 50 * HOUR, 1, 52 * HOUR),
    ]
    assignments = [fc.FlightAssignment(f"a{i}", i, required_skill="Pilot") for i in range(5)]
    schedule = fc.FlightCrewSchedule(airports, employees, flights, assignments)
    schedule.set_assignment([0, 0, 1, 1, 2])
    return schedule


def _zero_budget(picks):
    """The engine's score of the hand schedule under `picks`: a solve with no time keeps it."""
    schedule = fc.solve(_hand_schedule(), time_limit_seconds=0, random_seed=1, constraints=picks)
    assert schedule.assignment() == [0, 0, 1, 1, 2]
    return schedule.score.hard, schedule.score.soft


REPEAT = [EmployeeUnavailabilityCost(), EmployeeUnavailabilityCost(weight=3, exponent=2)]


def test_a_repeated_pick_by_hand():
    """unavailable seats x 10 plus 3 x their square per employee: 10 x 2 + 3 x 4 = 32."""
    raw = fc.constraint_costs(_hand_schedule(), REPEAT)
    assert list(raw) == ["employee_unavailability_cost", "employee_unavailability_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [0.0, 2.0, 0.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (10.0, 1.0, 3.0, 2.0)
    assert first["name"] == second["name"] == "employee_unavailability_cost"
    assert second["default_weight"] == 10.0
    assert "units" not in first  # every employee is evaluated, an idle one included
    expected = (-32, 0.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(fc.constraint_costs(_hand_schedule()), REPEAT) == expected
    assert _zero_budget(REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [LastAssignmentNotArrivingAtHomeCost(), LastAssignmentNotArrivingAtHomeCost()]
    assert score(fc.constraint_costs(_hand_schedule()), picks) == (0, -4000.0)
    assert _zero_budget(picks) == (0, -4000.0)
    assert list(fc.constraint_costs(_hand_schedule(), picks)) == [
        "last_assignment_not_arriving_at_home_cost",
        "last_assignment_not_arriving_at_home_cost#2",
    ]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        RequiredSkillCost(weight=1),
        FlightConflictCost(),
        RequiredSkillCost(weight=2),
        LastAssignmentNotArrivingAtHomeCost(),
        RequiredSkillCost(weight=3),
    ]
    raw = fc.constraint_costs(_hand_schedule(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "required_skill_cost": 1.0,
        "required_skill_cost#2": 2.0,
        "required_skill_cost#3": 3.0,
        "flight_conflict_cost": 10.0,
        "last_assignment_not_arriving_at_home_cost": 1000.0,
    }
    assert [key for key in raw if key.startswith("required")] == [
        "required_skill_cost",
        "required_skill_cost#2",
        "required_skill_cost#3",
    ]
    expected = (-(6 + 10), -2000.0)
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per employee; X1 holds no unavailable seat (raw 0), and neither does P1.
        (EmployeeUnavailabilityCost(offset=1), (-10, 0.0)),
        (EmployeeUnavailabilityCost(offset=3), (0, 0.0)),
        (RequiredSkillCost(offset=-1), (-200, 0.0)),  # a negative offset adds to every non-zero employee
        # Hard rounds per employee: 1.5 -> 2 on P1 and P2 (a rounded total gives 3).
        (LastAssignmentNotArrivingAtHomeCost(level="hard", weight=1, offset=-0.5), (-4, 0.0)),
        # The literal formula with an offset is refused on hard (the default of these two),
        # so its picks sit on soft:
        (
            EmployeeUnavailabilityCost(level="soft", offset=1, clip=False),
            (0, 10.0),
        ),  # -1 + 1 - 1: the zeros are rewarded
        (EmployeeUnavailabilityCost(level="soft", offset=3, exponent=2, clip=False), (0, 190.0)),  # -9 - 1 - 9
        (RequiredSkillCost(level="soft", offset=-1, clip=False), (0, -400.0)),  # ...or to every employee
        (
            EmployeeUnavailabilityCost(level="soft", weight=2, offset=0.5, clip=False),
            (0, -1.0),
        ),  # 2 x (-0.5 + 1.5 - 0.5)
        (LastAssignmentNotArrivingAtHomeCost(offset=1, clip=False), (0, 1000.0)),  # 0 + 0 - 1, soft
        (FirstAssignmentNotDepartingFromHomeCost(offset=0.5), (0, -500.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(fc.constraint_costs(_hand_schedule()), [pick]) == expected
    raw = fc.constraint_costs(_hand_schedule(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget([pick]) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [RequiredSkillCost(), EmployeeUnavailabilityCost(exponent=2), LastAssignmentNotArrivingAtHomeCost()]
    shifted = [
        RequiredSkillCost(offset=0.0, clip=False),
        EmployeeUnavailabilityCost(exponent=2, offset=0),
        LastAssignmentNotArrivingAtHomeCost(offset=0.0, clip=False),
    ]
    raw = fc.constraint_costs(_hand_schedule())
    assert score(raw, shifted) == score(raw, plain) == (-(100 + 40), -2000.0)
    assert _zero_budget(shifted) == _zero_budget(plain) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), FlightConflictCost(weight=3, exponent=2)],
        [
            RequiredSkillCost(),
            RequiredSkillCost(level="soft", offset=1, clip=False, weight=40),
            RequiredSkillCost(offset=1, weight=40),
            FlightConflictCost(),
            FlightConflictCost(weight=2, exponent=2, offset=1),
            TransferBetweenTwoFlightsCost(offset=-1),
            EmployeeUnavailabilityCost(level="soft", offset=-2, clip=False),
            EmployeeUnavailabilityCost(offset=-2),
            FirstAssignmentNotDepartingFromHomeCost(weight=250, offset=0.5, clip=False),
            LastAssignmentNotArrivingAtHomeCost(),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    schedule = build_schedule()
    config = fc.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    fc.Solver(config).solve_with_constraints(schedule, picks)
    raw = fc.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(fc.constraint_costs(schedule), picks) == python  # the default costs score the repeats too


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([RequiredSkillCost(level="soft")], (0, -100.0)),
        ([FlightConflictCost(level=Level.SOFT)], (0, -10.0)),
        ([LastAssignmentNotArrivingAtHomeCost(level=Level.HARD, weight=2)], (-4, 0.0)),
        ([EmployeeUnavailabilityCost(level="hard")], (-20, 0.0)),  # the default level, named
        # A repeat carries its own level and shaping: the hard default, soft the seats beyond
        # one per employee (2 - 1), and soft 3 x (x - 1) per employee: 3 x (-1 + 1 - 1) = -3.
        (
            [
                EmployeeUnavailabilityCost(),
                EmployeeUnavailabilityCost(level="soft", weight=1, offset=1),
                EmployeeUnavailabilityCost(level="soft", weight=3, offset=1, clip=False),
            ],
            (-20, 2.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(fc.constraint_costs(_hand_schedule()), picks) == expected
    raw = fc.constraint_costs(_hand_schedule(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(picks) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    message = "constraint 'flight_conflict_cost': this use case has no medium level"
    pick = FlightConflictCost(level=level)  # the spec cannot know the domain
    for picks in ([pick], [FlightConflictCost(), pick]):
        with pytest.raises(ValueError, match=message):
            _zero_budget(picks)
        with pytest.raises(ValueError, match=message):
            fc.constraint_costs(_hand_schedule(), picks)
        with pytest.raises(ValueError, match=message):
            score(fc.constraint_costs(_hand_schedule()), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        FlightConflictCost(level=False)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        FlightConflictCost(level="firm")
    pick = SimpleNamespace(name="flight_conflict_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        _zero_budget([FlightConflictCost(), pick])


def test_the_knob_was_renamed():
    with pytest.raises(TypeError):
        FlightConflictCost(hard=True)


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [1.5, -1.5])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: RequiredSkillCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: FlightConflictCost(level="hard", offset=offset, clip=False),
        lambda offset: EmployeeUnavailabilityCost(offset=offset, clip=False),
        lambda offset: LastAssignmentNotArrivingAtHomeCost(level=Level.HARD, offset=offset, clip=False),
    ],
    ids=["skill-default-hard", "conflict-level-hard", "unavailability-default-hard", "last-home-moved-to-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward the employees below it, so one employee's slack would
    cancel another's violation and an infeasible schedule could score hard 0; a negative
    one would charge every employee, an idle one included. The engine (solve, Solver,
    constraint_costs) and the score() mirror refuse it with the same message; the CP-SAT
    reference's test_solve.py checks the same words."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    repeated = [type(spec)(), spec]  # a refused repeat next to its accepted first instance
    assert _refusal(lambda: fc.solve(build_schedule(), time_limit_seconds=0.1, constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget(repeated)) == message
    config = fc.SolverConfig()
    config.spent_limit_seconds = 0.1
    assert _refusal(lambda: fc.Solver(config).solve_with_constraints(_hand_schedule(), [spec])) == message
    assert _refusal(lambda: fc.constraint_costs(_hand_schedule(), [spec])) == message
    assert _refusal(lambda: fc.constraint_costs(_hand_schedule(), repeated)) == message
    assert _refusal(lambda: score(fc.constraint_costs(_hand_schedule()), [spec])) == message
    assert _refusal(lambda: score(fc.constraint_costs(_hand_schedule()), repeated)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """100 x ((0 - offset) + (0 - offset) + (1 - offset)) per employee, on soft, on every path."""
    pick = RequiredSkillCost(level="soft", offset=offset, clip=False)
    expected = (0, -100 * (1 - 3 * offset))
    assert score(fc.constraint_costs(_hand_schedule()), [pick]) == expected
    raw = fc.constraint_costs(_hand_schedule(), [pick])
    assert (raw["required_skill_cost"]["level"], raw["required_skill_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _zero_budget([pick]) == expected


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (RequiredSkillCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold
        (RequiredSkillCost(offset=-3.0), (-400, 0.0)),  # 100 x (1 + 3) on X1 only
        (RequiredSkillCost(clip=False), (-100, 0.0)),  # clip=False with offset 0: the 0.2.0 shaping
        (RequiredSkillCost(offset=0.0, clip=False), (-100, 0.0)),
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(fc.constraint_costs(_hand_schedule()), [pick]) == expected
    assert _zero_budget([pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(name="required_skill_cost", weight=None, level="soft", exponent=1.0, offset=3.0, clip=False)
    assert _zero_budget([moved]) == (0, 800.0)
    kept = SimpleNamespace(name="required_skill_cost", weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q="'required_skill_cost'")
    assert _refusal(lambda: _zero_budget([kept])) == message
    assert _refusal(lambda: score(fc.constraint_costs(_hand_schedule()), [kept])) == message
