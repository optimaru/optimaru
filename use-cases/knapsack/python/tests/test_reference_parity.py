"""The C++ port computes exactly what the 0.1.5 pure-Python package computed.

`reference_knapsack` is the frozen 0.1.5 scorer and greedy construction (an
oracle, plain data in and out). On every sample — the web demo
(`fixtures/web_demo.json`, a copy of the web app's sample) and the package's own
test fixtures — and on seeded random instances, the C++ `constraint_costs`
totals equal the oracle's per constraint (under the 0.2.0 names), the default
score equals the oracle's, and the greedy construction (the zero-budget solve and
`with_initial_solution`'s fill) returns the oracle's selection.

0.1.5 listed one term per resource / entry / group / pair; 0.2.0 reports one
total per constraint (every knapsack constraint is global). The totals are what
must match; the documented differences (the exponent shapes the total, hard
rounds per constraint, a hard reward is refused) have their own tests below.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import pytest
import reference_knapsack as ref

import knapsack
from knapsack import with_initial_solution
from knapsack.constraints import (
    Constraint,
    ForbiddenItemsCost,
    KnapsackCapacityCost,
    MandatoryItemsCost,
    SelectedValueCost,
    get_constraints,
    score,
)
from knapsack.models import KnapsackProblemModel as Model

FIXTURES = Path(__file__).parent / "fixtures"


def _samples() -> dict[str, dict]:
    abc = [
        {"id": "A", "weight": 6, "value": 9},
        {"id": "B", "weight": 4, "value": 7},
        {"id": "C", "weight": 5, "value": 8},
    ]
    return {
        "web-demo": json.loads((FIXTURES / "web_demo.json").read_text()),
        # test_knapsack.py / test_initial_solution.py
        "basic": {"capacity": 10, "items": abc},
        "extended": {
            "capacity": 10,
            "resource_capacities": {"labor": 2},
            "mandatory_items": ["C"],
            "forbidden_items": ["A"],
            "selection_groups": [
                {"id": "choose-two", "item_ids": ["A", "B", "C"], "min_selected": 2, "max_selected": 2}
            ],
            "items": [
                {
                    "id": "A",
                    "weight": 3,
                    "value": 5,
                    "selected": True,
                    "resource_usage": {"labor": 3},
                    "requires": ["B"],
                },
                {"id": "B", "weight": 2, "value": 4},
                {"id": "C", "weight": 1, "value": 1},
            ],
        },
        "hard-infeasible": {
            "capacity": 3,
            "mandatory_items": ["A"],
            "items": [{"id": "A", "weight": 5, "value": 1}, {"id": "B", "weight": 1, "value": 1}],
        },
        # benchmarks/knapsack/cpsat tests/test_exact.py
        "exact": {
            "capacity": 8,
            "resource_capacities": {"labor": 3},
            "mandatory_items": ["A"],
            "forbidden_items": ["C"],
            "selection_groups": [{"id": "pair", "item_ids": ["A", "B", "C"], "min_selected": 2, "max_selected": 2}],
            "items": [
                {"id": "A", "weight": 3, "value": 8, "resource_usage": {"labor": 1}, "requires": ["B"]},
                {"id": "B", "weight": 4, "value": 7, "resource_usage": {"labor": 2}},
                {"id": "C", "weight": 1, "value": 100},
            ],
        },
        # Repeats, self-dependencies, undeclared resources, a group without a maximum.
        "repeats": {
            "capacity": 9,
            "resource_capacities": {"z": 2, "a": 0},
            "mandatory_items": ["C", "C", "D"],
            "forbidden_items": ["B", "B"],
            "selection_groups": [
                {"id": "open", "item_ids": ["A", "B", "C", "D"], "min_selected": 3},
                {"id": "cap", "item_ids": ["B", "D"], "max_selected": 1},
            ],
            "items": [
                {"id": "A", "weight": 4, "value": 6, "resource_usage": {"z": 2, "q": 5}, "requires": ["A", "C", "C"]},
                {"id": "B", "weight": 3, "value": 5, "resource_usage": {"a": 1}, "requires": ["D"]},
                {"id": "C", "weight": 2, "value": 2, "resource_usage": {"z": 1}},
                {"id": "D", "weight": 5, "value": 9, "requires": ["B"]},
            ],
        },
    }


def _with_selection(problem: dict, selected: list[bool], *, warm_start: bool = False) -> dict:
    data = json.loads(json.dumps(problem))
    for item, chosen in zip(data["items"], selected, strict=True):
        item["selected"] = chosen
    data["warm_start"] = warm_start
    return data


def _dump(data: dict) -> dict:
    return Model.model_validate(data).model_dump()


def _cpp_totals(data: dict) -> dict[str, float]:
    costs = knapsack.constraint_costs(Model.model_validate(data).to_core())
    return {name: sum(entry["costs"]) for name, entry in costs.items()}


def _ref_totals(data: dict) -> dict[str, float]:
    return {ref.RENAMED[name]: sum(terms) for name, terms in ref.constraint_costs(_dump(data)).items()}


def _cpp_score(data: dict) -> tuple[int, float]:
    result = knapsack.calculate_score(Model.model_validate(data).to_core())
    return result.hard, result.soft


def _assert_parity(data: dict) -> None:
    """Per-constraint raw totals, the order and the default score equal the oracle."""
    totals = _cpp_totals(data)
    expected = _ref_totals(data)
    assert list(totals) == list(expected)
    assert totals == expected
    assert _cpp_score(data) == ref.calculate_score(_dump(data))
    model = Model.model_validate(data)
    knapsack.calculate_score(model)
    assert (model.total_weight, model.total_value) == ref.totals(_dump(data))


def _random_selection(problem: dict, rng: random.Random) -> list[bool]:
    return [rng.random() < 0.5 for _ in problem["items"]]


def _constructed(data: dict) -> list[bool]:
    model = Model.model_validate(data)
    knapsack.solve(model, time_limit_seconds=0)
    return [item.selected for item in model.items]


def test_the_catalog_maps_one_to_one():
    assert [ref.RENAMED[name] for name in ref.OLD_NAMES] == knapsack.list_constraints()


# ----------------------------------------------------------------------------- samples


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_matches_the_oracle(name):
    problem = _samples()[name]
    rng = random.Random(f"parity:{name}")
    states = [
        [item.get("selected", False) for item in problem["items"]],  # the sample as given
        ref.greedy_selection(problem),  # the 0.1.5 starting point
        [True] * len(problem["items"]),
        [False] * len(problem["items"]),
    ]
    states += [_random_selection(problem, rng) for _ in range(40)]
    for selected in states:
        _assert_parity(_with_selection(problem, selected))


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_constructs_like_the_oracle(name):
    problem = _samples()[name]
    assert _constructed(_with_selection(problem, [False] * len(problem["items"]))) == ref.greedy_selection(problem)
    rng = random.Random(f"construct:{name}")
    for _ in range(10):
        # A partial warm start: some items decided, the rest filled in around the seeded load.
        decided = {item["id"]: rng.random() < 0.5 for item in problem["items"] if rng.random() < 0.5}
        solution = {"items": [{"id": item_id, "selected": chosen} for item_id, chosen in decided.items()]}
        seeded = with_initial_solution(Model.model_validate(problem), solution)
        assert [item.selected for item in seeded.items] == ref.greedy_selection(problem, decided)
        # ... and a zero-budget solve hands the seed back unchanged.
        assert _constructed(seeded.model_dump()) == ref.greedy_selection(problem, decided)


@pytest.mark.parametrize("name", list(_samples()))
def test_a_solved_sample_matches_the_oracle(name):
    model = Model.model_validate(_samples()[name])
    config = knapsack.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = 500
    config.random_seed = 42
    knapsack.solve(model, config=config)
    data = model.model_dump()
    _assert_parity(data)
    assert (model.score_hard, model.score_soft) == ref.calculate_score(data)


def test_undecided_items_score_as_not_selected():
    """The core may hold -1 (undecided); it scores like the oracle with those items left out."""
    problem = _samples()["repeats"]
    rng = random.Random("undecided")
    for _ in range(20):
        values = [rng.choice([-1, 0, 1]) for _ in problem["items"]]
        core = Model.model_validate(problem).to_core()
        core.set_assignment(values)
        totals = {name: sum(entry["costs"]) for name, entry in knapsack.constraint_costs(core).items()}
        data = _with_selection(problem, [value == 1 for value in values])
        assert totals == _ref_totals(data)
        result = knapsack.calculate_score(core)
        assert (result.hard, result.soft) == ref.calculate_score(_dump(data))


# ----------------------------------------------------------------------------- random instances


def _random_instance(rng: random.Random) -> dict:
    """A valid model with every feature (repeats, self-dependencies, undeclared resources, open groups)."""
    n = rng.randint(1, 12)
    ids = [f"i{index}" for index in range(n)]
    resources = {f"r{r}": rng.randint(0, 3 * n) for r in range(rng.randint(0, 3))}
    items = []
    for item_id in ids:
        item = {"id": item_id, "weight": rng.randint(1, 20), "value": rng.randint(0, 30)}
        usage = {name: rng.randint(0, 9) for name in [*resources, "undeclared"] if rng.random() < 0.5}
        if usage:
            item["resource_usage"] = usage
        if rng.random() < 0.3:
            item["requires"] = [rng.choice(ids) for _ in range(rng.randint(1, 3))]
        items.append(item)
    roles = {item_id: rng.choice(["", "", "", "mandatory", "forbidden"]) for item_id in ids}
    mandatory = [item_id for item_id in ids if roles[item_id] == "mandatory" for _ in range(rng.randint(1, 2))]
    forbidden = [item_id for item_id in ids if roles[item_id] == "forbidden" for _ in range(rng.randint(1, 2))]
    rng.shuffle(mandatory)
    rng.shuffle(forbidden)
    groups = []
    for g in range(rng.randint(0, 3)):
        members = rng.sample(ids, rng.randint(1, min(n, 5)))
        group = {"id": f"g{g}", "item_ids": members}
        top = len(members)
        if rng.random() < 0.5:
            group["max_selected"] = top = rng.randint(0, len(members))
        group["min_selected"] = rng.randint(0, top)
        groups.append(group)
    data = {
        "capacity": rng.randint(1, max(1, sum(item["weight"] for item in items))),
        "resource_capacities": resources,
        "mandatory_items": mandatory,
        "forbidden_items": forbidden,
        "selection_groups": groups,
        "items": items,
    }
    Model.model_validate(data)  # the generator only builds valid problems
    return data


SEEDS = range(60)


@pytest.mark.parametrize("seed", SEEDS)
def test_random_instances_match_the_oracle(seed):
    rng = random.Random(seed)
    problem = _random_instance(rng)
    for _ in range(5):
        _assert_parity(_with_selection(problem, _random_selection(problem, rng)))
    unselected = _with_selection(problem, [False] * len(problem["items"]))
    assert _constructed(unselected) == ref.greedy_selection(problem)
    decided = {item["id"]: rng.random() < 0.5 for item in problem["items"] if rng.random() < 0.5}
    seeded = with_initial_solution(
        Model.model_validate(problem),
        {"items": [{"id": item_id, "selected": chosen} for item_id, chosen in decided.items()]},
    )
    assert [item.selected for item in seeded.items] == ref.greedy_selection(problem, decided)


@pytest.mark.parametrize("seed", SEEDS)
def test_scores_match_the_oracle_at_integer_weights(seed):
    """score() re-prices like 0.1.5 score_costs at integer weights and exponent 1, whatever the levels.

    (hard=True on the selected_value_cost reward is left out: 0.1.5 scored it as a positive hard
    term, 0.2.0 refuses it — see test_a_hard_reward_is_refused_now.)"""
    rng = random.Random(1000 + seed)
    problem = _random_instance(rng)
    data = _with_selection(problem, _random_selection(problem, rng))
    raw = knapsack.constraint_costs(Model.model_validate(data).to_core())
    old = ref.constraint_costs(_dump(data))
    by_old = {new: old_name for old_name, new in ref.RENAMED.items()}
    specs = get_constraints()
    rng.shuffle(specs)
    picked: list[Constraint] = []
    for spec in specs[: rng.randint(1, len(specs))]:
        weight = rng.choice([None, 0, 1, 2, 5])
        hard = rng.choice([None, False] if spec.name == "selected_value_cost" else [None, True, False])
        picked.append(type(spec)(weight=weight, hard=hard))
    expected = ref.score_costs(old, [(by_old[spec.name], spec.weight, spec.hard, 1.0) for spec in picked])
    assert tuple(score(raw, picked)) == pytest.approx(expected)
    # ... and the engine scores the same picks the same way.
    core = Model.model_validate(data).to_core()
    zero = knapsack.SolverConfig()
    zero.spent_limit_seconds = 0
    core.warm_start = True
    knapsack.Solver(zero).solve_with_constraints(core, picked)
    assert (core.score.hard, core.score.soft) == pytest.approx(expected)


@pytest.mark.parametrize("seed", range(20))
@pytest.mark.parametrize("exponent", [0.5, 2.0, 3.0])
def test_single_term_constraints_shape_like_the_oracle(seed, exponent):
    """knapsack_capacity_cost and selected_value_cost were one term in 0.1.5 already, so
    any exponent gives the same score."""
    rng = random.Random(3000 + seed)
    problem = _random_instance(rng)
    data = _with_selection(problem, _random_selection(problem, rng))
    raw = knapsack.constraint_costs(Model.model_validate(data).to_core())
    old = ref.constraint_costs(_dump(data))
    picked = [KnapsackCapacityCost(exponent=exponent, hard=False), SelectedValueCost(exponent=exponent, weight=2)]
    expected = ref.score_costs(old, [("capacity", None, False, exponent), ("maximize_value", 2, None, exponent)])
    assert tuple(score(raw, picked)) == pytest.approx(expected)


# ----------------------------------------------------------------------------- documented 0.2.0 changes


def test_the_exponent_now_shapes_each_total():
    """0.1.5 shaped every term ([1, 1, 1] -> 3 at any exponent); 0.2.0 shapes the total (3 -> 9)."""
    data = _with_selection(
        {
            "capacity": 50,
            "mandatory_items": ["A", "B", "C"],
            "items": [{"id": i, "weight": 1, "value": 1} for i in "ABC"],
        },
        [False, False, False],
    )
    old = ref.constraint_costs(_dump(data))
    assert old["mandatory_items"] == [1, 1, 1]
    assert ref.score_costs(old, [("mandatory_items", None, None, 2.0)]) == (-3, 0.0)
    raw = knapsack.constraint_costs(Model.model_validate(data).to_core())
    assert raw["mandatory_items_cost"]["costs"] == [3.0]
    assert score(raw, [MandatoryItemsCost(exponent=2.0)]) == (-9, 0.0)


def test_fractional_hard_weights_now_round_per_constraint():
    """0.1.5 rounded the hard total once (0.5 + 0.5 -> -1); the engine rounds each constraint (-2)."""
    data = _samples()["extended"]  # A selected: mandatory 1, forbidden 1
    old = ref.constraint_costs(_dump(data))
    assert ref.score_costs(old, [("mandatory_items", 0.5, None, 1.0), ("forbidden_items", 0.5, None, 1.0)]) == (-1, 0.0)
    raw = knapsack.constraint_costs(Model.model_validate(data).to_core())
    assert score(raw, [MandatoryItemsCost(weight=0.5), ForbiddenItemsCost(weight=0.5)]) == (-2, 0.0)


def test_a_hard_reward_is_refused_now():
    """0.1.5 scored maximize_value with hard=True as a positive hard score; 0.2.0 raises."""
    data = _samples()["extended"]
    old = ref.constraint_costs(_dump(data))
    assert ref.score_costs(old, [("maximize_value", None, True, 1.0)]) == (5, 0.0)
    raw = knapsack.constraint_costs(Model.model_validate(data).to_core())
    with pytest.raises(ValueError, match="is a reward"):
        score(raw, [SelectedValueCost(hard=True)])
