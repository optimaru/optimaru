"""knapsack.constraints — an API for picking the cost function declaratively."""

import pickle
import re
from types import SimpleNamespace

import pytest

import knapsack
from knapsack.constraints import (
    Constraint,
    ForbiddenItemsCost,
    ItemDependencyCost,
    KnapsackCapacityCost,
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
    """Module-level instances keep the defaults (weight=None, hard=None, exponent=1)."""
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
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
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
        spec if spec.name != "forbidden_items_cost" else ForbiddenItemsCost(hard=False) for spec in get_constraints()
    ]
    assert knapsack.calculate_score(problem, flipped) == (-4, 4.0)
    assert engine_score(problem, flipped) == (-4, 4.0)


def test_reweighting_scales_only_that_term():
    problem = extended()
    heavier = [
        spec if spec.name != "item_dependency_cost" else ItemDependencyCost(weight=7) for spec in get_constraints()
    ]
    assert knapsack.calculate_score(problem, heavier) == (-11, 5.0)


def test_the_reward_refuses_hard_true_on_every_path():
    """selected_value_cost is a reward (raw <= 0): hard=True raises, in Python and through C++."""
    pick = SelectedValueCost(hard=True)  # the spec itself is a plain value
    problem = extended()
    with pytest.raises(ValueError, match="is a reward"):
        score(knapsack.constraint_costs(problem), [pick])
    with pytest.raises(ValueError, match="is a reward"):
        knapsack.calculate_score(problem, [pick])
    with pytest.raises(ValueError, match="is a reward"):
        knapsack.solve(problem, time_limit_seconds=0, constraints=[pick])
    duck = SimpleNamespace(name="selected_value_cost", weight=None, hard=True, exponent=1.0)
    with pytest.raises(ValueError, match="'selected_value_cost' is a reward"):
        knapsack.Solver(_zero()).solve_with_constraints(problem.to_core(), [duck])
    # hard=False re-states its level: allowed, and scores like the default.
    assert knapsack.calculate_score(problem, [SelectedValueCost(hard=False)]) == (0, 5.0)


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
        KnapsackCapacityCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="knapsack_capacity_cost", weight=-2.0, hard=None, exponent=1.0)
    flat = SimpleNamespace(name="knapsack_capacity_cost", weight=None, hard=None, exponent=0.0)
    for pick, message in ((negative, "weight must be finite and >= 0"), (flat, "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            knapsack.solve(basic(), time_limit_seconds=0, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="mandatory_items_cost", weight=None, hard=None, exponent=1.0)
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
        pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            knapsack.solve(basic(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])


def test_empty_and_repeated_picks_are_rejected():
    with pytest.raises(ValueError, match="must not be empty"):
        knapsack.solve(basic(), time_limit_seconds=0, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])
    with pytest.raises(ValueError, match="picked more than once"):
        knapsack.solve(
            basic(), time_limit_seconds=0, constraints=[knapsack_capacity_cost, KnapsackCapacityCost(weight=2)]
        )
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([selected_value_cost, selected_value_cost])


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [knapsack_capacity_cost],
        [ResourceCapacityCost(weight=3), MandatoryItemsCost(hard=False), selected_value_cost],
        [SelectionGroupBoundsCost(exponent=2.0), ItemDependencyCost(weight=0.5), SelectedValueCost(weight=2)],
        [ForbiddenItemsCost(hard=False, exponent=0.5), SelectedValueCost(exponent=1.5)],
        [KnapsackCapacityCost(weight=0.5), MandatoryItemsCost(weight=0.5), ForbiddenItemsCost(weight=0.5)],
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
