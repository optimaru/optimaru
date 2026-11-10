"""optimaru.knapsack.models — the canonical JSON contract and its conversion to the engine object."""

import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from optimaru import knapsack
from optimaru.knapsack.models import ItemModel, KnapsackProblemModel, SelectionGroupModel

FIXTURES = Path(__file__).parent / "fixtures"


def rich() -> KnapsackProblemModel:
    """Every fact, with repeats, a dict order that is not sorted and a group without a maximum."""
    return KnapsackProblemModel(
        capacity=20,
        resource_capacities={"zeta": 3, "alpha": 1},
        mandatory_items=["C", "C"],
        forbidden_items=["D"],
        selection_groups=[
            SelectionGroupModel(id="g-max", item_ids=["A", "B"], max_selected=1),
            SelectionGroupModel(id="g-open", item_ids=["B", "C", "D"], min_selected=1),
        ],
        items=[
            ItemModel(id="A", weight=3, value=5, resource_usage={"zeta": 2, "alpha": 1}, requires=["B", "B"]),
            ItemModel(id="B", weight=2, value=4, resource_usage={"undeclared": 9}, selected=True),
            ItemModel(id="C", weight=1, value=1, requires=["C"]),
            ItemModel(id="D", weight=7, value=9, selected=True),
        ],
    )


def test_to_core_maps_ids_to_indices_and_keeps_order_and_repeats():
    core = rich().to_core()
    assert isinstance(core, knapsack.KnapsackProblem)
    assert core.capacity == 20
    assert [(resource.id, resource.capacity) for resource in core.resources] == [("zeta", 3), ("alpha", 1)]
    assert core.mandatory_items == [2, 2]
    assert core.forbidden_items == [3]
    assert [(group.id, group.items, group.min_selected, group.max_selected) for group in core.selection_groups] == [
        ("g-max", [0, 1], 0, 1),
        ("g-open", [1, 2, 3], 1, None),
    ]
    assert [item.required_items for item in core.items] == [[1, 1], [], [2], []]
    assert core.items[0].resource_usage == {"zeta": 2, "alpha": 1}
    assert core.assignment() == [0, 1, 0, 1]
    assert core.warm_start is False


def test_from_core_round_trips_the_facts_and_fills_the_results():
    model = rich()
    core = model.to_core()
    knapsack.calculate_score(core)
    back = KnapsackProblemModel.from_core(core)
    expected = model.model_dump()
    expected.update(total_weight=9, total_value=13, score_hard=core.score.hard, score_soft=core.score.soft)
    assert back.model_dump() == expected
    assert list(back.resource_capacities) == ["zeta", "alpha"]  # declaration order survives
    assert list(back.items[0].resource_usage) == ["alpha", "zeta"]  # an item's usage is a map in the engine: sorted


def test_from_core_of_a_solved_core_fills_the_totals():
    core = rich().to_core()
    knapsack.solve(core, time_limit_seconds=0.2, random_seed=1)
    back = KnapsackProblemModel.from_core(core)
    selected = [item for item in back.items if item.selected]
    assert back.total_weight == sum(item.weight for item in selected) == core.total_weight()
    assert back.total_value == sum(item.value for item in selected) == core.total_value()
    assert (back.score_hard, back.score_soft) == (core.score.hard, core.score.soft)
    assert back.warm_start is False


def test_the_web_demo_validates_and_converts():
    data = json.loads((FIXTURES / "web_demo.json").read_text())
    model = KnapsackProblemModel.model_validate(data)
    core = model.to_core()
    assert core.item_count() == 12
    assert [resource.id for resource in core.resources] == ["fragile_slots"]
    assert KnapsackProblemModel.from_core(core).model_dump(exclude={"total_weight", "total_value"}) == {
        **model.model_dump(exclude={"total_weight", "total_value"}),
        "score_hard": 0,
        "score_soft": 0.0,
    }


def test_solving_the_model_writes_the_result_back_in_place():
    model = rich()
    model.warm_start = True
    result = knapsack.solve(model, time_limit_seconds=0.2, random_seed=3)
    assert (model.score_hard, model.score_soft) == (result.hard, result.soft)
    assert model.warm_start is False
    assert model.total_weight == sum(item.weight for item in model.items if item.selected)
    assert list(model.items[0].resource_usage) == ["zeta", "alpha"]  # the facts are not rewritten


@pytest.mark.parametrize(
    "change, message",
    [
        (lambda d: d["items"].append({"id": "A", "weight": 1, "value": 1}), "unique"),
        (lambda d: d["mandatory_items"].append("Z"), "unknown item references"),
        (lambda d: d["items"][0]["requires"].append("Z"), "unknown item references"),
        (lambda d: d["forbidden_items"].append("C"), "both mandatory and forbidden"),
        (lambda d: d["resource_capacities"].update(alpha=-1), "non-negative"),
        (lambda d: d["items"][0]["resource_usage"].update(zeta=-1), "non-negative"),
        (lambda d: d["items"][0].update(weight=0), "greater than 0"),
        (lambda d: d["items"][0].update(value=-1), "greater than or equal to 0"),
        (lambda d: d.update(capacity=0), "greater than 0"),
        (lambda d: d["selection_groups"][0].update(item_ids=["A", "A"]), "unique"),
        (lambda d: d["selection_groups"][0].update(min_selected=2, max_selected=1), "bounds"),
        (lambda d: d["selection_groups"][0].update(max_selected=3), "bounds"),
        (lambda d: d.update(capacity_weight=1), "Extra inputs"),
    ],
)
def test_validators_are_unchanged(change, message):
    data = rich().model_dump()
    change(data)
    with pytest.raises(ValidationError, match=message):
        KnapsackProblemModel.model_validate(data)


def test_the_core_validates_what_a_direct_caller_could_get_wrong():
    """A core built by hand is checked by the C++ prepare(), as ValueError."""
    core = rich().to_core()
    core.mandatory_items = [7]
    with pytest.raises(ValueError, match="out of range"):
        knapsack.calculate_score(core)
    core = rich().to_core()
    core.forbidden_items = [2]
    with pytest.raises(ValueError, match="both mandatory and forbidden"):
        knapsack.constraint_costs(core)
    core = rich().to_core()
    with pytest.raises(ValueError, match="-1, 0 or 1"):
        core.set_assignment([0, 1, 2, 0])
    with pytest.raises(ValueError, match="length"):
        core.set_assignment([0, 1])


def test_greedy_selection_matches_the_documented_cases():
    core = KnapsackProblemModel(
        capacity=10,
        items=[
            ItemModel(id="A", weight=6, value=9),
            ItemModel(id="B", weight=4, value=7),
            ItemModel(id="C", weight=5, value=8),
        ],
    ).to_core()
    assert knapsack._core.greedy_selection(core, [-1, -1, -1]) == [False, True, True]
    assert knapsack._core.greedy_selection(core, [1, -1, -1]) == [True, True, False]
    assert knapsack._core.greedy_selection(core, [-1, 0, -1]) == [False, False, True]
    with pytest.raises(ValueError):
        knapsack._core.greedy_selection(core, [-1, -1])
