"""project_job.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pytest

import project_job as pjs
from project_job.constraints import (
    Constraint,
    TotalMakespan,
    get_constraints,
    renewable_resource_capacity,
    score,
    total_makespan,
    total_project_delay,
)
from project_job.demo import generate_demo_data


def build_schedule():
    return generate_demo_data().to_core()


def solve(constraints=None):
    schedule = build_schedule()
    pjs.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = pjs.list_constraints()
    assert available == [
        "non_renewable_resource_capacity",
        "renewable_resource_capacity",
        "total_project_delay",
        "total_makespan",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == pjs.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the medium term disappears entirely."""
    baseline = solve([renewable_resource_capacity, total_project_delay, total_makespan])
    assert baseline.soft < 0

    without_delay = solve([renewable_resource_capacity, total_makespan])
    assert without_delay.medium == 0
    assert without_delay.soft < 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the makespan weight grows soft only; hard stays put."""
    base = solve([renewable_resource_capacity, total_makespan])
    heavier = solve([renewable_resource_capacity, TotalMakespan(weight=10)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    The makespan is always positive, so raising the level always lands on hard."""
    soft_level = solve([total_makespan])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([TotalMakespan(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "total_makespans"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        pjs.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert total_makespan.weight is None
    assert total_makespan.hard is None
    assert total_makespan.exponent == 1.0


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    pjs.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return pjs.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == pjs.calculate_score(solution).hard


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
    base = score(raw, [total_makespan])
    ten = score(raw, [TotalMakespan(weight=10)])
    default_weight = raw[total_makespan.name]["weight"]
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
