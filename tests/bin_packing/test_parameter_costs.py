"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.bin_packing as bp
from optimaru.bin_packing.constraints import ColorConflictCost, ItemConflictCost, get_constraints
from optimaru.bin_packing.models import BinPackingProblemModel, parameter_uses

from .._picks import replace_default, without


def problem(**fields) -> BinPackingProblemModel:
    data = {
        "bin_capacity": 10,
        "items": [{"id": "a", "weight": 4, "color": "red"}, {"id": "b", "weight": 5}, {"id": "c", "weight": 3}],
        "conflicts": [{"first": "a", "second": "b"}],
    } | fields
    return BinPackingProblemModel.model_validate(data)


CHECKED = [("color", "color_conflict_cost", ColorConflictCost), ("conflicts", "item_conflict_cost", ItemConflictCost)]


def test_the_default_selection_accepts_every_parameter():
    assert {use.field for use in parameter_uses(problem())} == {"color", "conflicts"}
    bp.solve(problem(), time_limit_seconds=0)


@pytest.mark.parametrize("field, cost, spec", CHECKED)
def test_a_selection_without_the_cost_is_refused(field, cost, spec):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        bp.solve(problem(), time_limit_seconds=0, constraints=without(get_constraints(), cost))


@pytest.mark.parametrize("field, cost, spec", CHECKED)
def test_the_cost_at_weight_zero_counts(field, cost, spec):
    bp.solve(problem(), time_limit_seconds=0, constraints=replace_default(get_constraints(), cost, spec(weight=0)))


def test_without_the_parameters_no_cost_is_needed():
    bare = problem(items=[{"id": "a", "weight": 4}, {"id": "b", "weight": 5}], conflicts=[])
    assert parameter_uses(bare) == []
    bp.solve(
        bare,
        time_limit_seconds=0,
        constraints=without(without(get_constraints(), "color_conflict_cost"), "item_conflict_cost"),
    )
