"""tournament_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pytest

import tournament_scheduling as ts
from tournament_scheduling.constraints import (
    Constraint,
    EvenlyConfrontationCount,
    evenly_confrontation_count,
    fair_assignment_count_per_team,
    get_constraints,
    one_assignment_per_date_per_team,
    score,
)
from tournament_scheduling.models import demo_data


def build_schedule():
    return demo_data().to_core()


def solve(constraints=None):
    schedule = build_schedule()
    ts.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints (the two fairness terms) after.
    available = ts.list_constraints()
    assert available == [
        "one_assignment_per_date_per_team",
        "unavailability_penalty",
        "fair_assignment_count_per_team",
        "evenly_confrontation_count",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ts.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring.

    Whether both paths reach the same **solution** cannot be checked: the teamCountMoves
    guided operator does not run on ts's plugin path (it is tied to the monolithic
    calculator's live aggregates — cpp/include/ts/solver.hpp).
    What can be checked is that both calculators agree on the same solution."""
    schedule = build_schedule()
    ts.solve(
        schedule,
        time_limit_seconds=0.6,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = ts.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the fairness terms disappear entirely."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve([one_assignment_per_date_per_team])
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the head-to-head balance weight grows soft only; hard stays put."""
    picks = [one_assignment_per_date_per_team]
    base = solve([*picks, evenly_confrontation_count])
    heavier = solve([*picks, EvenlyConfrontationCount(weight=50)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Head-to-head unfairness can never be 0 on this instance, so raising the level always
    lands on hard."""
    soft_level = solve([one_assignment_per_date_per_team, evenly_confrontation_count])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([one_assignment_per_date_per_team, EvenlyConfrontationCount(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "unavailability_penalty_constraint"  # the C++ struct name (not the component name)
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ts.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert fair_assignment_count_per_team.weight is None
    assert fair_assignment_count_per_team.hard is None
    assert fair_assignment_count_per_team.exponent == 1.0


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    ts.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return ts.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = ts.calculate_score(solution)
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
    base = score(raw, [evenly_confrontation_count])
    ten = score(raw, [EvenlyConfrontationCount(weight=10)])
    default_weight = raw[evenly_confrontation_count.name]["weight"]
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
