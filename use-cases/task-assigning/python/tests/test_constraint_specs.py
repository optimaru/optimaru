"""task_assigning.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pytest

import task_assigning as ta
from task_assigning.constraints import (
    Constraint,
    CriticalPriorityTaskEndTime,
    MinimizeMakespan,
    critical_priority_task_end_time,
    get_constraints,
    minimize_makespan,
    minimize_unassigned_tasks,
    no_missing_skills,
    score,
)
from task_assigning.demo import generate_demo_data


def build_solution():
    return generate_demo_data().to_core()


def solve(constraints=None):
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return solution.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, the global constraint (minimize_unassigned_tasks) after.
    available = ta.list_constraints()
    assert available == [
        "no_missing_skills",
        "minimize_makespan",
        "critical_priority_task_end_time",
        "major_priority_task_end_time",
        "minor_priority_task_end_time",
        "minimize_unassigned_tasks",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ta.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    solution = build_solution()
    ta.solve(
        solution,
        time_limit_seconds=0.6,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = ta.calculate_score(solution)
    assert solution.score.hard == recomputed.hard
    assert solution.score.soft0 == recomputed.soft0
    assert solution.score.soft1 == pytest.approx(recomputed.soft1)
    assert solution.score.soft2 == pytest.approx(recomputed.soft2)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — that level is emptied entirely.

    Four Bendable levels: hard = missing skills, soft0 = unassigned,
    soft1 = makespan, soft2 = priority end times."""
    baseline = solve()
    assert baseline.soft1 < 0 and baseline.soft2 < 0

    hard_only = solve([no_missing_skills, minimize_unassigned_tasks])
    assert hard_only.soft1 == 0
    assert hard_only.soft2 == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the makespan weight grows soft only; hard stays put."""
    picks = [no_missing_skills, minimize_unassigned_tasks]
    base = solve([*picks, minimize_makespan])
    heavier = solve([*picks, MinimizeMakespan(weight=10**9)])
    assert heavier.hard == base.hard
    assert heavier.soft1 < base.soft1  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard side.

    As long as every task is assigned, priority end times can never be 0. Raising the
    level puts that cost into the engine's folded hard level, and reading it back it sits
    below kMissingSkillUnit (10^9), so it shows up in the soft0 slot — the penalty moves
    from soft2 to soft0 wholesale."""
    picks = [minimize_unassigned_tasks]

    soft_level = solve([*picks, critical_priority_task_end_time])
    assert soft_level.soft2 < 0
    assert soft_level.soft0 == 0

    hard_level = solve([*picks, CriticalPriorityTaskEndTime(hard=True)])
    assert hard_level.soft2 == 0
    assert hard_level.soft0 < 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "no_missing_skill"  # missing trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ta.solve(build_solution(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert minimize_makespan.weight is None
    assert minimize_makespan.hard is None
    assert minimize_makespan.exponent == 1.0


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.4, random_seed=1)
    return ta.constraint_costs(solution), solution


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == ta.calculate_score(solution).hard


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
    base = score(raw, [minimize_makespan])
    ten = score(raw, [MinimizeMakespan(weight=10)])
    default_weight = raw[minimize_makespan.name]["weight"]
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
