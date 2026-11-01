"""bin_packing.constraints — an API for picking the cost function declaratively."""

import pickle
import random
import re
from types import SimpleNamespace

import pytest

import bin_packing as bp
from bin_packing.constraints import (
    BinCapacityCost,
    ColorConflictCost,
    Constraint,
    ItemConflictCost,
    UsedBinsCost,
    bin_capacity_cost,
    color_conflict_cost,
    get_constraints,
    item_conflict_cost,
    score,
    used_bins_cost,
)

NAMES = ["bin_capacity_cost", "color_conflict_cost", "item_conflict_cost", "used_bins_cost"]


def build_problem() -> bp.BinPackingProblem:
    """Two reds and a conflict pair that would rather share a bin: the constraints on decide."""
    return bp.BinPackingProblem(
        10,
        [
            bp.Item("A", 5, color="red"),
            bp.Item("B", 5, color="red"),
            bp.Item("C", 4),
            bp.Item("D", 3),
            bp.Item("E", 3),
        ],
        conflicts=[("C", "D")],
    )


def _config(seconds: float = 0.3) -> bp.SolverConfig:
    config = bp.SolverConfig()
    config.spent_limit_seconds = seconds
    config.random_seed = 1
    return config


def solve(constraints=None) -> bp.Score:
    problem = build_problem()
    bp.solve(problem, config=_config(), constraints=constraints)
    return problem.score


def _solved(constraints=None) -> bp.BinPackingProblem:
    problem = build_problem()
    bp.solve(problem, config=_config(), constraints=constraints)
    return problem


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    assert bp.list_constraints() == NAMES
    for spec in get_constraints():
        assert spec.name in bp.list_constraints()


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == bp.list_constraints()
    assert get_constraints() == [bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost]


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain == explicit
    assert plain == bp.Score(0, -3.0)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    hard_only = solve([bin_capacity_cost, color_conflict_cost, item_conflict_cost])
    assert hard_only == bp.Score(0, 0.0)


def test_reweighting_one_constraint_scales_only_that_term():
    base = solve()
    heavier = solve([bin_capacity_cost, color_conflict_cost, item_conflict_cost, UsedBinsCost(weight=10)])
    assert heavier.hard == base.hard
    assert heavier.soft == 10 * base.soft


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft empties the hard level."""
    problem = build_problem()
    problem.set_assignment([0, 0, 0, 0, 0])  # 20 into a capacity-10 bin, two reds, C-D together
    assert bp.calculate_score(problem).hard == -(10 + 1 + 1)
    demoted = [BinCapacityCost(level="soft"), ColorConflictCost(level="soft"), ItemConflictCost(level="soft"), used_bins_cost]
    assert bp.calculate_score(problem, demoted) == bp.Score(0, -(10 + 1 + 1 + 1))
    assert bp.calculate_score(problem, [UsedBinsCost(level="hard")]) == bp.Score(-1, 0.0)


def test_calculate_score_with_constraints_stores_the_score():
    problem = build_problem()
    problem.set_assignment([0, 1, 2, 3, 4])
    assert bp.calculate_score(problem, [UsedBinsCost(weight=2)]) == bp.Score(0, -10.0)
    assert problem.score == bp.Score(0, -10.0)
    assert bp.calculate_score(problem) == bp.Score(0, -5.0)


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""
    typo = SimpleNamespace(name="used_bin_cost", weight=None, level=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve([typo])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        bp.solve(build_problem(), config=_config(0.1), constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in (bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost):
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in get_constraints()] == NAMES


def test_every_default_instance_is_exported():
    import bin_packing.constraints as module

    for name in NAMES:
        assert name in module.__all__
        assert getattr(module, name).name == name


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = bp.constraint_info()
    assert [row.name for row in rows] == bp.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = bp.constraint_info()[0]
    assert isinstance(row, bp.ConstraintInfo)
    assert row._asdict() == {
        "name": "bin_capacity_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='bin_capacity_cost'")
    assert pickle.loads(pickle.dumps(bp.constraint_info())) == bp.constraint_info()
    assert bp.ConstraintInfo.__module__ == bp._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = bp.constraint_costs(build_problem())
    assert list(raw) == NAMES
    for row in bp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] is (row.name == "used_bins_cost")
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert "medium_fold" not in entry  # no medium level in this domain


def test_constraint_costs_shapes():
    """Three per-bin constraints (one entry per bin 0..n-1) and the global bin count."""
    problem = build_problem()
    problem.set_assignment([0, 0, 1, 1, 4])
    raw = bp.constraint_costs(problem)
    for name in NAMES[:3]:
        assert len(raw[name]["costs"]) == problem.item_count()
    assert raw["used_bins_cost"]["costs"] == [3]
    assert raw["color_conflict_cost"]["costs"] == [1, 0, 0, 0, 0]
    assert raw["item_conflict_cost"]["costs"] == [0, 1, 0, 0, 0]
    assert raw["bin_capacity_cost"]["costs"] == [0, 0, 0, 0, 0]


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights (1 each)."""
    explicit = [
        BinCapacityCost(weight=1),
        ColorConflictCost(weight=1),
        ItemConflictCost(weight=1),
        UsedBinsCost(weight=1),
    ]
    problem = _solved()
    raw = bp.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (problem.score.hard, problem.score.soft)


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
        UsedBinsCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="used_bins_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="used_bins_cost", weight=None, level=None, exponent=0.0)
    for pick, message in ((negative, "weight must be finite and >= 0"), (flat, "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            bp.solve(build_problem(), config=_config(0.1), constraints=[pick])
        with pytest.raises(ValueError, match=message):
            bp.calculate_score(build_problem(), [pick])


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
    pick = SimpleNamespace(name="used_bins_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'used_bins_cost': {re.escape(message)}"):
        bp.solve(build_problem(), config=_config(0.1), constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        UsedBinsCost(**knobs)


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in ("bin_capacity", "color_compatibility", "item_conflicts", "minimize_bins"):
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            solve([pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(bp.constraint_costs(build_problem()), [pick])
    import bin_packing.constraints as module

    for old in ("BinCapacity", "ColorCompatibility", "ItemConflicts", "MinimizeBins", "minimize_bins"):
        assert not hasattr(module, old)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def test_score_equals_the_engine_for_the_default_assembly():
    problem = _solved()
    engine = bp.calculate_score(problem)
    python = score(bp.constraint_costs(problem))
    assert python == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [bin_capacity_cost],
        [bin_capacity_cost, color_conflict_cost, item_conflict_cost, UsedBinsCost(weight=60)],
        [BinCapacityCost(level="soft"), color_conflict_cost, item_conflict_cost, used_bins_cost],
        [BinCapacityCost(exponent=2.0), ColorConflictCost(exponent=3.0), item_conflict_cost, used_bins_cost],
        [BinCapacityCost(weight=0.5), UsedBinsCost(level="hard", weight=0.5, exponent=2.0)],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score."""
    problem = _solved(specs)
    python = score(bp.constraint_costs(problem), specs)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)


def test_python_score_equals_cpp_calculate_score_for_random_specs():
    """For random assignments and random specs (exponents included): Python == C++."""
    rng = random.Random(3)
    classes = (BinCapacityCost, ColorConflictCost, ItemConflictCost, UsedBinsCost)
    for _ in range(60):
        problem = build_problem()
        problem.set_assignment([rng.randrange(problem.item_count()) for _ in range(problem.item_count())])
        picked = rng.sample(classes, rng.randrange(1, len(classes) + 1))
        specs = [
            cls(
                weight=rng.choice([None, 0, 0.5, 1, 3]),
                level=rng.choice([None, "hard", "soft"]),
                exponent=rng.choice([1.0, 0.5, 2.0, 3.0]),
            )
            for cls in picked
        ]
        engine = bp.calculate_score(problem, specs)
        python = score(bp.constraint_costs(problem), specs)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)


def test_exponent_shapes_per_bin_and_the_bin_count():
    """The 0.2.0 unit granularity: per bin for colours/conflicts, on the count for used bins."""
    problem = bp.BinPackingProblem(
        100,
        [bp.Item(f"R{k}", 1, color="red") for k in range(3)] + [bp.Item(f"B{k}", 1, color="blue") for k in range(2)],
        conflicts=[("R0", "R1"), ("R1", "R2")],
    )
    problem.set_assignment([0, 0, 0, 0, 0])
    raw = bp.constraint_costs(problem)
    assert score(raw, [ColorConflictCost(exponent=2.0)]).hard == -9  # 0.1.x: 2**2 + 1**2 = 5
    assert score(raw, [ItemConflictCost(exponent=2.0)]).hard == -4  # 0.1.x: 1 + 1 = 2
    problem.set_assignment([0, 1, 2, 3, 4])
    assert bp.calculate_score(problem, [UsedBinsCost(exponent=2.0)]) == bp.Score(0, -25.0)


def test_score_is_additive_over_the_picked_constraints():
    problem = _solved()
    raw = bp.constraint_costs(problem)
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_rejects_an_unknown_constraint_name():
    raw = bp.constraint_costs(_solved())

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


def test_naming_a_constraint_twice_is_rejected():
    with pytest.raises(ValueError, match="picked more than once"):
        bp.solve(build_problem(), config=_config(0.1), constraints=[used_bins_cost, UsedBinsCost(weight=10)])
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([used_bins_cost, used_bins_cost])
