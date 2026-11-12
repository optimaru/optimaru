"""optimaru.employee_scheduling.constraints — one spec per constraint, the C++ catalog and the knobs.

The specs carry the five knobs (weight / level / exponent / offset / clip) and
the domain parameters; the defaults (level, weight, reward) live in the C++ components
and are reported by `constraint_info()`. `get_constraints()` is the entry point
every use case shares; the constraint factory (`DecomposedCalculator`,
`constraint_costs`) may hold one constraint several times.
"""

import re
from types import SimpleNamespace

import pytest

import optimaru.employee_scheduling as es
from optimaru.employee_scheduling import constraints as specs
from optimaru.employee_scheduling import models
from optimaru.employee_scheduling.constraints import (
    SPEC_CLASSES,
    DayPatternCost,
    DesiredDayCost,
    DesiredShiftCost,
    EmployeeUnavailabilityCost,
    Level,
    LocationImbalanceCost,
    MaxConsecutiveDaysCost,
    MaxOneShiftPerDayCost,
    MinRestCost,
    MinWeeklyMinutesCost,
    MinWeeklyRestDaysCost,
    RequiredSkillCost,
    RestAfterLocationRunCost,
    ShiftCountImbalanceCost,
    ShiftCoverageCost,
    ShiftOverlapCost,
    UndesiredDayCost,
    get_constraints,
    score,
)

from ._reference import REFERENCE_SCORE, reference_schedule

#: Every name the factory accepts, in factory order (= CONSTRAINT_NAMES).
NAMES = [
    "required_skill_cost",
    "employee_unavailability_cost",
    "shift_overlap_cost",
    "max_one_shift_per_day_cost",
    "min_rest_cost",
    "undesired_day_cost",
    "desired_day_cost",
    "desired_shift_cost",
    "shift_count_imbalance_cost",
    "max_consecutive_days_cost",
    "min_consecutive_days_cost",
    "max_consecutive_days_off_cost",
    "rest_after_consecutive_work_cost",
    "max_weekly_minutes_cost",
    "min_weekly_minutes_cost",
    "min_weekly_rest_days_cost",
    "assignment_cost",
    "unassigned_shifts_cost",
    "shift_count_gap_cost",
    "weekly_minutes_gap_cost",
    "shift_coverage_cost",
    "location_run_length_cost",
    "rest_after_location_run_cost",
    "location_run_gap_cost",
    "worker_location_count_cost",
    "day_pattern_cost",
    "target_days_off_cost",
    "worker_day_count_in_range_cost",
    "location_imbalance_cost",
    "worker_pairing_cost",
]
DEFAULT_NAMES = [
    "required_skill_cost",
    "employee_unavailability_cost",
    "shift_overlap_cost",
    "max_one_shift_per_day_cost",
    "min_rest_cost",
    "undesired_day_cost",
    "desired_day_cost",
    "shift_count_imbalance_cost",
]
HARD = {
    "required_skill_cost",
    "employee_unavailability_cost",
    "shift_overlap_cost",
    "max_one_shift_per_day_cost",
    "min_rest_cost",
    "max_consecutive_days_cost",
    "rest_after_consecutive_work_cost",
    "max_weekly_minutes_cost",
    "min_weekly_rest_days_cost",
    "unassigned_shifts_cost",
    "rest_after_location_run_cost",
}
REWARDS = {"desired_day_cost", "desired_shift_cost"}
GLOBALS = {
    "shift_count_imbalance_cost",
    "unassigned_shifts_cost",
    "shift_count_gap_cost",
    "weekly_minutes_gap_cost",
    "shift_coverage_cost",
    "location_imbalance_cost",
    "worker_pairing_cost",
}


def _assigned_roster(seed=5):
    schedule = es.make_random_roster(days=14, shifts_per_day=4, employees=5, seed=seed)
    for emp in schedule.employees:  # the random roster has no undesired dates
        emp.undesired_dates = {2, 9}
    for index, shift in enumerate(schedule.shifts):
        shift.employee_index = (index * 3 + index // 7) % len(schedule.employees)
    return schedule


# ---------------------------------------------------------------------------
# Names and the default set
# ---------------------------------------------------------------------------


def test_list_constraints_is_every_name_the_factory_accepts():
    assert es.list_constraints() == list(es.CONSTRAINT_NAMES) == NAMES


def test_get_constraints_defaults_to_the_eight_default_specs():
    """The eight that `es::defaultConstraints()` stacks, in that order."""
    assert [spec.name for spec in get_constraints()] == DEFAULT_NAMES


def test_every_spec_is_named_after_its_class():
    assert [spec.name for spec in SPEC_CLASSES] == NAMES
    for spec in SPEC_CLASSES:
        snake = re.sub(r"(?<!^)(?=[A-Z])", "_", spec.__name__).lower()
        assert snake == spec.name


def test_the_module_level_instances_are_the_defaults():
    for spec in SPEC_CLASSES:
        instance = getattr(specs, spec.name)
        assert type(instance) is spec
        assert (instance.weight, instance.level, instance.exponent) == (None, None, 1.0)
        assert instance == spec()
        assert instance.name in specs.__all__ and spec.__name__ in specs.__all__


def test_every_spec_has_its_labels():
    hangul = re.compile("[가-힣]")
    for spec in SPEC_CLASSES:
        assert spec.label_en and spec.description_en, spec
        assert hangul.search(spec.label_ko) and hangul.search(spec.description_ko), spec
        assert "0" in spec.description_en  # says what 0 means


def test_get_constraints_reproduces_the_builtin_score():
    """A calculator assembled from the default list must match the built-in score."""
    schedule = reference_schedule()
    calculator = es.DecomposedCalculator(get_constraints())
    assert str(calculator.score(schedule)) == REFERENCE_SCORE


def test_passing_a_list_uses_it_as_given():
    """The given list is used as is: only what is in it gets scored."""
    picked = get_constraints([RequiredSkillCost(), UndesiredDayCost(2.0)])
    assert [spec.name for spec in picked] == ["required_skill_cost", "undesired_day_cost"]

    schedule = reference_schedule()
    partial = es.DecomposedCalculator(picked).score(schedule)
    full = es.DecomposedCalculator(get_constraints()).score(schedule)
    assert str(partial) != str(full)


def test_empty_list_is_rejected_not_treated_as_the_defaults():
    """An empty list is NOT the defaults: a filter that happens to come back empty
    must be an error, in Python and on the C++ path alike."""
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])
    with pytest.raises(ValueError, match="must not be empty"):
        es.constraint_costs(reference_schedule(), [])


def test_returned_list_is_a_fresh_copy():
    """A caller editing the returned list must not affect the next call."""
    first = get_constraints()
    first.pop()
    assert len(get_constraints()) == 8


def test_a_rule_may_repeat_with_other_parameters_or_the_same():
    """A constraint may repeat, with other parameters or the same ones: every pick is an instance."""
    picked = get_constraints([DayPatternCost(pattern="WORK,WORK"), DayPatternCost(pattern="OFF,OFF")])
    assert len(picked) == 2
    same = [DayPatternCost(pattern="WORK,WORK"), DayPatternCost(pattern="WORK,WORK")]
    assert get_constraints(same) == same
    scoped = get_constraints([MaxConsecutiveDaysCost(), MaxConsecutiveDaysCost(worker_indices=(0,))])
    assert len(scoped) == 2


# ---------------------------------------------------------------------------
# The catalog: constraint_info() == the C++ components' constants
# ---------------------------------------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    rows = es.constraint_info()
    # The defaults first; then the constraint-only units, then the constraint-only globals.
    rest = [name for name in NAMES if name not in DEFAULT_NAMES]
    assert [row.name for row in rows] == (
        DEFAULT_NAMES + [n for n in rest if n not in GLOBALS] + [n for n in rest if n in GLOBALS]
    )
    for row in rows:
        assert row.default_weight == 1.0, row
        assert row.default_level == ("hard" if row.name in HARD else "soft"), row
        assert row.reward is (row.name in REWARDS), row
        assert row.level_flippable is (row.name not in REWARDS), row
        assert row.default_enabled is (row.name in DEFAULT_NAMES), row


def test_the_documented_levels_match_the_engine():
    """worker_location_count_cost and worker_day_count_in_range_cost are SOFT by default."""
    rows = {row.name: row for row in es.constraint_info()}
    assert rows["worker_location_count_cost"].default_level == "soft"
    assert rows["worker_day_count_in_range_cost"].default_level == "soft"


def test_the_factory_specs_rules_and_catalog_name_the_same_constraints():
    assert set(es.CONSTRAINT_NAMES) == {row.name for row in es.constraint_info()}
    assert list(models.CONSTRAINT_MODELS) == list(es.CONSTRAINT_NAMES)
    calculator = es.DecomposedCalculator()
    for name in es.CONSTRAINT_NAMES:
        calculator.add(name)  # every name builds, at its default parameters
    calculator.score(_assigned_roster())
    assert sorted(total["name"] for total in calculator.constraint_totals()) == sorted(NAMES)


def test_constraint_costs_metadata_matches_constraint_info():
    rows = {row.name: row for row in es.constraint_info()}
    costs = es.constraint_costs(_assigned_roster(), [spec() for spec in SPEC_CLASSES])
    assert sorted(costs) == sorted(NAMES)
    for name, entry in costs.items():
        row = rows[name]
        assert entry["name"] == name
        assert entry["weight"] == row.default_weight
        assert entry["level"] == row.default_level
        assert "hard" not in entry
        assert entry["reward"] is row.reward
        assert entry["level_flippable"] is row.level_flippable
        assert entry["global"] is (name in GLOBALS)
        assert len(entry["costs"]) == (1 if name in GLOBALS else 5)
        assert "medium_fold" not in entry  # no medium level in this domain


def test_constraint_costs_defaults_to_the_eight_defaults():
    assert list(es.constraint_costs(reference_schedule())) == DEFAULT_NAMES


@pytest.mark.parametrize(
    "spec",
    [pytest.param(spec, id=spec.name) for spec in SPEC_CLASSES],
)
def test_a_default_is_the_same_on_every_path(spec):
    """The spec, the JSON constraint model and add(name) build the same component."""
    schedule = es.make_random_roster(days=21, shifts_per_day=2, employees=3, seed=5)
    for index, shift in enumerate(schedule.shifts):
        shift.employee_index = index % 2  # two people cover everything, the third rests
    named = es.DecomposedCalculator()
    named.add(spec.name)
    by_name = named.score(schedule)

    assert str(es.DecomposedCalculator([spec()]).score(schedule)) == str(by_name)
    assert str(es.DecomposedCalculator([models.constraint_model(spec)()]).score(schedule)) == str(by_name)


def test_rest_after_location_run_default_is_the_same_on_every_path():
    shifts = [es.Shift(f"Shift {day}", es.minutes(day, 9), es.minutes(day, 17), "Location B") for day in range(6)]
    for shift in shifts:
        shift.employee_index = 0
    shifts[2].location = "Location A"
    schedule = es.EmployeeSchedule([es.Employee("A")], shifts)

    named = es.DecomposedCalculator()
    named.add("rest_after_location_run_cost", location="Location B")
    by_name = named.score(schedule)

    assert by_name.hard == -1  # the day after the first stint is worked
    constraint = models.constraint_model(RestAfterLocationRunCost)(location="Location B")
    assert str(es.DecomposedCalculator([constraint]).score(schedule)) == str(by_name)
    assert str(es.DecomposedCalculator([RestAfterLocationRunCost(location="Location B")]).score(schedule)) == str(
        by_name
    )


def test_the_spec_parameter_drift_is_fixed():
    """per_day, week_anchor (MinWeeklyMinutesCost included) reach the engine."""
    assert {"per_day"} <= set(UndesiredDayCost().params()) and {"per_day"} <= set(DesiredDayCost().params())
    assert MinWeeklyMinutesCost().params()["week_anchor"] == 0
    # Two 10-hour shifts on days 6 and 7: one week from anchor 1, two weeks from day 0.
    employee = es.Employee("A")
    shifts = [es.Shift("s6", es.minutes(6, 0), es.minutes(6, 10)), es.Shift("s7", es.minutes(7, 0), es.minutes(7, 10))]
    for shift in shifts:
        shift.employee_index = 0
    schedule = es.EmployeeSchedule([employee], shifts)
    anchored = es.DecomposedCalculator([MinWeeklyMinutesCost(minutes=1500, week_anchor=1)]).score(schedule)
    day0 = es.DecomposedCalculator([MinWeeklyMinutesCost(minutes=1500)]).score(schedule)
    assert anchored.soft == -300  # one week of 1200
    assert day0.soft == -(900 + 900)  # two weeks of 600


def test_the_run_repair_default_is_the_same_on_both_sides():
    assert es.SolverConfig().employee_run_repair_max_days == models.SolverSettings().employee_run_repair_max_days


# ---------------------------------------------------------------------------
# The knobs
# ---------------------------------------------------------------------------


def test_specs_carry_the_documented_defaults():
    """Spec knobs default to weight None (= the C++ default), hard None, exponent 1.0."""
    for spec in get_constraints():
        assert spec.weight is None
        assert spec.level is None
        assert spec.exponent == pytest.approx(1.0)


def test_weight_none_scores_like_the_explicit_default_weights():
    schedule = reference_schedule()
    explicit = [type(spec)(weight=1.0) for spec in get_constraints()]
    raw = es.constraint_costs(schedule)
    assert score(raw) == score(raw, explicit)
    builtin = es.calculate_score(schedule)
    assert score(raw) == (builtin.hard, pytest.approx(builtin.soft))
    assert str(es.DecomposedCalculator(explicit).score(schedule)) == REFERENCE_SCORE


def test_score_reprices_raw_costs_like_the_engine():
    schedule = _assigned_roster()
    selection = [
        RequiredSkillCost(weight=2.0),
        MinRestCost(weight=0.25, level="soft"),
        UndesiredDayCost(level="hard"),
        DesiredDayCost(exponent=2.0),
        ShiftCountImbalanceCost(weight=3.0, exponent=2.0),
        DayPatternCost(pattern="WORK,WORK,WORK"),
        DayPatternCost(pattern="OFF,WORK", weight=2.0),
        MaxConsecutiveDaysCost(value=2, worker_indices=(0, 1)),
        ShiftCoverageCost(min_count=2, level="hard"),
    ]
    raw = es.constraint_costs(schedule, selection)
    engine = es.DecomposedCalculator(selection).score(schedule)
    priced = score(raw, selection)
    assert priced.hard == engine.hard
    assert priced.soft == pytest.approx(engine.soft)


def test_repeated_rules_are_keyed_by_instance_and_labelled():
    selection = [DayPatternCost(pattern="WORK,WORK"), MinRestCost(), DayPatternCost(pattern="OFF,OFF")]
    raw = es.constraint_costs(_assigned_roster(), selection)
    assert list(raw) == ["day_pattern_cost", "min_rest_cost", "day_pattern_cost#2"]
    assert specs.instance_keys(selection) == ["day_pattern_cost", "min_rest_cost", "day_pattern_cost#2"]
    assert [raw[key]["label"] for key in raw] == ["Day pattern[WORK,WORK]", "", "Day pattern[OFF,OFF]"]
    assert raw["day_pattern_cost#2"]["name"] == "day_pattern_cost"


@pytest.mark.parametrize(
    "pick",
    [
        DesiredDayCost(level="hard"),
        DesiredDayCost(level="hard", worker_indices=(0,)),
        DesiredShiftCost(level="hard"),
        DesiredShiftCost(level="hard", shift_indices=(0, 1)),
    ],
    ids=["desired-day", "scoped-desired-day", "desired-shift", "scoped-desired-shift"],
)
def test_a_reward_refuses_hard_on_every_path(pick):
    schedule = reference_schedule()
    with pytest.raises(ValueError, match="is a reward"):
        es.DecomposedCalculator([pick])
    with pytest.raises(ValueError, match="is a reward"):
        es.constraint_costs(schedule, [pick])
    with pytest.raises(ValueError, match="is a reward"):
        named = es.DecomposedCalculator()
        named.add(pick.name, level="hard", **pick.params())
    with pytest.raises(ValueError, match="is a reward"):
        score(es.constraint_costs(schedule, [type(pick)(**pick.params())]), [pick])


def test_a_reward_stays_soft_and_scoped_rewards_keep_their_metadata():
    schedule = reference_schedule()
    assert str(es.DecomposedCalculator([DesiredDayCost(level="soft")]).score(schedule)) == str(
        es.DecomposedCalculator([DesiredDayCost()]).score(schedule)
    )
    (entry,) = es.constraint_costs(schedule, [DesiredDayCost(worker_indices=(0,))]).values()
    assert entry["reward"] is True and entry["level_flippable"] is False and entry["level"] == "soft"
    assert all(cost <= 0 for cost in entry["costs"])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    pick = SimpleNamespace(name="min_rest_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'min_rest_cost': {re.escape(message)}"):
        es.DecomposedCalculator([pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        es.DecomposedCalculator().add("min_rest_cost", **knobs)
    with pytest.raises(TypeError, match=re.escape(message)):
        MinRestCost(**knobs)


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": -2.0}, "weight must be finite and >= 0"),
        ({"exponent": 0.0}, "exponent must be finite and > 0"),
        ({"offset": float("inf")}, "offset must be finite, not inf"),
        ({"offset": float("nan")}, "offset must be finite, not nan"),
    ],
)
def test_core_refuses_invalid_knobs_from_duck_typed_picks(knobs, message):
    pick = SimpleNamespace(name="shift_overlap_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(ValueError, match=message):
        es.DecomposedCalculator([pick])
    with pytest.raises(ValueError, match=message):
        es.DecomposedCalculator().add("shift_overlap_cost", **knobs)
    with pytest.raises(ValueError, match=message):
        ShiftOverlapCost(**knobs)


def test_an_unknown_name_is_refused():
    with pytest.raises(ValueError, match="unknown constraint 'required_skill'; available: required_skill_cost, "):
        es.DecomposedCalculator().add("required_skill")


def test_a_scope_is_a_parameter_of_unit_rules_and_shift_coverage_only():
    with pytest.raises(TypeError):
        ShiftCountImbalanceCost(worker_indices=(0,))
    with pytest.raises(ValueError, match="not yet supported for the global constraint 'shift_count_imbalance_cost'"):
        es.DecomposedCalculator().add("shift_count_imbalance_cost", worker_indices=[0])
    assert ShiftCoverageCost(worker_indices=(0,)).params()["worker_indices"] == (0,)
    assert MaxOneShiftPerDayCost(shift_indices=(1, 2)).params()["shift_indices"] == (1, 2)
    assert EmployeeUnavailabilityCost().params() == {"worker_indices": None, "shift_indices": None}


@pytest.mark.parametrize(
    ("spec", "fields"),
    [
        (MaxConsecutiveDaysCost, {"value": 0}),
        (MinWeeklyRestDaysCost, {"value": 8}),
        (MinRestCost, {"min_gap_minutes": -1}),
        (ShiftCoverageCost, {"max_count": -2}),
    ],
)
def test_the_spec_bounds_are_checked(spec, fields):
    with pytest.raises(ValueError, match=f"constraint '{spec.name}': {next(iter(fields))} must be"):
        spec(**fields)


# ---------------------------------------------------------------------------
# Repeated picks and the offset / clip knobs
# ---------------------------------------------------------------------------
# The reference schedule's raw costs per employee (Worker 01..04): undesired
# minutes [0, 480, 0, 0] (soft), desired minutes [-600, 0, 0, -239] (a soft
# reward), unavailable minutes [480, 0, 0, 0] (hard), min rest [0, 0, 0, 0]
# (hard); shift_count_imbalance_cost is global, sqrt(15 - 49/4) = 1.65831.

REPEAT = [UndesiredDayCost(), UndesiredDayCost(weight=0.5, exponent=2)]


def _engine(picks, schedule=None):
    result = es.DecomposedCalculator(picks).score(schedule or reference_schedule())
    return result.hard, result.soft


def test_a_repeated_pick_by_hand():
    """undesired minutes x 1 plus 0.5 x undesired minutes^2 per employee: 480 + 0.5 x 480^2."""
    raw = es.constraint_costs(reference_schedule(), REPEAT)
    assert list(raw) == ["undesired_day_cost", "undesired_day_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [0.0, 480.0, 0.0, 0.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "undesired_day_cost"
    assert first["label"] == second["label"]
    expected = (0, -(480 + 0.5 * 480**2))
    assert expected == (0, -115680.0)
    assert score(raw, REPEAT) == expected
    assert _engine(REPEAT) == expected
    # A constraint with domain parameters (here per_day and the scope) needs its own
    # "#2" entry: the default costs hold it once.
    with pytest.raises(ValueError, match="picked more often than the costs hold it"):
        score(es.constraint_costs(reference_schedule()), REPEAT)


def test_identical_picks_count_twice():
    picks = [UndesiredDayCost(), UndesiredDayCost()]
    raw = es.constraint_costs(reference_schedule(), picks)
    assert list(raw) == ["undesired_day_cost", "undesired_day_cost#2"]
    assert score(raw, picks) == _engine(picks) == (0, -960.0)
    totals = es.DecomposedCalculator(picks)
    totals.score(reference_schedule())
    assert [(t["name"], t["cost"]) for t in totals.constraint_totals()] == [
        ("undesired_day_cost", 480.0),
        ("undesired_day_cost", 480.0),
    ]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        UndesiredDayCost(weight=1),
        RequiredSkillCost(),
        UndesiredDayCost(weight=2),
        DesiredDayCost(),
        UndesiredDayCost(weight=3),
    ]
    raw = es.constraint_costs(reference_schedule(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "undesired_day_cost": 1.0,
        "required_skill_cost": 1.0,
        "undesired_day_cost#2": 2.0,
        "desired_day_cost": 1.0,
        "undesired_day_cost#3": 3.0,
    }
    assert list(raw) == specs.instance_keys(picks)
    assert score(raw, picks) == _engine(picks) == (-1, -(6 * 480 - 839))


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Unit constraints, shaped per employee (four employees, raw [0, 480, 0, 0]).
        (UndesiredDayCost(offset=100), (0, -380.0)),
        (UndesiredDayCost(offset=100, clip=False), (0, -(380 - 3 * 100))),  # every employee
        (UndesiredDayCost(offset=-10), (0, -490.0)),  # a negative offset adds to a non-zero cost
        (UndesiredDayCost(offset=-10, clip=False), (0, -(490 + 3 * 10))),  # ...or to every cost
        (UndesiredDayCost(offset=400, exponent=2), (0, -(80.0**2))),
        # A worker scope: only employees 1 and 3 are evaluated (the literal charges two, not four).
        (UndesiredDayCost(offset=100, clip=False, worker_indices=(1, 3, 3, 9)), (0, -(380 - 100))),
        # The reward (raw [-600, 0, 0, -239]): a threshold rewards only beyond the offset,
        # a literal one shifts every employee further.
        (DesiredDayCost(offset=200), (0, 400 + 39)),
        (DesiredDayCost(offset=200, clip=False), (0, 800 + 2 * 200 + 439)),
        # Hard, rounded per employee: 3 x 479.5 = 1438.5 -> 1439.
        (EmployeeUnavailabilityCost(offset=0.5, weight=3), (-1439, 0.0)),
        (EmployeeUnavailabilityCost(offset=80), (-400, 0.0)),
        # The literal formula with an offset is refused on hard; on soft it charges every
        # employee, not rounded: 3 x 479.5 + 3 x 3 x -0.5 = 1434.
        (EmployeeUnavailabilityCost(level="soft", offset=0.5, clip=False, weight=3), (0, -(1438.5 - 4.5))),
        # min rest is 0 everywhere: the literal formula (on soft) charges every evaluated employee.
        (MinRestCost(level="soft", offset=5, clip=False), (0, 20.0)),
        (MinRestCost(offset=5), (0, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    raw = es.constraint_costs(reference_schedule(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected


def test_a_global_total_is_shaped_once():
    raw = es.constraint_costs(reference_schedule())
    (total,) = raw["shift_count_imbalance_cost"]["costs"]
    for pick, expected in (
        (ShiftCountImbalanceCost(offset=1), -(total - 1)),
        (ShiftCountImbalanceCost(offset=2), 0.0),
        (ShiftCountImbalanceCost(offset=2, clip=False), -(total - 2)),
        (ShiftCountImbalanceCost(offset=1, exponent=2), -((total - 1) ** 2)),
    ):
        python = score(es.constraint_costs(reference_schedule(), [pick]), [pick])
        assert python == _engine([pick])
        assert python == (0, pytest.approx(expected, abs=1e-12))


def test_a_scoped_rule_reports_and_charges_exactly_its_units():
    """A worker-scoped constraint evaluates its in-range employees only (ascending, each once)."""
    pick = MinRestCost(level="soft", offset=5, clip=False, worker_indices=(2, 0, 2, 9, -1))
    raw = es.constraint_costs(reference_schedule(), [pick])
    (entry,) = raw.values()
    assert entry["units"] == [0, 2]
    assert entry["costs"] == [0.0, 0.0, 0.0, 0.0]
    assert score(raw, [pick]) == _engine([pick]) == (0, 2 * 5.0)  # 2 evaluated employees x -(0 - 5)
    unscoped = es.constraint_costs(reference_schedule(), [MinRestCost()])
    assert "units" not in unscoped["min_rest_cost"]
    shift_scoped = es.constraint_costs(reference_schedule(), [MinRestCost(shift_indices=(0, 1))])
    assert "units" not in shift_scoped["min_rest_cost"]  # a shift scope evaluates every employee


def test_a_second_reward_pick_still_refuses_hard():
    picks = [DesiredDayCost(), DesiredDayCost(level="hard", offset=10)]
    with pytest.raises(ValueError, match="is a reward"):
        es.DecomposedCalculator(picks)
    with pytest.raises(ValueError, match="is a reward"):
        score(es.constraint_costs(reference_schedule(), [DesiredDayCost(), DesiredDayCost()]), picks)


# Each pick carries its own level: undesired minutes [0, 480, 0, 0] (soft), unavailable
# minutes [480, 0, 0, 0] (hard). The second undesired pick is hard beyond 400 minutes, the
# second unavailability pick soft, literal beyond 80 at weight 0.5 (every employee charged).
LEVELS = [
    UndesiredDayCost(),
    UndesiredDayCost(level=Level.HARD, offset=400),
    EmployeeUnavailabilityCost(),
    EmployeeUnavailabilityCost(level="soft", weight=0.5, offset=80, clip=False),
]


def test_each_repeat_carries_its_own_level():
    """hard: (480 - 400) + 480 = 560; soft: 480 + 0.5 x ((480 - 80) + 3 x (0 - 80)) = 560."""
    raw = es.constraint_costs(reference_schedule(), LEVELS)
    assert {key: entry["level"] for key, entry in raw.items()} == {
        "undesired_day_cost": "soft",
        "undesired_day_cost#2": "hard",
        "employee_unavailability_cost": "hard",
        "employee_unavailability_cost#2": "soft",
    }
    expected = (-((480 - 400) + 480), -(480 + 0.5 * ((480 - 80) + 3 * (0 - 80))))
    assert expected == (-560, -560.0)
    assert score(raw, LEVELS) == expected
    assert _engine(LEVELS) == expected
    by_name = es.DecomposedCalculator()
    for pick in LEVELS:
        knobs = {knob: getattr(pick, knob) for knob in ("weight", "level", "exponent", "offset", "clip")}
        by_name.add(pick.name, **knobs, **pick.params())
    result = by_name.score(reference_schedule())
    assert (result.hard, result.soft) == expected
    assert [(t["name"], t["level"]) for t in by_name.constraint_totals()] == [
        ("undesired_day_cost", "soft"),
        ("undesired_day_cost", "hard"),
        ("employee_unavailability_cost", "hard"),
        ("employee_unavailability_cost", "soft"),
    ]


@pytest.mark.parametrize("level", [Level.MEDIUM, "medium"])
def test_medium_is_refused_on_every_path(level):
    """Employee scheduling has no medium level: a pick asking for it raises before scoring,
    in the calculator, in add(), in constraint_costs() and in score(), also as a repeat."""
    for picks in ([UndesiredDayCost(level=level)], [RequiredSkillCost(), RequiredSkillCost(level=level)]):
        with pytest.raises(ValueError, match="this use case has no medium level"):
            es.DecomposedCalculator(picks)
        with pytest.raises(ValueError, match="this use case has no medium level"):
            es.constraint_costs(reference_schedule(), picks)
        default_level = [type(pick)(**pick.params()) for pick in picks]  # a constraint needs its own "#2"
        with pytest.raises(ValueError, match="this use case has no medium level"):
            score(es.constraint_costs(reference_schedule(), default_level), picks)
    with pytest.raises(ValueError, match="this use case has no medium level"):
        es.DecomposedCalculator().add("undesired_day_cost", level=level)


def test_add_takes_the_offset_and_clip_knobs():
    by_name = es.DecomposedCalculator()
    by_name.add("undesired_day_cost")
    by_name.add("undesired_day_cost", weight=0.5, exponent=2.0, offset=100, clip=False)
    picks = [UndesiredDayCost(), UndesiredDayCost(weight=0.5, exponent=2.0, offset=100, clip=False)]
    assert by_name.score(reference_schedule()).soft == _engine(picks)[1]
    with pytest.raises(TypeError):
        es.DecomposedCalculator().add("undesired_day_cost", 1.0, None, 1.0, 100.0)  # keyword-only


def test_offset_zero_reproduces_the_plain_scores():
    """offset 0 is the plain shaping in both modes, so a selection scores the same with or without it."""
    plain = [*get_constraints(), UndesiredDayCost(exponent=2), DayPatternCost(pattern="WORK,WORK")]
    defaults = [type(spec)(offset=0.0, clip=False) for spec in get_constraints()]  # every default, offset 0
    shifted = [
        *defaults,
        UndesiredDayCost(exponent=2, offset=0),
        DayPatternCost(pattern="WORK,WORK", offset=0.0, clip=False),
    ]
    schedule = _assigned_roster()
    assert score(es.constraint_costs(schedule, shifted), shifted) == score(es.constraint_costs(schedule, plain), plain)
    assert _engine(plain, schedule) == _engine(shifted, schedule)
    assert str(es.DecomposedCalculator(defaults).score(reference_schedule())) == REFERENCE_SCORE
    for entry in es.constraint_costs(schedule).values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


PICKS = {
    "repeat": [*get_constraints(), *REPEAT, DayPatternCost(pattern="WORK,WORK"), DayPatternCost(pattern="WORK,WORK")],
    "repeat-and-offsets": [
        *get_constraints(),
        RequiredSkillCost(level="soft", offset=1, clip=False, weight=2),
        RequiredSkillCost(offset=1, weight=2),
        UndesiredDayCost(offset=200, exponent=2, weight=0.01),
        UndesiredDayCost(offset=-30, clip=False, weight=0.5),
        DesiredDayCost(offset=60, clip=False),
        ShiftCountImbalanceCost(offset=0.5, clip=False, weight=2),
        MaxConsecutiveDaysCost(value=3, offset=1, worker_indices=(0, 2)),
        MaxConsecutiveDaysCost(value=3, level="soft", offset=2, clip=False, worker_indices=(1,)),
        DayPatternCost(pattern="WORK,OFF,WORK", offset=-1),
        LocationImbalanceCost(offset=1, clip=False),
    ],
}


@pytest.mark.parametrize("picks", list(PICKS.values()), ids=list(PICKS))
def test_score_matches_the_engine_for_repeated_and_shifted_picks(picks):
    schedule = _assigned_roster()
    raw = es.constraint_costs(schedule, picks)
    # The calculator lists unit constraints before global ones; the keys are the picks'.
    assert sorted(raw) == sorted(specs.instance_keys(picks))
    priced = score(raw, picks)
    engine = es.DecomposedCalculator(picks).score(schedule)
    assert priced.hard == engine.hard
    assert priced.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize("picks", list(PICKS.values()), ids=list(PICKS))
def test_a_solve_with_repeated_and_shifted_picks_reports_the_mirror_score(picks):
    """The incremental search keeps the repeated, shifted instances apart: the reported
    score is the Python mirror's of the final roster (and a fresh recompute's)."""
    schedule = es.make_random_roster(days=14, shifts_per_day=4, employees=5, seed=11)
    config = es.SolverConfig()
    config.random_seed = 3
    config.step_count_limit = 4000
    config.spent_limit_seconds = 30
    calculator = es.DecomposedCalculator(picks)
    es.Solver(config).solve(schedule, calculator)
    reported = schedule.score
    python = score(es.constraint_costs(schedule, picks), picks)
    fresh = es.DecomposedCalculator(picks).score(schedule)
    assert python.hard == reported.hard == fresh.hard
    assert python.soft == pytest.approx(reported.soft) == pytest.approx(fresh.soft)


# ---------------------------------------------------------------------------
# clip=False with an offset stays off the hard score
# ---------------------------------------------------------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


def _knobs(pick):
    return {knob: getattr(pick, knob) for knob in ("weight", "level", "exponent", "offset", "clip")}


@pytest.mark.parametrize("offset", [0.5, -0.5])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: EmployeeUnavailabilityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: RequiredSkillCost(level=Level.HARD, offset=offset, clip=False),
        lambda offset: UndesiredDayCost(level="hard", offset=offset, clip=False),  # soft moved to hard
        lambda offset: MaxConsecutiveDaysCost(value=3, offset=offset, clip=False, worker_indices=(1,)),  # scoped
    ],
    ids=["unavailability-default-hard", "skill-level-hard", "undesired-moved-to-hard", "scoped-default-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward the employees without unavailable minutes, so their
    slack would cancel another hard violation and an infeasible roster could score hard 0;
    a negative one would charge every employee of a roster without any. The engine
    (DecomposedCalculator(list), add(), constraint_costs) and the score() mirror refuse it
    with the same message, also as a repeat next to the defaults."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    repeat = [*get_constraints(), spec]
    assert _refusal(lambda: es.DecomposedCalculator([spec])) == message
    assert _refusal(lambda: es.DecomposedCalculator(repeat)) == message
    assert _refusal(lambda: es.DecomposedCalculator().add(spec.name, **_knobs(spec), **spec.params())) == message
    assert _refusal(lambda: es.constraint_costs(reference_schedule(), [spec])) == message
    # The mirror reads the entry of an allowed pick with the same parameters.
    allowed = type(spec)(**spec.params())
    assert _refusal(lambda: score(es.constraint_costs(reference_schedule(), [allowed]), [spec])) == message


@pytest.mark.parametrize("offset", [0.5, -0.5])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """3 x (480 - offset) + 3 x 3 x (0 - offset) on soft, every employee charged."""
    pick = EmployeeUnavailabilityCost(level="soft", weight=3, offset=offset, clip=False)
    expected = (0, -3 * ((480 - offset) + 3 * (0 - offset)))
    raw = es.constraint_costs(reference_schedule(), [pick])
    entry = raw["employee_unavailability_cost"]
    assert (entry["level"], entry["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert score(es.constraint_costs(reference_schedule()), [pick]) == expected
    assert _engine([pick]) == expected
    by_name = es.DecomposedCalculator()
    by_name.add(pick.name, **_knobs(pick))
    result = by_name.score(reference_schedule())
    assert (result.hard, result.soft) == expected
    # Next to the hard default, the soft literal copy is fine as well.
    both = [EmployeeUnavailabilityCost(), pick]
    assert _engine(both) == (-480, expected[1])


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (EmployeeUnavailabilityCost(offset=0.5), (-480, 0.0)),  # clip=True on hard: a threshold, 479.5 -> 480
        (EmployeeUnavailabilityCost(offset=-0.5), (-481, 0.0)),  # adds 0.5 to the non-zero employee only
        (EmployeeUnavailabilityCost(clip=False), (-480, 0.0)),  # clip=False with offset 0: the plain shaping
        (MinRestCost(offset=0.0, clip=False), (0, 0.0)),
        (MinRestCost(offset=-5), (0, 0.0)),  # a threshold keeps a 0 at 0
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(es.constraint_costs(reference_schedule(), [pick]), [pick]) == expected
    assert _engine([pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(
        name="employee_unavailability_cost", weight=3, level="soft", exponent=1.0, offset=0.5, clip=False
    )
    assert _engine([moved]) == (0, -3 * (479.5 - 1.5))
    kept = SimpleNamespace(
        name="employee_unavailability_cost", weight=3, level=None, exponent=1.0, offset=0.5, clip=False
    )
    message = HARD_REFUSAL.format(q="'employee_unavailability_cost'")
    assert _refusal(lambda: es.DecomposedCalculator([kept])) == message
    assert _refusal(lambda: score(es.constraint_costs(reference_schedule()), [kept])) == message
