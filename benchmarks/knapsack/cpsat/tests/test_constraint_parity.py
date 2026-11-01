"""C++ engine vs CP-SAT: the same selection gives the same raw cost per constraint.

For every selection of small feature-rich instances (CP-SAT's whole search
space) and 200 seeded random selections of a 10-item instance, each constraint
is selected alone at weight 1, the selection is pinned in CP-SAT (assumptions on
the item literals) and the CP-SAT term must equal the engine's
``constraint_costs()`` total for that constraint — 0 exactly when the engine
reports no violation.

Then the optimum: CP-SAT's optimum equals the brute-force optimum of the
engine's score, per constraint and for the default set; and the catalog and
the knobs (defaults, level="soft") agree.
"""

from __future__ import annotations

import random
from itertools import product

import pytest
from knapsack import constraint_costs, constraint_info, list_constraints
from knapsack.constraints import (
    ForbiddenItemsCost,
    KnapsackCapacityCost,
    ResourceCapacityCost,
    SelectedValueCost,
    get_constraints,
    score,
)
from knapsack.models import KnapsackProblemModel as Model
from ortools.sat.python import cp_model

from knapsack_cpsat import build, catalog, solve

# Every fact: two resources (one undeclared usage), repeated mandatory and
# forbidden entries, a bounded and an open group, repeated and self dependencies.
EVERYTHING = {
    "capacity": 12,
    "resource_capacities": {"labor": 3, "slots": 1},
    "mandatory_items": ["a", "a", "d"],
    "forbidden_items": ["e", "e"],
    "selection_groups": [
        {"id": "pick-one", "item_ids": ["b", "c", "e"], "min_selected": 1, "max_selected": 1},
        {"id": "at-least-two", "item_ids": ["a", "c", "d", "f"], "min_selected": 2},
    ],
    "items": [
        {"id": "a", "weight": 4, "value": 6, "resource_usage": {"labor": 2}, "requires": ["c", "c", "a"]},
        {"id": "b", "weight": 3, "value": 5, "resource_usage": {"slots": 1, "other": 7}},
        {"id": "c", "weight": 2, "value": 3, "resource_usage": {"labor": 2, "slots": 1}},
        {"id": "d", "weight": 5, "value": 8, "requires": ["b"]},
        {"id": "e", "weight": 1, "value": 9},
        {"id": "f", "weight": 6, "value": 4, "resource_usage": {"labor": 1}, "requires": ["e"]},
    ],
}

# The web demo (camping trip), trimmed to 7 items so every selection can be pinned.
CAMPING = {
    "capacity": 30,
    "resource_capacities": {"fragile_slots": 2},
    "mandatory_items": ["first-aid"],
    "selection_groups": [{"id": "large-gear", "item_ids": ["tent", "drone"], "min_selected": 0, "max_selected": 1}],
    "items": [
        {"id": "camera", "weight": 12, "value": 24, "resource_usage": {"fragile_slots": 1}},
        {"id": "battery", "weight": 8, "value": 18},
        {"id": "tent", "weight": 22, "value": 35},
        {"id": "food", "weight": 10, "value": 22},
        {"id": "first-aid", "weight": 6, "value": 15},
        {"id": "drone", "weight": 20, "value": 40, "resource_usage": {"fragile_slots": 2}, "requires": ["battery"]},
        {"id": "radio", "weight": 7, "value": 14},
    ],
}

# A plain 0/1 knapsack.
PLAIN = {
    "capacity": 10,
    "items": [
        {"id": "A", "weight": 6, "value": 9},
        {"id": "B", "weight": 4, "value": 7},
        {"id": "C", "weight": 5, "value": 8},
        {"id": "D", "weight": 3, "value": 4},
    ],
}

INSTANCES = {"everything": EVERYTHING, "camping": CAMPING, "plain": PLAIN}


def _random_instance(seed: int, n: int = 10) -> dict:
    rng = random.Random(seed)
    ids = [f"i{k}" for k in range(n)]
    items = []
    for item_id in ids:
        item = {"id": item_id, "weight": rng.randint(1, 15), "value": rng.randint(0, 20)}
        usage = {name: rng.randint(0, 5) for name in ("r0", "r1", "undeclared") if rng.random() < 0.5}
        if usage:
            item["resource_usage"] = usage
        if rng.random() < 0.3:
            item["requires"] = [rng.choice(ids) for _ in range(rng.randint(1, 2))]
        items.append(item)
    return {
        "capacity": sum(item["weight"] for item in items) // 3,
        "resource_capacities": {"r0": 8, "r1": 4},
        "mandatory_items": [ids[0], ids[0], ids[3]],
        "forbidden_items": [ids[5], ids[7], ids[7]],
        "selection_groups": [
            {"id": "g0", "item_ids": ids[1:5], "min_selected": 1, "max_selected": 2},
            {"id": "g1", "item_ids": ids[4:9], "min_selected": 2},
        ],
        "items": items,
    }


RANDOM = _random_instance(7)


def _selected(problem: dict, selection) -> Model:
    model = Model.model_validate(problem)
    for item, chosen in zip(model.items, selection, strict=True):
        item.selected = bool(chosen)
    return model


def _engine_raw(problem: dict, selection, name: str) -> float:
    return sum(constraint_costs(_selected(problem, selection).to_core())[name]["costs"])


def _single(name: str):
    """The constraint alone at weight 1, at its default level."""
    spec = next(spec for spec in get_constraints() if spec.name == name)
    return type(spec)(weight=1)


def _default_level(name: str) -> str:
    return next(row.default_level for row in constraint_info() if row.name == name)


class _Pinned:
    """One constraint's CP-SAT model, re-solved with the item literals pinned."""

    def __init__(self, problem: dict, name: str) -> None:
        self.built = build(Model.model_validate(problem), [_single(name)])
        self.built.objective.minimize()
        self.level = _default_level(name)

    def raw(self, selection) -> int:
        model = self.built.model
        model.clear_assumptions()
        model.add_assumptions(
            [literal if chosen else literal.Not() for literal, chosen in zip(self.built.context.chosen, selection)]
        )
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert solver.solve(model) == cp_model.OPTIMAL
        return -self.built.objective.score(solver)[self.level]


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_pinned_selections_cost_the_same_in_both_solvers(instance, name):
    problem = INSTANCES[instance]
    pinned = _Pinned(problem, name)
    n = len(problem["items"])
    nonzero = 0
    for selection in product((False, True), repeat=n):
        engine = _engine_raw(problem, selection, name)
        cpsat = pinned.raw(selection)
        assert cpsat == engine, f"{name} {selection}: CP-SAT {cpsat} != engine {engine}"
        nonzero += engine != 0
    if instance == "everything":
        assert nonzero > 0, f"{name} is never non-zero on {instance}: the parity check proves nothing"


@pytest.mark.parametrize("name", list_constraints())
def test_random_selections_cost_the_same_in_both_solvers(name):
    pinned = _Pinned(RANDOM, name)
    rng = random.Random(f"parity:{name}")
    n = len(RANDOM["items"])
    nonzero = 0
    for _ in range(200):
        selection = [rng.random() < 0.5 for _ in range(n)]
        engine = _engine_raw(RANDOM, selection, name)
        assert pinned.raw(selection) == engine, f"{name} {selection}"
        nonzero += engine != 0
    assert nonzero > 0


def test_raw_costs_match_by_hand():
    """{a, b, e} on EVERYTHING, every term by hand, in both solvers."""
    selection = [item["id"] in {"a", "b", "e"} for item in EVERYTHING["items"]]
    expected = {
        "knapsack_capacity_cost": 0,  # 4 + 3 + 1 = 8 <= 12
        "resource_capacity_cost": 0,  # labor 2 <= 3, slots 1 <= 1 ("other" is undeclared)
        "mandatory_items_cost": 1,  # a, a selected; d missing
        "forbidden_items_cost": 2,  # e listed twice, selected
        "selection_group_bounds_cost": 2,  # pick-one {b, e} = 2 > 1; at-least-two {a} = 1 < 2
        "item_dependency_cost": 2,  # a requires c twice (a -> a never counts)
        "selected_value_cost": -20,  # 6 + 5 + 9
    }
    for name, value in expected.items():
        assert _engine_raw(EVERYTHING, selection, name) == value, name
        assert _Pinned(EVERYTHING, name).raw(selection) == value, name


def _brute_force(problem: dict, selection) -> tuple:
    n = len(problem["items"])
    best = None
    for chosen in product((False, True), repeat=n):
        value = tuple(score(constraint_costs(_selected(problem, chosen).to_core()), selection))
        best = value if best is None or value > best else best
    return best


@pytest.mark.parametrize("instance", [*INSTANCES, "random"])
@pytest.mark.parametrize("name", list_constraints())
def test_each_constraint_matches_brute_force(instance, name):
    problem = RANDOM if instance == "random" else INSTANCES[instance]
    selection = [_single(name)]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, selection))


@pytest.mark.parametrize("instance", [*INSTANCES, "random"])
def test_the_default_set_matches_brute_force(instance):
    problem = RANDOM if instance == "random" else INSTANCES[instance]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, get_constraints()))


# --- The catalog and the knobs ---------------------------------------------------


def test_cpsat_defaults_are_the_engine_defaults():
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info(), in the same order."""
    engine = {row.name: row for row in constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == ("soft" if row.reward else "either")
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0}


def test_demoting_a_hard_constraint_scores_like_the_engine():
    """level="soft" moves the term to soft in both solvers; the CP-SAT result rescores identically."""
    selection = [
        ForbiddenItemsCost(level="soft") if spec.name == "forbidden_items_cost" else spec for spec in get_constraints()
    ]
    solved = solve(Model.model_validate(EVERYTHING), time_limit_seconds=10, constraints=selection)
    engine = score(constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(EVERYTHING, selection))


def test_reweighting_scores_like_the_engine():
    selection = [
        KnapsackCapacityCost(level="soft", weight=3),
        ResourceCapacityCost(weight=2),
        SelectedValueCost(weight=2),
    ]
    solved = solve(Model.model_validate(RANDOM), time_limit_seconds=10, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(RANDOM, selection))


def test_the_reward_refuses_hard_true_in_cpsat_too():
    with pytest.raises(ValueError, match="is a reward"):
        solve(Model.model_validate(PLAIN), time_limit_seconds=1, constraints=[SelectedValueCost(level="hard")])


def test_exponents_and_fractional_weights_stay_unsupported():
    with pytest.raises(ValueError, match="exponent is not supported"):
        solve(Model.model_validate(PLAIN), time_limit_seconds=1, constraints=[KnapsackCapacityCost(exponent=2)])
    with pytest.raises(ValueError, match="must be an integer"):
        solve(Model.model_validate(PLAIN), time_limit_seconds=1, constraints=[KnapsackCapacityCost(weight=0.5)])


def test_selections_by_name_and_mapping_work():
    solved = solve(
        Model.model_validate(PLAIN),
        time_limit_seconds=5,
        constraints=["knapsack_capacity_cost", {"name": "selected_value_cost", "weight": 1}],
    )
    assert (solved.score_hard, solved.score_soft) == (0, 16.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve(Model.model_validate(PLAIN), time_limit_seconds=1, constraints=["capacity"])
