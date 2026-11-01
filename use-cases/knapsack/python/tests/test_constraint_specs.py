"""knapsack.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import knapsack
from knapsack._constraint import instance_keys
from knapsack.constraints import (
    Constraint,
    ForbiddenItemsCost,
    ItemDependencyCost,
    KnapsackCapacityCost,
    Level,
    MandatoryItemsCost,
    ResourceCapacityCost,
    SelectedValueCost,
    SelectionGroupBoundsCost,
    forbidden_items_cost,
    get_constraints,
    item_dependency_cost,
    knapsack_capacity_cost,
    mandatory_items_cost,
    resource_capacity_cost,
    score,
    selected_value_cost,
    selection_group_bounds_cost,
)
from knapsack.models import ItemModel, KnapsackProblemModel, SelectionGroupModel

NAMES = [
    "knapsack_capacity_cost",
    "resource_capacity_cost",
    "mandatory_items_cost",
    "forbidden_items_cost",
    "selection_group_bounds_cost",
    "item_dependency_cost",
    "selected_value_cost",
]


def extended(selected=("A",)) -> KnapsackProblemModel:
    """The 0.1.x extended fixture: with only A selected, each of five hard terms is 1."""
    return KnapsackProblemModel(
        capacity=10,
        resource_capacities={"labor": 2},
        mandatory_items=["C"],
        forbidden_items=["A"],
        selection_groups=[
            SelectionGroupModel(id="choose-two", item_ids=["A", "B", "C"], min_selected=2, max_selected=2)
        ],
        items=[
            ItemModel(id="A", weight=3, value=5, resource_usage={"labor": 3}, requires=["B"], selected="A" in selected),
            ItemModel(id="B", weight=2, value=4, selected="B" in selected),
            ItemModel(id="C", weight=1, value=1, selected="C" in selected),
        ],
    )


def basic() -> KnapsackProblemModel:
    return KnapsackProblemModel(
        capacity=10,
        items=[
            ItemModel(id="A", weight=6, value=9),
            ItemModel(id="B", weight=4, value=7),
            ItemModel(id="C", weight=5, value=8),
        ],
    )


def solved(constraints=None, problem=None) -> KnapsackProblemModel:
    problem = problem if problem is not None else extended()
    config = knapsack.SolverConfig()
    config.step_count_limit = 2000
    config.random_seed = 1
    knapsack.solve(problem, config=config, constraints=constraints)
    return problem


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    assert knapsack.list_constraints() == NAMES
    for spec in get_constraints():
        assert spec.name in NAMES


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == knapsack.list_constraints()


def test_module_level_instances_keep_every_default():
    """Module-level instances keep the defaults (weight=None, level=None, exponent=1)."""
    instances = (
        knapsack_capacity_cost,
        resource_capacity_cost,
        mandatory_items_cost,
        forbidden_items_cost,
        selection_group_bounds_cost,
        item_dependency_cost,
        selected_value_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
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
    rows = knapsack.constraint_info()
    assert [row.name for row in rows] == knapsack.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [("hard", 1.0, True, False, True)] * 6 + [("soft", 1.0, False, True, True)]


def test_constraint_info_rows_are_plain_values():
    row = knapsack.constraint_info()[-1]
    assert isinstance(row, knapsack.ConstraintInfo)
    assert row._asdict() == {
        "name": "selected_value_cost",
        "default_level": "soft",
        "default_weight": 1.0,
        "level_flippable": False,
        "reward": True,
        "default_enabled": True,
    }
    assert repr(knapsack.constraint_info()[0]).startswith("ConstraintInfo(name='knapsack_capacity_cost'")
    assert pickle.loads(pickle.dumps(knapsack.constraint_info())) == knapsack.constraint_info()
    assert knapsack.ConstraintInfo.__module__ == knapsack._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults for every constraint, one global total each."""
    raw = knapsack.constraint_costs(extended())
    assert list(raw) == NAMES
    for row in knapsack.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (row.level_flippable, row.reward)
        assert entry["global"] is True and len(entry["costs"]) == 1
        assert (entry["name"], entry["label"], entry["exponent"]) == (row.name, "", 1.0)
        assert "medium_fold" not in entry  # no medium level in this domain


# --- Raw costs by hand ----------------------------------------------------------


def test_constraint_costs_match_the_hand_calculation():
    """Extended fixture with A: one unit for each of five hard constraints, value 5."""
    raw = knapsack.constraint_costs(extended())
    assert {name: entry["costs"] for name, entry in raw.items()} == {
        "knapsack_capacity_cost": [0.0],
        "resource_capacity_cost": [1.0],
        "mandatory_items_cost": [1.0],
        "forbidden_items_cost": [1.0],
        "selection_group_bounds_cost": [1.0],
        "item_dependency_cost": [1.0],
        "selected_value_cost": [-5.0],
    }
    assert score(raw) == (-5, 5.0)
    clean = knapsack.constraint_costs(extended(selected=("B", "C")))
    assert {name: entry["costs"] for name, entry in clean.items()} == {
        **{name: [0.0] for name in NAMES[:-1]},
        "selected_value_cost": [-5.0],
    }


def test_globals_shape_their_total():
    """0.2.0: the exponent shapes each constraint's total (0.1.x shaped each term)."""
    problem = KnapsackProblemModel(
        capacity=100,
        resource_capacities={"r1": 1, "r2": 0},
        mandatory_items=["C", "C", "C"],
        items=[
            ItemModel(id="A", weight=6, value=9, resource_usage={"r1": 3, "r2": 1}, selected=True),
            ItemModel(id="B", weight=4, value=7, resource_usage={"r2": 2}, selected=True),
            ItemModel(id="C", weight=5, value=8),
        ],
    )
    raw = knapsack.constraint_costs(problem)
    assert raw["resource_capacity_cost"]["costs"] == [5.0]  # overflows 2 + 3
    assert score(raw, [ResourceCapacityCost(exponent=2)]) == (-25, 0.0)  # (2 + 3)^2, not 2^2 + 3^2
    assert score(raw, [MandatoryItemsCost(exponent=2)]) == (-9, 0.0)  # 3 misses -> 3^2
    assert score(raw, [SelectedValueCost(exponent=2)]) == (0, 256.0)  # +(16^2)
    assert engine_score(problem, [ResourceCapacityCost(exponent=2)]) == (-25, 0.0)
    assert engine_score(problem, [MandatoryItemsCost(exponent=2)]) == (-9, 0.0)
    assert engine_score(problem, [SelectedValueCost(exponent=2)]) == (0, 256.0)


def _zero() -> knapsack.SolverConfig:
    config = knapsack.SolverConfig()
    config.spent_limit_seconds = 0
    return config


def engine_score(problem: KnapsackProblemModel, constraints) -> tuple[int, float]:
    """The engine's score of the model's selection as it stands (a warm start, zero budget)."""
    core = problem.to_core()
    core.warm_start = True
    knapsack.Solver(_zero()).solve_with_constraints(core, constraints)
    return core.score.hard, core.score.soft


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weight 1 everywhere."""
    explicit = [type(spec)(weight=1) for spec in get_constraints()]
    problem = solved()
    raw = knapsack.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (problem.score_hard, problem.score_soft)


def test_default_solve_equals_explicit_default_constraints():
    plain = solved()
    explicit = solved(get_constraints())
    assert (plain.score_hard, plain.score_soft) == (explicit.score_hard, explicit.score_soft)


def test_picking_a_subset_drops_the_other_terms():
    problem = extended()
    hard_only = knapsack.calculate_score(problem, [mandatory_items_cost, forbidden_items_cost])
    assert hard_only == (-2, 0.0)
    assert knapsack.calculate_score(problem, [selected_value_cost]) == (0, 5.0)


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft moves its cost to the soft level."""
    problem = extended()
    flipped = [
        spec if spec.name != "forbidden_items_cost" else ForbiddenItemsCost(level="soft") for spec in get_constraints()
    ]
    assert knapsack.calculate_score(problem, flipped) == (-4, 4.0)
    assert engine_score(problem, flipped) == (-4, 4.0)


def test_reweighting_scales_only_that_term():
    problem = extended()
    heavier = [
        spec if spec.name != "item_dependency_cost" else ItemDependencyCost(weight=7) for spec in get_constraints()
    ]
    assert knapsack.calculate_score(problem, heavier) == (-11, 5.0)


@pytest.mark.parametrize("level", ["hard", Level.HARD])
def test_the_reward_refuses_level_hard_on_every_path(level):
    """selected_value_cost is a reward (raw <= 0): level="hard" raises, in Python and through C++."""
    pick = SelectedValueCost(level=level)  # the spec itself is a plain value
    problem = extended()
    with pytest.raises(ValueError, match="is a reward"):
        score(knapsack.constraint_costs(problem), [pick])
    with pytest.raises(ValueError, match="is a reward"):
        knapsack.calculate_score(problem, [pick])
    with pytest.raises(ValueError, match="is a reward"):
        knapsack.solve(problem, time_limit_seconds=0, constraints=[pick])
    with pytest.raises(ValueError, match="is a reward"):
        knapsack.constraint_costs(problem, [pick])
    duck = SimpleNamespace(name="selected_value_cost", weight=None, level="hard", exponent=1.0)
    with pytest.raises(ValueError, match="'selected_value_cost' is a reward"):
        knapsack.Solver(_zero()).solve_with_constraints(problem.to_core(), [duck])
    # level="soft" re-states its level: allowed, and scores like the default.
    assert knapsack.calculate_score(problem, [SelectedValueCost(level="soft")]) == (0, 5.0)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": float("inf")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"exponent": -1.0}, ValueError),
        ({"level": 1}, TypeError),
        ({"level": "firm"}, ValueError),
        ({"hard": True}, TypeError),  # the removed knob is an unknown keyword
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        KnapsackCapacityCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="knapsack_capacity_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="knapsack_capacity_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(
        name="knapsack_capacity_cost", weight=None, level=None, exponent=1.0, offset=float("inf")
    )
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            knapsack.solve(basic(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            knapsack.constraint_costs(basic(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        KnapsackCapacityCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="mandatory_items_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'mandatory_items_cost': {re.escape(message)}"):
        knapsack.solve(basic(), time_limit_seconds=0, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        MandatoryItemsCost(**knobs)


def test_unknown_and_old_names_are_refused():
    """0.2.0 renamed the catalog; the 0.1.x names are unknown now (no aliases)."""
    old_names = (
        "capacity",
        "resource_capacities",
        "mandatory_items",
        "forbidden_items",
        "selection_group_bounds",
        "item_dependencies",
        "maximize_value",
    )
    raw = knapsack.constraint_costs(extended())
    for old in (*old_names, "knapsack_capacity_costs"):
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            knapsack.solve(basic(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])


def test_an_empty_pick_list_is_rejected():
    with pytest.raises(ValueError, match="must not be empty"):
        knapsack.solve(basic(), time_limit_seconds=0, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        knapsack.constraint_costs(basic(), [])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    problem = basic()
    problem.items[0].selected = problem.items[2].selected = True  # A + C: 11 on 10
    picks = [KnapsackCapacityCost(), KnapsackCapacityCost(weight=2), SelectedValueCost()]
    assert engine_score(problem, picks) == (-(1 + 2), 17.0)
    assert knapsack.calculate_score(problem, picks) == (-3, 17.0)
    assert get_constraints([SelectedValueCost(), SelectedValueCost()]) == [SelectedValueCost(), SelectedValueCost()]


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [knapsack_capacity_cost],
        [ResourceCapacityCost(weight=3), MandatoryItemsCost(level="soft"), selected_value_cost],
        [SelectionGroupBoundsCost(exponent=2.0), ItemDependencyCost(weight=0.5), SelectedValueCost(weight=2)],
        [ForbiddenItemsCost(level="soft", exponent=0.5), SelectedValueCost(exponent=1.5)],
        [KnapsackCapacityCost(weight=0.5), MandatoryItemsCost(weight=0.5), ForbiddenItemsCost(weight=0.5)],
        [*get_constraints(), ResourceCapacityCost(weight=0.5, exponent=2.0), SelectedValueCost(weight=0.5)],
        [ResourceCapacityCost(offset=1.0), ResourceCapacityCost(offset=-2.0, clip=False), SelectedValueCost(offset=3)],
        [
            MandatoryItemsCost(offset=0.5, clip=False, weight=3),
            ForbiddenItemsCost(offset=1.0),
            SelectionGroupBoundsCost(level="soft", offset=1.0, exponent=2.0, clip=False),
            SelectedValueCost(offset=4.0, clip=False),
            SelectedValueCost(weight=0.5, offset=2.0, exponent=1.5),
        ],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs equals the engine score."""
    problem = extended()
    core = problem.to_core()
    config = knapsack.SolverConfig()
    config.step_count_limit = 500
    config.random_seed = 2
    knapsack.Solver(config).solve_with_constraints(core, specs)
    python = score(knapsack.constraint_costs(core), specs)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)


def test_fractional_hard_weights_round_per_constraint():
    """Each hard constraint rounds on its own (half away from zero): 0.5 + 0.5 -> -2, not -1."""
    raw = knapsack.constraint_costs(extended())
    picks = [MandatoryItemsCost(weight=0.5), ForbiddenItemsCost(weight=0.5)]
    assert score(raw, picks) == (-2, 0.0)
    assert engine_score(extended(), picks) == (-2, 0.0)


def test_score_is_additive_over_the_picked_constraints():
    raw = knapsack.constraint_costs(solved())
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_equals_the_engine_for_the_default_assembly():
    problem = solved()
    engine = knapsack.calculate_score(problem.to_core())
    python = score(knapsack.constraint_costs(problem))
    assert (python.hard, python.soft) == (engine.hard, engine.soft)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def shifted() -> KnapsackProblemModel:
    """A + B selected (weight 10 of 100): resource overflow 2 + 3 = 5, mandatory C missing
    three times, value 9 + 7 = 16; the capacity, forbidden, group and dependency totals are 0."""
    return KnapsackProblemModel(
        capacity=100,
        resource_capacities={"r1": 1, "r2": 0},
        mandatory_items=["C", "C", "C"],
        items=[
            ItemModel(id="A", weight=6, value=9, resource_usage={"r1": 3, "r2": 1}, selected=True),
            ItemModel(id="B", weight=4, value=7, resource_usage={"r2": 2}, selected=True),
            ItemModel(id="C", weight=5, value=8),
        ],
    )


def engine_solve(problem: KnapsackProblemModel, constraints) -> knapsack.KnapsackProblem:
    """The engine's zero-budget warm-start solve of the model's selection (it keeps the selection)."""
    core = problem.to_core()
    core.warm_start = True
    knapsack.Solver(_zero()).solve_with_constraints(core, constraints)
    return core


REPEAT = [MandatoryItemsCost(), MandatoryItemsCost(weight=2, exponent=2)]


def test_a_repeated_pick_by_hand():
    """mandatory x 1 plus 2 x mandatory^2 on the total 3: 3 + 2 x 9 = 21 hard."""
    problem = shifted()
    raw = knapsack.constraint_costs(problem, REPEAT)
    assert list(raw) == ["mandatory_items_cost", "mandatory_items_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [3.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 2.0, 2.0)
    assert first["name"] == second["name"] == "mandatory_items_cost"
    assert second["default_weight"] == 1.0
    expected = (-(3 + 2 * 3**2), 0.0)
    assert expected == (-21, 0.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw total.
    assert score(knapsack.constraint_costs(problem), REPEAT) == expected
    assert knapsack.calculate_score(problem, REPEAT) == expected
    engine = engine_solve(problem, REPEAT)
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


def test_identical_picks_count_twice():
    picks = [SelectedValueCost(), SelectedValueCost()]
    assert score(knapsack.constraint_costs(shifted()), picks) == (0, 32.0)
    assert engine_score(shifted(), picks) == (0, 32.0)
    assert list(knapsack.constraint_costs(shifted(), picks)) == ["selected_value_cost", "selected_value_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        ResourceCapacityCost(weight=1),
        SelectedValueCost(),
        ResourceCapacityCost(weight=2),
        MandatoryItemsCost(),
        ResourceCapacityCost(weight=3),
    ]
    raw = knapsack.constraint_costs(shifted(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "resource_capacity_cost": 1.0,
        "resource_capacity_cost#2": 2.0,
        "resource_capacity_cost#3": 3.0,
        "mandatory_items_cost": 1.0,
        "selected_value_cost": 1.0,
    }
    assert score(raw, picks) == (-(5 * 6 + 3), 16.0)
    assert engine_score(shifted(), picks) == score(raw, picks)


def test_a_second_pick_of_the_reward_still_refuses_hard():
    picks = [SelectedValueCost(), SelectedValueCost(level="hard")]
    with pytest.raises(ValueError, match="is a reward"):
        score(knapsack.constraint_costs(shifted()), picks)
    with pytest.raises(ValueError, match="is a reward"):
        knapsack.solve(shifted(), time_limit_seconds=0, constraints=picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Every knapsack constraint is global: the offset shapes its single total.
        (ResourceCapacityCost(offset=2), (-3, 0.0)),
        (ResourceCapacityCost(offset=7), (0, 0.0)),
        (ResourceCapacityCost(offset=7, clip=False), (2, 0.0)),  # 5 - 7: a hard reward below 7
        (ResourceCapacityCost(offset=2, exponent=3), (-27, 0.0)),
        (KnapsackCapacityCost(offset=4, clip=False), (4, 0.0)),  # a raw 0 is charged in literal mode
        (KnapsackCapacityCost(offset=-1), (0, 0.0)),  # ...and left alone by the threshold
        (KnapsackCapacityCost(offset=-1, clip=False), (-1, 0.0)),
        (MandatoryItemsCost(offset=-1), (-4, 0.0)),  # a negative offset adds to a non-zero total
        (KnapsackCapacityCost(weight=3, offset=0.5, clip=False), (2, 0.0)),  # 3 x -0.5 = -1.5 -> -2
        (ItemDependencyCost(level="soft", offset=2, clip=False), (0, 2.0)),
        (ForbiddenItemsCost(weight=0, offset=5, clip=False), (0, 0.0)),  # weight 0 contributes 0
        # The reward (raw -16): the threshold rewards only the value beyond the offset,
        # the literal formula adds the offset to the reward.
        (SelectedValueCost(offset=10), (0, 6.0)),
        (SelectedValueCost(offset=10, clip=False), (0, 26.0)),
        (SelectedValueCost(offset=-4, clip=False), (0, 12.0)),
        (SelectedValueCost(offset=20), (0, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    problem = shifted()
    assert score(knapsack.constraint_costs(problem), [pick]) == expected
    raw = knapsack.constraint_costs(problem, [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # every knapsack constraint is global, none is scoped
    assert score(raw, [pick]) == expected
    assert knapsack.calculate_score(problem, [pick]) == expected
    engine = engine_solve(problem, [pick])
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [ResourceCapacityCost(exponent=2), MandatoryItemsCost(), SelectedValueCost(exponent=1.5)]
    shifted_by_zero = [
        ResourceCapacityCost(exponent=2, offset=0.0, clip=False),
        MandatoryItemsCost(offset=0),
        SelectedValueCost(exponent=1.5, offset=0.0, clip=False),
    ]
    problem = solved()
    raw = knapsack.constraint_costs(problem)
    assert score(raw, shifted_by_zero) == score(raw, plain)
    assert engine_score(problem, plain) == engine_score(problem, shifted_by_zero) == tuple(score(raw, plain))
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT],
        [
            KnapsackCapacityCost(),
            KnapsackCapacityCost(offset=2, clip=False, weight=2),
            ResourceCapacityCost(offset=3),
            ResourceCapacityCost(offset=-1, clip=False, weight=0.25),
            MandatoryItemsCost(),
            ForbiddenItemsCost(),
            SelectionGroupBoundsCost(exponent=2, offset=1),
            ItemDependencyCost(),
            SelectedValueCost(offset=20, clip=False),
            SelectedValueCost(weight=0.1, exponent=2, offset=5),
        ],
        [
            *get_constraints(),
            KnapsackCapacityCost(level="soft", weight=1.5, offset=1, clip=False),
            ForbiddenItemsCost(level=Level.SOFT, weight=2, offset=-1),
            ItemDependencyCost(level="soft", exponent=2, offset=1),
            SelectedValueCost(level="soft", weight=0.5, offset=3, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets", "repeats-on-other-levels"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    problem = extended()
    core = problem.to_core()
    config = knapsack.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    knapsack.Solver(config).solve_with_constraints(core, picks)
    raw = knapsack.constraint_costs(core, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)
    assert score(knapsack.constraint_costs(core), picks) == python  # the default costs score the repeats too


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([ResourceCapacityCost(level="soft")], (0, -5.0)),
        ([MandatoryItemsCost(level=Level.SOFT, weight=2)], (0, -6.0)),
        ([KnapsackCapacityCost(level="soft", offset=1, clip=False)], (0, 1.0)),  # a raw 0 - 1, on soft
        ([ForbiddenItemsCost(level=Level.HARD)], (0, 0.0)),  # the default level, named
        ([SelectedValueCost(level=Level.SOFT)], (0, 16.0)),  # the reward's own level, named
        # A repeat carries its own level and shaping. Hard: the resource overflow 5 and the
        # mandatory misses beyond 1 (3 - 1). Soft: 3 x (5 - 1) on the literal resource repeat,
        # the 3 misses, the value 16 and 2 x (16 - 10) beyond the reward's threshold:
        # -12 - 3 + 16 + 12.
        (
            [
                ResourceCapacityCost(),
                ResourceCapacityCost(level="soft", weight=3, offset=1, clip=False),
                MandatoryItemsCost(level="soft"),
                MandatoryItemsCost(level=Level.HARD, offset=1),
                SelectedValueCost(),
                SelectedValueCost(weight=2, offset=10),
            ],
            (-7, 13.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    problem = shifted()
    assert score(knapsack.constraint_costs(problem), picks) == expected
    raw = knapsack.constraint_costs(problem, picks)
    for pick, key in zip(picks, instance_keys(picks), strict=True):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
        assert raw[key]["hard"] == (raw[key]["level"] == "hard"), key
    assert score(raw, picks) == expected
    assert knapsack.calculate_score(problem, picks) == expected
    engine = engine_solve(problem, picks)
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    message = "constraint 'knapsack_capacity_cost': this use case has no medium level"
    pick = KnapsackCapacityCost(level=level)  # the spec cannot know the domain
    for picks in ([pick], [KnapsackCapacityCost(), pick]):
        with pytest.raises(ValueError, match=message):
            engine_solve(shifted(), picks)
        with pytest.raises(ValueError, match=message):
            solved(picks)
        with pytest.raises(ValueError, match=message):
            knapsack.constraint_costs(shifted(), picks)
        with pytest.raises(ValueError, match=message):
            score(knapsack.constraint_costs(shifted()), picks)
        with pytest.raises(ValueError, match=message):
            knapsack.calculate_score(shifted(), picks)
    # The reward is refused the same way, before its reward rule is looked at.
    reward = [SelectedValueCost(), SelectedValueCost(level=level)]
    message = "constraint 'selected_value_cost': this use case has no medium level"
    with pytest.raises(ValueError, match=message):
        engine_solve(shifted(), reward)
    with pytest.raises(ValueError, match=message):
        score(knapsack.constraint_costs(shifted()), reward)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        MandatoryItemsCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        MandatoryItemsCost(level="firm")
    pick = SimpleNamespace(name="mandatory_items_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        engine_solve(shifted(), [MandatoryItemsCost(), pick])


def test_the_knob_was_renamed():
    """0.3.0 replaced hard=True / hard=False by level="hard" / level="soft"; hard= is unknown."""
    with pytest.raises(TypeError):
        MandatoryItemsCost(hard=True)
    with pytest.raises(TypeError):
        ForbiddenItemsCost(weight=None, hard=False)
    assert MandatoryItemsCost(level="soft") == MandatoryItemsCost(level=Level.SOFT)
