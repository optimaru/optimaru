"""es_scheduling.constraints — one spec per constraint, the C++ catalog and the knobs.

The specs carry the three knobs (weight / level / exponent) and the domain
parameters; the defaults (level, weight, reward) live in the C++ components
and are reported by `constraint_info()`. `get_constraints()` is the entry point
every use case shares; the rule factory (`DecomposedCalculator`,
`constraint_costs`) may hold one constraint several times.
"""

import pickle
import re
from types import SimpleNamespace

import pytest
from _reference import REFERENCE_SCORE, reference_schedule

import es_scheduling as es
from es_scheduling import constraints as specs
from es_scheduling import models
from es_scheduling.constraints import (
    SPEC_CLASSES,
    DayPatternCost,
    DesiredDayCost,
    DesiredShiftCost,
    EmployeeUnavailabilityCost,
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
#: The pre-0.2.0 names, all refused now.
OLD_NAMES = [
    "required_skill",
    "unavailable_employee",
    "overlapping_shift",
    "one_shift_per_day",
    "min_rest",
    "undesired_day",
    "desired_day",
    "desired_shift",
    "balance_assignments",
    "max_consecutive_days",
    "min_consecutive_days",
    "max_consecutive_off",
    "rest_after_consecutive_work",
    "max_weekly_minutes",
    "min_weekly_minutes",
    "min_weekly_rest_days",
    "unassigned_shift",
    "balance_min_max_gap",
    "balance_weekly_minutes_gap",
    "shift_coverage",
    "location_run",
    "rest_after_location_run",
    "location_run_gap",
    "worker_location_count",
    "day_pattern",
    "target_days_off",
    "worker_day_count_in_range",
    "balance_location",
    "worker_pair_same_shift",
]


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


def test_a_rule_may_repeat_with_other_parameters_but_not_with_the_same():
    picked = get_constraints([DayPatternCost(pattern="WORK,WORK"), DayPatternCost(pattern="OFF,OFF")])
    assert len(picked) == 2
    with pytest.raises(ValueError, match="same parameters"):
        get_constraints([DayPatternCost(pattern="WORK,WORK"), DayPatternCost(pattern="WORK,WORK")])
    scoped = get_constraints([MaxConsecutiveDaysCost(), MaxConsecutiveDaysCost(worker_indices=(0,))])
    assert len(scoped) == 2


# ---------------------------------------------------------------------------
# The catalog: constraint_info() == the C++ components' constants
# ---------------------------------------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    rows = es.constraint_info()
    # The defaults first; then the rule-only units, then the rule-only globals.
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
    """worker_location_count_cost and worker_day_count_in_range_cost are SOFT by
    default (the C++ truth; older documentation said hard)."""
    rows = {row.name: row for row in es.constraint_info()}
    assert rows["worker_location_count_cost"].default_level == "soft"
    assert rows["worker_day_count_in_range_cost"].default_level == "soft"


def test_constraint_info_rows_are_plain_values():
    row = es.constraint_info()[0]
    assert isinstance(row, es.ConstraintInfo)
    assert row._asdict() == {
        "name": "required_skill_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='required_skill_cost'")
    assert pickle.loads(pickle.dumps(es.constraint_info())) == es.constraint_info()
    assert es.ConstraintInfo.__module__ == es._core.__name__


def test_the_factory_specs_rules_and_catalog_name_the_same_constraints():
    assert set(es.CONSTRAINT_NAMES) == {row.name for row in es.constraint_info()}
    assert list(models.RULE_MODELS) == list(es.CONSTRAINT_NAMES)
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
        assert entry["hard"] is (row.default_level == "hard")
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
    """The spec, the JSON rule model and add(name) build the same component."""
    schedule = es.make_random_roster(days=21, shifts_per_day=2, employees=3, seed=5)
    for index, shift in enumerate(schedule.shifts):
        shift.employee_index = index % 2  # two people cover everything, the third rests
    named = es.DecomposedCalculator()
    named.add(spec.name)
    by_name = named.score(schedule)

    assert str(es.DecomposedCalculator([spec()]).score(schedule)) == str(by_name)
    assert str(es.DecomposedCalculator([models.rule_model(spec)()]).score(schedule)) == str(by_name)


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
    rule = models.rule_model(RestAfterLocationRunCost)(location="Location B")
    assert str(es.DecomposedCalculator([rule]).score(schedule)) == str(by_name)
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
    spec = get_constraints()[0]
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
    [({"weight": -2.0}, "weight must be finite and >= 0"), ({"exponent": 0.0}, "exponent must be finite and > 0")],
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


@pytest.mark.parametrize("old", OLD_NAMES)
def test_the_old_names_are_unknown(old):
    with pytest.raises(ValueError, match=f"unknown constraint '{old}'; available: required_skill_cost, "):
        es.DecomposedCalculator().add(old)


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
