"""A parameter that acts only through a cost needs that cost among the constraints."""

import pytest

import optimaru.knapsack as knapsack
from optimaru.knapsack.constraints import (
    ForbiddenItemsCost,
    ItemDependencyCost,
    KnapsackCapacityCost,
    MandatoryItemsCost,
    ResourceCapacityCost,
    SelectedValueCost,
    SelectionGroupBoundsCost,
    get_constraints,
)
from optimaru.knapsack.models import (
    ItemModel,
    KnapsackProblemModel,
    KnapsackRequest,
    SelectionGroupModel,
    parameter_uses,
    solve_request,
)

PLAIN = [KnapsackCapacityCost(), SelectedValueCost()]


def problem(**fields) -> KnapsackProblemModel:
    fields = dict(fields)
    items = [
        ItemModel(id=f"i{k}", weight=k + 1, value=10 - k, resource_usage=fields.pop(f"usage{k}", {})) for k in range(4)
    ]
    if "requires" in fields:
        items[0].requires = fields.pop("requires")
    return KnapsackProblemModel(capacity=5, items=items, **fields)


def solve(model, constraints=None):
    return knapsack.solve(model, time_limit_seconds=0.05, random_seed=1, constraints=constraints)


EVERYTHING = {
    "resource_capacities": {"volume": 3},
    "usage1": {"volume": 2},
    "mandatory_items": ["i0"],
    "forbidden_items": ["i3"],
    "selection_groups": [SelectionGroupModel(id="g", item_ids=["i1", "i2"], max_selected=1)],
    "requires": ["i1"],
}


def test_the_default_constraints_take_every_parameter():
    model = problem(**dict(EVERYTHING))
    assert {use.field for use in parameter_uses(model)} == {
        "resource_capacities",
        "resource_usage",
        "mandatory_items",
        "forbidden_items",
        "requires",
        "min_selected / max_selected",
    }
    solve(model)
    solve_request(KnapsackRequest(model_input=problem(**dict(EVERYTHING)), time_limit_seconds=0.05))


@pytest.mark.parametrize(
    ("fields", "field", "cost"),
    [
        ({"resource_capacities": {"volume": 3}}, "resource_capacities", "resource_capacity_cost"),
        ({"usage2": {"volume": 1}}, "resource_usage", "resource_capacity_cost"),
        ({"mandatory_items": ["i0"]}, "mandatory_items", "mandatory_items_cost"),
        ({"forbidden_items": ["i0"]}, "forbidden_items", "forbidden_items_cost"),
        ({"requires": ["i1"]}, "requires", "item_dependency_cost"),
        (
            {"selection_groups": [SelectionGroupModel(id="g", item_ids=["i1", "i2"], min_selected=1)]},
            "min_selected / max_selected",
            "selection_group_bounds_cost",
        ),
    ],
)
def test_a_selection_without_the_cost_is_refused(fields, field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        solve(problem(**fields), PLAIN)
    with pytest.raises(ValueError, match=cost):
        solve_request(KnapsackRequest(model_input=problem(**fields), time_limit_seconds=0.05), constraints=PLAIN)


def test_a_pick_at_weight_zero_counts():
    picks = [
        *PLAIN,
        ResourceCapacityCost(weight=0),
        MandatoryItemsCost(weight=0),
        ForbiddenItemsCost(weight=0),
        ItemDependencyCost(weight=0),
        SelectionGroupBoundsCost(weight=0),
    ]
    solve(problem(**dict(EVERYTHING)), picks)


def test_parameters_left_unset_need_no_cost():
    # A group with neither bound asks nothing.
    model = problem(selection_groups=[SelectionGroupModel(id="g", item_ids=["i1", "i2"])])
    assert parameter_uses(model) == []
    solve(model, PLAIN)
    solve(model, iter(PLAIN))  # a generator is read once: the check must not use it up


def test_an_engine_object_is_not_checked():
    core = problem(mandatory_items=["i0"]).to_core()
    solve(core, PLAIN)
    assert get_constraints() is not None
