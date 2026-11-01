"""max_cut.constraints — an API for picking the cost function declaratively."""

import json
import pickle
import re
from pathlib import Path
from types import SimpleNamespace

import pytest

import max_cut
from max_cut.constraints import (
    Constraint,
    CutWeightCost,
    DifferentPartitionPairsCost,
    FixedPartitionCost,
    PartitionImbalanceCost,
    PartitionSizeBoundsCost,
    SamePartitionPairsCost,
    cut_weight_cost,
    different_partition_pairs_cost,
    fixed_partition_cost,
    get_constraints,
    partition_imbalance_cost,
    partition_size_bounds_cost,
    same_partition_pairs_cost,
    score,
)
from max_cut.models import EdgeModel, MaxCutProblemModel, VertexPairModel

NAMES = [
    "fixed_partition_cost",
    "same_partition_pairs_cost",
    "different_partition_pairs_cost",
    "partition_size_bounds_cost",
    "partition_imbalance_cost",
    "cut_weight_cost",
]


def fixture_h(partition=(0, 1, 1, 1)) -> MaxCutProblemModel:
    """A-B 2, B-C 3, C-D 4; A fixed to 1; same (A, B); different (B, C); exactly 2 per side.

    With 0,1,1,1 every hard constraint is positive: fixed [1, 0], same [1, 0], different
    [0, 1], size [1, 1]; imbalance 2, cut 2 -> -5hard/2soft. With 1,1,0,0 every hard
    constraint is 0, imbalance 0, cut 3.
    """
    return MaxCutProblemModel(
        vertices=["A", "B", "C", "D"],
        edges=[
            EdgeModel(source="A", target="B", weight=2),
            EdgeModel(source="B", target="C", weight=3),
            EdgeModel(source="C", target="D", weight=4),
        ],
        fixed_partition={"A": 1},
        same_partition_pairs=[VertexPairModel(first="A", second="B")],
        different_partition_pairs=[VertexPairModel(first="B", second="C")],
        min_partition_size=2,
        max_partition_size=2,
        partition=dict(zip("ABCD", partition, strict=True)),
    )


def plain() -> MaxCutProblemModel:
    return MaxCutProblemModel(
        vertices=["A", "B", "C"],
        edges=[EdgeModel(source="A", target="B", weight=2), EdgeModel(source="B", target="C", weight=3)],
    )


def solved(constraints=None, problem=None) -> MaxCutProblemModel:
    problem = problem if problem is not None else fixture_h()
    config = max_cut.SolverConfig()
    config.step_count_limit = 2000
    config.random_seed = 1
    max_cut.solve(problem, config=config, constraints=constraints)
    return problem


def _zero() -> max_cut.SolverConfig:
    config = max_cut.SolverConfig()
    config.spent_limit_seconds = 0
    return config


def engine_score(problem: MaxCutProblemModel, constraints) -> tuple[int, float]:
    """The engine's score of the model's partition as it stands (a warm start, zero budget)."""
    core = problem.to_core()
    core.warm_start = True
    max_cut.Solver(_zero()).solve_with_constraints(core, constraints)
    return core.score.hard, core.score.soft


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    assert max_cut.list_constraints() == NAMES
    for spec in get_constraints():
        assert spec.name in NAMES


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == max_cut.list_constraints()


def test_module_level_instances_keep_every_default():
    """Module-level instances keep the defaults (weight=None, hard=None, exponent=1)."""
    instances = (
        fixed_partition_cost,
        same_partition_pairs_cost,
        different_partition_pairs_cost,
        partition_size_bounds_cost,
        partition_imbalance_cost,
        cut_weight_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
        assert spec.params() == {}
    assert list(instances) == get_constraints()


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = max_cut.constraint_info()
    assert [row.name for row in rows] == max_cut.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [("hard", 1.0, True, False, True)] * 4 + [
        ("soft", 0.0, True, False, False),  # partition_imbalance_cost: opt-in
        ("soft", 1.0, False, True, True),  # cut_weight_cost: a reward
    ]


def test_constraint_info_rows_are_plain_values():
    row = max_cut.constraint_info()[-1]
    assert isinstance(row, max_cut.ConstraintInfo)
    assert row._asdict() == {
        "name": "cut_weight_cost",
        "default_level": "soft",
        "default_weight": 1.0,
        "level_flippable": False,
        "reward": True,
        "default_enabled": True,
    }
    assert repr(max_cut.constraint_info()[0]).startswith("ConstraintInfo(name='fixed_partition_cost'")
    assert pickle.loads(pickle.dumps(max_cut.constraint_info())) == max_cut.constraint_info()
    assert max_cut.ConstraintInfo.__module__ == max_cut._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults; four unit constraints per side, two globals."""
    raw = max_cut.constraint_costs(fixture_h())
    assert list(raw) == NAMES
    for row in max_cut.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (row.level_flippable, row.reward)
        is_global = row.name in ("partition_imbalance_cost", "cut_weight_cost")
        assert entry["global"] is is_global and len(entry["costs"]) == (1 if is_global else 2)
        assert (entry["name"], entry["label"], entry["exponent"]) == (row.name, "", 1.0)
        assert "medium_fold" not in entry  # no medium level in this domain


# --- Raw costs by hand ----------------------------------------------------------


def test_constraint_costs_match_the_hand_calculation():
    raw = max_cut.constraint_costs(fixture_h())
    assert {name: entry["costs"] for name, entry in raw.items()} == {
        "fixed_partition_cost": [1.0, 0.0],
        "same_partition_pairs_cost": [1.0, 0.0],
        "different_partition_pairs_cost": [0.0, 1.0],
        "partition_size_bounds_cost": [1.0, 1.0],
        "partition_imbalance_cost": [2.0],
        "cut_weight_cost": [-2.0],
    }
    assert score(raw) == (-5, 2.0)
    clean = max_cut.constraint_costs(fixture_h((1, 1, 0, 0)))
    assert {name: entry["costs"] for name, entry in clean.items()} == {
        **{name: [0.0, 0.0] for name in NAMES[:4]},
        "partition_imbalance_cost": [0.0],
        "cut_weight_cost": [-3.0],
    }
    assert score(clean) == (0, 3.0)


def test_units_shape_per_side_and_globals_their_total():
    """0.2.0: the exponent shapes each side's cost (0.1.x shaped each vertex / pair term)."""
    problem = MaxCutProblemModel(
        vertices=["A", "B", "C", "D"],
        edges=[EdgeModel(source="A", target="D", weight=3)],
        fixed_partition={"A": 1, "B": 1, "C": 1},
        different_partition_pairs=[VertexPairModel(first="A", second="B")],
        partition={"A": 0, "B": 0, "C": 0, "D": 1},
    )
    raw = max_cut.constraint_costs(problem)
    assert raw["fixed_partition_cost"]["costs"] == [3.0, 0.0]
    assert score(raw, [FixedPartitionCost(exponent=2)]) == (-9, 0.0)  # 3^2 on side 0, not 1 + 1 + 1
    assert score(raw, [PartitionImbalanceCost(weight=1, exponent=2)]) == (0, -4.0)  # |3 - 1|^2
    assert score(raw, [CutWeightCost(exponent=2)]) == (0, 9.0)  # +(3^2), sign-preserving
    assert engine_score(problem, [FixedPartitionCost(exponent=2)]) == (-9, 0.0)
    assert engine_score(problem, [PartitionImbalanceCost(weight=1, exponent=2)]) == (0, -4.0)
    assert engine_score(problem, [CutWeightCost(exponent=2)]) == (0, 9.0)


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights (1, imbalance 0)."""
    explicit = [type(spec)(weight=0 if spec.name == "partition_imbalance_cost" else 1) for spec in get_constraints()]
    problem = solved()
    raw = max_cut.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (problem.score_hard, problem.score_soft)


def test_default_solve_equals_explicit_default_constraints():
    default = solved()
    explicit = solved(get_constraints())
    assert (default.score_hard, default.score_soft) == (explicit.score_hard, explicit.score_soft)


def test_picking_a_subset_drops_the_other_terms():
    problem = fixture_h()
    assert max_cut.calculate_score(problem, [fixed_partition_cost, partition_size_bounds_cost]) == (-3, 0.0)
    assert max_cut.calculate_score(problem, [cut_weight_cost]) == (0, 2.0)


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft moves its cost to the soft level."""
    problem = fixture_h()
    flipped = [
        spec if spec.name != "partition_size_bounds_cost" else PartitionSizeBoundsCost(hard=False)
        for spec in get_constraints()
    ]
    assert max_cut.calculate_score(problem, flipped) == (-3, 0.0)
    assert engine_score(problem, flipped) == (-3, 0.0)
    promoted = [*get_constraints()[:4], PartitionImbalanceCost(weight=1, hard=True), cut_weight_cost]
    assert max_cut.calculate_score(problem, promoted) == (-7, 2.0)
    assert engine_score(problem, promoted) == (-7, 2.0)


def test_reweighting_scales_only_that_term():
    problem = fixture_h()
    heavier = [
        spec if spec.name != "same_partition_pairs_cost" else SamePartitionPairsCost(weight=7)
        for spec in get_constraints()
    ]
    assert max_cut.calculate_score(problem, heavier) == (-11, 2.0)
    balanced = [*get_constraints()[:4], PartitionImbalanceCost(weight=1), cut_weight_cost]
    assert max_cut.calculate_score(problem, balanced) == (-5, 0.0)


def test_the_reward_refuses_hard_true_on_every_path():
    """cut_weight_cost is a reward (raw <= 0): hard=True raises, in Python and through C++."""
    pick = CutWeightCost(hard=True)  # the spec itself is a plain value
    problem = fixture_h()
    with pytest.raises(ValueError, match="is a reward"):
        score(max_cut.constraint_costs(problem), [pick])
    with pytest.raises(ValueError, match="is a reward"):
        max_cut.calculate_score(problem, [pick])
    with pytest.raises(ValueError, match="is a reward"):
        max_cut.solve(problem, time_limit_seconds=0, constraints=[pick])
    duck = SimpleNamespace(name="cut_weight_cost", weight=None, hard=True, exponent=1.0)
    with pytest.raises(ValueError, match="'cut_weight_cost' is a reward"):
        max_cut.Solver(_zero()).solve_with_constraints(problem.to_core(), [duck])
    # hard=False re-states its level: allowed, and scores like the default.
    assert max_cut.calculate_score(problem, [CutWeightCost(hard=False)]) == (0, 2.0)
    assert engine_score(problem, [CutWeightCost(hard=False)]) == (0, 2.0)


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
        FixedPartitionCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="fixed_partition_cost", weight=-2.0, hard=None, exponent=1.0)
    flat = SimpleNamespace(name="fixed_partition_cost", weight=None, hard=None, exponent=0.0)
    unbounded = SimpleNamespace(name="fixed_partition_cost", weight=None, hard=None, exponent=1.0, offset=float("inf"))
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            max_cut.solve(plain(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            max_cut.constraint_costs(fixture_h(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        FixedPartitionCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="partition_size_bounds_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'partition_size_bounds_cost': {re.escape(message)}"):
        max_cut.solve(plain(), time_limit_seconds=0, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        PartitionSizeBoundsCost(**knobs)


def test_unknown_and_old_names_are_refused():
    """0.2.0 renamed the catalog; the 0.1.x names are unknown now (no aliases)."""
    old_names = (
        "fixed_partition",
        "same_partition_pairs",
        "different_partition_pairs",
        "partition_size_bounds",
        "partition_balance",
        "maximize_cut_weight",
    )
    raw = max_cut.constraint_costs(fixture_h())
    for old in (*old_names, "cut_weight_costs"):
        pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            max_cut.solve(plain(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])


def test_an_empty_pick_list_is_rejected():
    with pytest.raises(ValueError, match="must not be empty"):
        max_cut.solve(plain(), time_limit_seconds=0, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        max_cut.constraint_costs(fixture_h(), [])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [CutWeightCost(), CutWeightCost(weight=2), PartitionSizeBoundsCost()]
    assert engine_score(fixture_h(), picks) == (-2, 2.0 + 2 * 2.0)
    assert max_cut.calculate_score(fixture_h(), picks) == (-2, 6.0)
    assert get_constraints([FixedPartitionCost(), FixedPartitionCost()]) == [FixedPartitionCost(), FixedPartitionCost()]


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [fixed_partition_cost],
        [SamePartitionPairsCost(weight=3), DifferentPartitionPairsCost(hard=False), cut_weight_cost],
        [PartitionSizeBoundsCost(exponent=2.0), PartitionImbalanceCost(weight=0.5), CutWeightCost(weight=2)],
        [FixedPartitionCost(hard=False, exponent=0.5), CutWeightCost(exponent=1.5)],
        [
            SamePartitionPairsCost(weight=0.5),
            DifferentPartitionPairsCost(weight=0.5),
            PartitionSizeBoundsCost(weight=0.5),
        ],
        [*get_constraints(), PartitionSizeBoundsCost(weight=0.5, exponent=2.0), CutWeightCost(weight=0.5)],
        [
            PartitionSizeBoundsCost(offset=1.0),
            PartitionSizeBoundsCost(offset=-2.0, clip=False),
            CutWeightCost(offset=3),
        ],
        [
            SamePartitionPairsCost(offset=0.5, clip=False, weight=3),
            DifferentPartitionPairsCost(offset=1.0),
            PartitionImbalanceCost(weight=1, offset=1.0, exponent=2.0, clip=False),
            CutWeightCost(offset=4.0, clip=False),
            CutWeightCost(weight=0.5, offset=2.0, exponent=1.5),
        ],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs equals the engine score."""
    core = fixture_h().to_core()
    config = max_cut.SolverConfig()
    config.step_count_limit = 500
    config.random_seed = 2
    max_cut.Solver(config).solve_with_constraints(core, specs)
    python = score(max_cut.constraint_costs(core), specs)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)


def test_fractional_hard_weights_round_per_side():
    """Each side rounds on its own (half away from zero): 0.5 on [1, 1] -> -2, not round(1.0) = -1."""
    raw = max_cut.constraint_costs(fixture_h())
    picks = [PartitionSizeBoundsCost(weight=0.5)]
    assert score(raw, picks) == (-2, 0.0)
    assert engine_score(fixture_h(), picks) == (-2, 0.0)
    assert score(raw, [PartitionSizeBoundsCost(weight=0.4)]) == (0, 0.0)  # 0.1.x: round(0.8) = -1


def test_score_is_additive_over_the_picked_constraints():
    raw = max_cut.constraint_costs(solved())
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_equals_the_engine_for_the_default_assembly():
    problem = solved()
    engine = max_cut.calculate_score(problem.to_core())
    python = score(max_cut.constraint_costs(problem))
    assert (python.hard, python.soft) == (engine.hard, engine.soft)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def engine_solve(problem: MaxCutProblemModel, constraints) -> max_cut.MaxCutProblem:
    """The engine's zero-budget warm-start solve of the model's partition (it keeps the partition)."""
    core = problem.to_core()
    core.warm_start = True
    max_cut.Solver(_zero()).solve_with_constraints(core, constraints)
    return core


REPEAT = [PartitionSizeBoundsCost(), PartitionSizeBoundsCost(weight=2, exponent=2)]


def test_a_repeated_pick_by_hand():
    """size per side [1, 1] plus 2 x size^2 per side: (1 + 1) + 2 x (1 + 1) = 6 hard."""
    problem = fixture_h()
    raw = max_cut.constraint_costs(problem, REPEAT)
    assert list(raw) == ["partition_size_bounds_cost", "partition_size_bounds_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [1.0, 1.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 2.0, 2.0)
    assert first["name"] == second["name"] == "partition_size_bounds_cost"
    assert second["default_weight"] == 1.0
    expected = (-(2 + 2 * 2), 0.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(max_cut.constraint_costs(problem), REPEAT) == expected
    assert max_cut.calculate_score(problem, REPEAT) == expected
    engine = engine_solve(problem, REPEAT)
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


def test_identical_picks_count_twice():
    picks = [CutWeightCost(), CutWeightCost()]
    assert score(max_cut.constraint_costs(fixture_h()), picks) == (0, 4.0)
    assert engine_score(fixture_h(), picks) == (0, 4.0)
    assert list(max_cut.constraint_costs(fixture_h(), picks)) == ["cut_weight_cost", "cut_weight_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        DifferentPartitionPairsCost(weight=1),
        CutWeightCost(),
        DifferentPartitionPairsCost(weight=2),
        FixedPartitionCost(),
        DifferentPartitionPairsCost(weight=3),
    ]
    raw = max_cut.constraint_costs(fixture_h(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "fixed_partition_cost": 1.0,
        "different_partition_pairs_cost": 1.0,
        "different_partition_pairs_cost#2": 2.0,
        "different_partition_pairs_cost#3": 3.0,
        "cut_weight_cost": 1.0,
    }
    assert score(raw, picks) == (-(6 + 1), 2.0)
    assert engine_score(fixture_h(), picks) == score(raw, picks)


def test_a_second_pick_of_the_reward_still_refuses_hard():
    picks = [CutWeightCost(), CutWeightCost(hard=True)]
    with pytest.raises(ValueError, match="is a reward"):
        score(max_cut.constraint_costs(fixture_h()), picks)
    with pytest.raises(ValueError, match="is a reward"):
        max_cut.solve(fixture_h(), time_limit_seconds=0, constraints=picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Unit constraints, shaped per side: fixed [1, 0], same [1, 0], different [0, 1], size [1, 1].
        (DifferentPartitionPairsCost(offset=1, clip=False), (1, 0.0)),  # (0 - 1) + (1 - 1): the side of raw 0 too
        (DifferentPartitionPairsCost(offset=1), (0, 0.0)),
        (SamePartitionPairsCost(offset=2, clip=False), (3, 0.0)),  # a hard reward below 2 on both sides
        (SamePartitionPairsCost(offset=-1), (-2, 0.0)),  # a negative offset adds to the non-zero side only
        (SamePartitionPairsCost(offset=-1, clip=False), (-3, 0.0)),  # ...or to both sides
        (SamePartitionPairsCost(offset=1.5, clip=False), (3, 0.0)),  # llround(-0.5) + llround(-1.5): per side
        (FixedPartitionCost(weight=3, offset=0.5, clip=False), (0, 0.0)),  # 1.5 -> 2, -1.5 -> -2
        (PartitionSizeBoundsCost(offset=1, exponent=3, clip=False), (0, 0.0)),
        (PartitionSizeBoundsCost(hard=False, offset=0.5), (0, -1.0)),  # 0.5 + 0.5, soft: not rounded
        # The globals shape their single total: imbalance 2, cut reward 2.
        (PartitionImbalanceCost(weight=1, offset=1), (0, -1.0)),
        (PartitionImbalanceCost(weight=1, offset=3, clip=False), (0, 1.0)),  # shaped once, not per side
        (PartitionImbalanceCost(offset=3, clip=False), (0, 0.0)),  # the default weight 0 contributes 0
        (CutWeightCost(offset=1), (0, 1.0)),  # the reward beyond the offset only
        (CutWeightCost(offset=1, clip=False), (0, 3.0)),  # the offset adds to the reward
        (CutWeightCost(offset=5), (0, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    problem = fixture_h()
    assert score(max_cut.constraint_costs(problem), [pick]) == expected
    raw = max_cut.constraint_costs(problem, [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # both sides are always evaluated: no scoped constraint
    assert score(raw, [pick]) == expected
    assert max_cut.calculate_score(problem, [pick]) == expected
    engine = engine_solve(problem, [pick])
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain_picks = [PartitionSizeBoundsCost(exponent=2), SamePartitionPairsCost(), CutWeightCost(exponent=1.5)]
    shifted_by_zero = [
        PartitionSizeBoundsCost(exponent=2, offset=0.0, clip=False),
        SamePartitionPairsCost(offset=0),
        CutWeightCost(exponent=1.5, offset=0.0, clip=False),
    ]
    problem = solved()
    raw = max_cut.constraint_costs(problem)
    assert score(raw, shifted_by_zero) == score(raw, plain_picks)
    assert (
        engine_score(problem, plain_picks) == engine_score(problem, shifted_by_zero) == tuple(score(raw, plain_picks))
    )
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT],
        [
            FixedPartitionCost(),
            SamePartitionPairsCost(),
            SamePartitionPairsCost(offset=1, clip=False, weight=2),
            DifferentPartitionPairsCost(offset=-1),
            PartitionSizeBoundsCost(),
            PartitionSizeBoundsCost(offset=1, exponent=2, weight=0.5, hard=False),
            PartitionImbalanceCost(weight=0.25, offset=2, clip=False),
            CutWeightCost(offset=20, clip=False),
            CutWeightCost(weight=0.1, exponent=2, offset=5),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    data = json.loads((Path(__file__).parent / "fixtures" / "web_demo.json").read_text())
    core = MaxCutProblemModel.model_validate(data).to_core()
    config = max_cut.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    max_cut.Solver(config).solve_with_constraints(core, picks)
    raw = max_cut.constraint_costs(core, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)
    assert score(max_cut.constraint_costs(core), picks) == python  # the default costs score the repeats too
