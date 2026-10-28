"""order_picking.constraints — an API for picking the cost function declaratively.

"Which constraints are on" is the only path covered here. Per-constraint values
are covered by `test_order_picking.py` and the C++ tests.
"""

import pytest

import order_picking as op
from order_picking.constraints import (
    Constraint,
    MinimizeOrderSplitByTrolley,
    get_constraints,
    minimize_distance_from_last_pick_task_to_path_origin,
    minimize_distance_from_previous_pick_task,
    minimize_order_split_by_trolley,
    required_number_of_buckets,
    score,
)


def build_solution():
    return op.build_demo_data()


def solve(constraints=None):
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.5, random_seed=1, constraints=constraints)
    return solution.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = op.list_constraints()
    assert available == [
        "required_number_of_buckets",
        "minimize_distance_from_previous_pick_task",
        "minimize_distance_from_last_pick_task_to_path_origin",
        "minimize_order_split_by_trolley",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == op.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring.

    Whether both paths reach the same **solution** cannot be checked: the
    order_consolidate_moves operator does not run on the plugin path (it reads
    the monolithic calculator's live volume table — cpp/include/op/solver.hpp)."""
    solution = build_solution()
    op.solve(
        solution,
        time_limit_seconds=0.5,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = op.calculate_score(solution)
    assert solution.score.hard == recomputed.hard
    assert solution.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the split term disappears entirely."""
    baseline = solve()
    assert baseline.soft < 0

    distance_only = solve(
        [
            required_number_of_buckets,
            minimize_distance_from_previous_pick_task,
            minimize_distance_from_last_pick_task_to_path_origin,
        ]
    )
    # The split penalty (weight 1000) is gone, so soft is much less negative.
    assert distance_only.soft > baseline.soft


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the split weight grows soft only; hard stays put."""
    picks = [required_number_of_buckets]
    base = solve([*picks, minimize_order_split_by_trolley])
    heavier = solve([*picks, MinimizeOrderSplitByTrolley(weight=100_000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Every pick has to ride somewhere, so (order, trolley) pairs can never be 0 —
    raising the level always lands on hard."""
    soft_level = solve([required_number_of_buckets, minimize_order_split_by_trolley])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([required_number_of_buckets, MinimizeOrderSplitByTrolley(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "minimize_order_split_by_trolleys"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        op.solve(build_solution(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert minimize_order_split_by_trolley.weight is None
    assert minimize_order_split_by_trolley.hard is None
    assert minimize_order_split_by_trolley.exponent == 1.0


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.4, random_seed=1)
    return op.constraint_costs(solution), solution


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = op.calculate_score(solution)
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
    base = score(raw, [minimize_order_split_by_trolley])
    ten = score(raw, [MinimizeOrderSplitByTrolley(weight=10)])
    default_weight = raw[minimize_order_split_by_trolley.name]["weight"]
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
