"""es_scheduling.constraints.get_constraints — the default cost function in one call.

The spec classes and the `DecomposedCalculator` assembly are unchanged. What is
checked here is that `get_constraints()`, the entry point every other use case in
the repository shares, gives back exactly the eight default constraints.
"""

import pytest
from _reference import REFERENCE_SCORE, reference_schedule

import es_scheduling as es
from es_scheduling import models
from es_scheduling.constraints import (
    RequiredSkill,
    UndesiredDay,
    get_constraints,
)


def test_get_constraints_defaults_to_the_eight_default_specs():
    """The eight that `es::defaultConstraints()` stacks, in that order."""
    assert [spec.name for spec in get_constraints()] == [
        "required_skill",
        "unavailable_employee",
        "overlapping_shift",
        "one_shift_per_day",
        "min_rest",
        "undesired_day",
        "desired_day",
        "balance_assignments",
    ]


def test_list_constraints_gives_the_available_names():
    assert es.list_constraints() == list(es.CONSTRAINT_NAMES)


def test_default_names_are_a_subset_of_the_available_names():
    """The default set is a subset of the selectable names (the rest are opt-in)."""
    available = set(es.CONSTRAINT_NAMES)
    assert {spec.name for spec in get_constraints()} <= available
    assert len(available) > len(get_constraints())


def test_get_constraints_reproduces_the_builtin_score():
    """A calculator assembled from the default list must match the built-in score."""
    schedule = reference_schedule()
    calculator = es.DecomposedCalculator(get_constraints())
    assert str(calculator.score(schedule)) == REFERENCE_SCORE


def test_passing_a_list_uses_it_as_given():
    """The given list is used as is: only what is in it gets scored."""
    picked = get_constraints([RequiredSkill(), UndesiredDay(2.0)])
    assert [spec.name for spec in picked] == ["required_skill", "undesired_day"]

    schedule = reference_schedule()
    partial = es.DecomposedCalculator(picked).score(schedule)
    full = es.DecomposedCalculator(get_constraints()).score(schedule)
    assert str(partial) != str(full)


def test_empty_list_is_rejected_not_treated_as_the_defaults():
    """An empty list is NOT the defaults: a filter that happens to come back empty
    must be an error.

    "Picked nothing" must not silently flip to "score everything", and the C++
    assembly path follows the same contract (an empty list is a value_error)."""
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_returned_list_is_a_fresh_copy():
    """A caller editing the returned list must not affect the next call."""
    first = get_constraints()
    first.pop()
    assert len(get_constraints()) == 8


@pytest.mark.parametrize(
    "spec, rule, name",
    [
        (es.MaxConsecutiveDays, models.MaxConsecutiveDaysRule, "max_consecutive_days"),
        (es.MaxConsecutiveOff, models.MaxConsecutiveOffRule, "max_consecutive_off"),
        (es.RestAfterConsecutiveWork, models.RestAfterConsecutiveWorkRule, "rest_after_consecutive_work"),
        (es.MinWeeklyRestDays, models.MinWeeklyRestDaysRule, "min_weekly_rest_days"),
    ],
)
def test_a_default_is_the_same_on_every_path(spec, rule, name):
    """The spec, the rule model and add(name) each hold their own copy of a default."""
    schedule = es.make_random_roster(days=21, shifts_per_day=2, employees=3, seed=5)
    for index, shift in enumerate(schedule.shifts):
        shift.employee_index = index % 2  # two people cover everything, the third rests

    named = es.DecomposedCalculator()
    named.add(name)
    by_name = named.score(schedule)

    assert str(es.DecomposedCalculator([spec()]).score(schedule)) == str(by_name)
    assert str(es.DecomposedCalculator([rule()]).score(schedule)) == str(by_name)


def test_rest_after_location_run_default_is_the_same_on_every_path():
    shifts = [es.Shift(f"Shift {day}", es.minutes(day, 9), es.minutes(day, 17), "Location B") for day in range(6)]
    for shift in shifts:
        shift.employee_index = 0
    shifts[2].location = "Location A"
    schedule = es.EmployeeSchedule([es.Employee("A")], shifts)

    named = es.DecomposedCalculator()
    named.add("rest_after_location_run", location="Location B")
    by_name = named.score(schedule)

    assert by_name.hard == -1  # the day after the first stint is worked
    rule = models.RestAfterLocationRunRule(location="Location B")
    assert str(es.DecomposedCalculator([rule]).score(schedule)) == str(by_name)


def test_run_repair_default_is_the_same_on_both_sides():
    assert es.SolverConfig().employee_run_repair_max_days == models.SolverSettings().employee_run_repair_max_days


def test_specs_carry_the_documented_defaults():
    """Spec defaults are weight 1.0, hard None, exponent 1.0."""
    spec = get_constraints()[0]
    assert spec.weight == pytest.approx(1.0)
    assert spec.hard is None
    assert spec.exponent == pytest.approx(1.0)
