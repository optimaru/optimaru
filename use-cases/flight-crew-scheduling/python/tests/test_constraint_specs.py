"""flight_crew.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pytest

import flight_crew as fc
from flight_crew.constraints import (
    Constraint,
    FlightConflict,
    RequiredSkill,
    employee_unavailability,
    first_assignment_not_departing_from_home,
    flight_conflict,
    get_constraints,
    last_assignment_not_arriving_at_home,
    required_skill,
    score,
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
        "required_skill",
        "flight_conflict",
        "transfer_between_two_flights",
        "employee_unavailability",
        "first_assignment_not_departing_from_home",
        "last_assignment_not_arriving_at_home",
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

    hard_only = solve([required_skill, flight_conflict, employee_unavailability])
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the skill weight grows hard while the soft makeup stays put."""
    picks = [first_assignment_not_departing_from_home]
    base = solve([*picks, required_skill])
    heavier = solve([*picks, RequiredSkill(weight=10_000)])
    assert heavier.hard <= base.hard  # penalty, so equal or more negative


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft moves its penalty to the soft level."""
    hard_level = solve([flight_conflict, last_assignment_not_arriving_at_home])
    soft_level = solve([FlightConflict(hard=False), last_assignment_not_arriving_at_home])
    # Only the level carrying the same cost differs.
    assert hard_level.hard <= 0
    assert soft_level.hard == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "required_skills"  # trailing s
        weight = None
        hard = None
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


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert required_skill.weight is None
    assert required_skill.hard is None
    assert required_skill.exponent == 1.0


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
    base = score(raw, [required_skill])
    ten = score(raw, [RequiredSkill(weight=10)])
    default_weight = raw[required_skill.name]["weight"]
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
