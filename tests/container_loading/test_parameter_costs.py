"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.container_loading as cl
from optimaru.container_loading.constraints import (
    BalanceLimitCost,
    ContainerWeightCost,
    GroupSplitCost,
    StackingLoadCost,
    StackingRuleCost,
    UnloadingOrderCost,
    get_constraints,
)
from optimaru.container_loading.models import ContainerLoadingProblemModel, parameter_uses

from .._picks import replace_default, without


def problem(**fields) -> ContainerLoadingProblemModel:
    data = {
        "containers": [
            {"id": "c", "length": 10, "width": 10, "height": 10, "max_weight": 50},
            {"id": "d", "length": 10, "width": 10, "height": 10, "max_offset_length": 0.5, "max_offset_width": 0.5},
        ],
        "items": [
            {"id": "a", "length": 5, "width": 5, "height": 5, "max_load_on_top": 3},
            {"id": "b", "length": 5, "width": 5, "height": 5, "unload_order": 1},
            {"id": "c", "length": 5, "width": 5, "height": 5, "group": "order"},
            {"id": "d", "length": 5, "width": 5, "height": 5, "max_stack_layers": 2, "stack_group": "food"},
        ],
        "stacking_rules": [{"top": "food", "bottom": "food"}],
    } | fields
    return ContainerLoadingProblemModel.model_validate(data)


CHECKED = [
    ("max_weight", "container_weight_cost", ContainerWeightCost),
    ("max_load_on_top", "stacking_load_cost", StackingLoadCost),
    ("unload_order", "unloading_order_cost", UnloadingOrderCost),
    ("group", "group_split_cost", GroupSplitCost),
    ("max_offset_length", "balance_limit_cost", BalanceLimitCost),
    ("max_offset_width", "balance_limit_cost", BalanceLimitCost),
    ("max_stack_layers", "stacking_rule_cost", StackingRuleCost),
    ("stack_group", "stacking_rule_cost", StackingRuleCost),
    ("stacking_rules", "stacking_rule_cost", StackingRuleCost),
]


def test_the_default_selection_accepts_every_parameter():
    uses = parameter_uses(problem())
    assert {(use.field, use.costs) for use in uses} == {(field, (cost,)) for field, cost, _ in CHECKED}
    assert {use.where for use in uses} == {
        "container c",
        "container d",
        "item a",
        "item b",
        "item c",
        "item d",
        "the problem",
    }
    cl.solve(problem(), time_limit_seconds=0)


@pytest.mark.parametrize("field, cost, spec", CHECKED)
def test_a_selection_without_the_cost_is_refused(field, cost, spec):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        cl.solve(problem(), time_limit_seconds=0, constraints=without(get_constraints(), cost))


@pytest.mark.parametrize("field, cost, spec", CHECKED)
def test_the_cost_at_weight_zero_counts(field, cost, spec):
    cl.solve(problem(), time_limit_seconds=0, constraints=replace_default(get_constraints(), cost, spec(weight=0)))


def test_without_the_parameters_no_cost_is_needed():
    bare = problem(
        containers=[{"id": "c", "length": 10, "width": 10, "height": 10, "cost": 4.0, "max_stack_height": 9}],
        items=[
            {"id": "a", "length": 5, "width": 5, "height": 5, "weight": 7, "rotations": "upright", "bottom_only": True}
        ],
        stacking_rules=[],
    )
    assert parameter_uses(bare) == []
    picks = get_constraints()
    for cost in {cost for _, cost, _ in CHECKED}:
        picks = without(picks, cost)
    solved = cl.solve(bare, time_limit_seconds=0, constraints=picks)
    assert (solved.score_hard, solved.score_soft) == (0, -4.0)


def test_a_compiled_problem_is_solved_as_it_is():
    """The check is the model's: an engine object carries no record of what was set on purpose."""
    core = problem().to_core()
    cl.solve(core, time_limit_seconds=0, constraints=without(get_constraints(), "group_split_cost"))
