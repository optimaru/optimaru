"""order_picking.constraints — an API for picking the cost function declaratively.

"Which constraints are on" and the knobs are covered here. Per-constraint values
are covered by `test_order_picking.py` and the C++ tests.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import order_picking as op
from order_picking.constraints import (
    Constraint,
    DistanceFromLastPickTaskToPathOriginCost,
    DistanceFromPreviousPickTaskCost,
    OrderSplitByTrolleyCost,
    TrolleyCapacityCost,
    distance_from_last_pick_task_to_path_origin_cost,
    distance_from_previous_pick_task_cost,
    get_constraints,
    order_split_by_trolley_cost,
    score,
    trolley_capacity_cost,
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
        "trolley_capacity_cost",
        "distance_from_previous_pick_task_cost",
        "distance_from_last_pick_task_to_path_origin_cost",
        "order_split_by_trolley_cost",
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
            trolley_capacity_cost,
            distance_from_previous_pick_task_cost,
            distance_from_last_pick_task_to_path_origin_cost,
        ]
    )
    # The split penalty (weight 1000) is gone, so soft is much less negative.
    assert distance_only.soft > baseline.soft


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the split weight grows soft only; hard stays put."""
    picks = [trolley_capacity_cost]
    base = solve([*picks, order_split_by_trolley_cost])
    heavier = solve([*picks, OrderSplitByTrolleyCost(weight=100_000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Every pick has to ride somewhere, so (order, trolley) pairs can never be 0 —
    raising the level always lands on hard."""
    soft_level = solve([trolley_capacity_cost, order_split_by_trolley_cost])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([trolley_capacity_cost, OrderSplitByTrolleyCost(level="hard")])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "order_split_by_trolley_costs"  # trailing s
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        op.solve(build_solution(), time_limit_seconds=0.2, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_is_rejected():
    """Passing the same constraint twice must not let the later one silently win."""
    twice = [order_split_by_trolley_cost, OrderSplitByTrolleyCost(weight=10)]
    with pytest.raises(ValueError, match="picked more than once"):
        op.solve(build_solution(), time_limit_seconds=0.2, constraints=twice)
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints(twice)


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert get_constraints() == [
        trolley_capacity_cost,
        distance_from_previous_pick_task_cost,
        distance_from_last_pick_task_to_path_origin_cost,
        order_split_by_trolley_cost,
    ]


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = op.constraint_info()
    assert [row.name for row in rows] == op.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 1000.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = op.constraint_info()[0]
    assert isinstance(row, op.ConstraintInfo)
    assert row._asdict() == {
        "name": "trolley_capacity_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='trolley_capacity_cost'")
    assert pickle.loads(pickle.dumps(op.constraint_info())) == op.constraint_info()
    assert op.ConstraintInfo.__module__ == op._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = op.constraint_costs(build_solution())
    for row in op.constraint_info():
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
    """weight=None scores exactly like the explicit default weights 1 / 1 / 1 / 1000."""
    explicit = [
        TrolleyCapacityCost(weight=1),
        DistanceFromPreviousPickTaskCost(weight=1),
        DistanceFromLastPickTaskToPathOriginCost(weight=1),
        OrderSplitByTrolleyCost(weight=1000),
    ]
    raw, solution = _raw()
    engine = op.calculate_score(solution)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (engine.hard, engine.soft)


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
        OrderSplitByTrolleyCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "order_split_by_trolley_cost"
        weight = -2.0
        level = None
        exponent = 1.0

    class Flat:
        name = "order_split_by_trolley_cost"
        weight = None
        level = None
        exponent = 0.0

    for pick, message in ((Negative(), "weight must be finite and >= 0"), (Flat(), "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            op.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="order_split_by_trolley_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'order_split_by_trolley_cost': {re.escape(message)}"):
        op.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        OrderSplitByTrolleyCost(**knobs)


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in (
        "required_number_of_buckets",
        "minimize_distance_from_previous_pick_task",
        "minimize_distance_from_last_pick_task_to_path_origin",
        "minimize_order_split_by_trolley",
    ):

        class Old:
            name = old
            weight = None
            level = None
            exponent = 1.0

        with pytest.raises(ValueError, match="unknown constraint"):
            op.solve(build_solution(), time_limit_seconds=0.1, constraints=[Old()])
    for gone in ("ConstraintTotals", "calculate_totals"):
        assert not hasattr(op, gone)
        assert not hasattr(op._core, gone)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw(constraints=None):
    """Raw per-constraint costs of one solved solution."""
    solution = build_solution()
    op.solve(solution, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
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


@pytest.mark.parametrize(
    "specs",
    [
        [TrolleyCapacityCost(level="soft"), *get_constraints()[1:]],
        [*get_constraints()[:3], OrderSplitByTrolleyCost(weight=10, exponent=2.0)],
        [TrolleyCapacityCost(exponent=2.0), DistanceFromPreviousPickTaskCost(level="hard", weight=0.5)],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per trolley, so reproducing it needs the per-unit raw costs."""
    raw, solution = _raw(specs)
    python = score(raw, specs)
    assert python.hard == solution.score.hard
    assert python.soft == pytest.approx(solution.score.soft)


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
    base = score(raw, [order_split_by_trolley_cost])
    ten = score(raw, [OrderSplitByTrolleyCost(weight=10)])
    default_weight = raw[order_split_by_trolley_cost.name]["weight"]
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
