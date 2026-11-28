"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.box_sizing as bs
from optimaru.box_sizing.constraints import (
    BoxCost,
    BoxTypeCost,
    BoxTypeLimitCost,
    BoxWeightCost,
    ShippingCost,
    get_constraints,
)
from optimaru.box_sizing.models import BoxSizingProblemModel, parameter_uses

from .._picks import replace_default, without
from .conftest import nested_data


def problem(**box) -> BoxSizingProblemModel:
    data = nested_data(max_box_types=3)
    data["boxes"][2] |= {"max_weight": 1000, "cost": 2.0, "required": True, "dim_divisor": 5000} | box
    data["parcels"][0]["zone"] = "near"
    data["rate_tables"] = [
        {"breaks": [{"max_weight": 1000, "price": 3.0}]},
        {"zone": "near", "breaks": [{"max_weight": 1000, "price": 2.0}]},
    ]
    return BoxSizingProblemModel.model_validate(data)


CHECKED = [
    ("max_weight", "box_weight_cost", BoxWeightCost),
    ("cost", "box_cost", BoxCost),
    ("max_box_types", "box_type_limit_cost", BoxTypeLimitCost),
    ("dim_divisor", "shipping_cost", ShippingCost),
    ("zone", "shipping_cost", ShippingCost),
    ("rate_tables", "shipping_cost", ShippingCost),
]


def test_the_default_selection_accepts_every_parameter():
    assert {use.field for use in parameter_uses(problem())} == {
        "max_weight",
        "cost",
        "required",
        "max_box_types",
        "dim_divisor",
        "zone",
        "rate_tables",
    }
    bs.solve(problem(), time_limit_seconds=0)


@pytest.mark.parametrize("field, cost, spec", CHECKED)
def test_a_selection_without_the_cost_is_refused(field, cost, spec):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        bs.solve(problem(), time_limit_seconds=0, constraints=without(get_constraints(), cost))


@pytest.mark.parametrize("field, cost, spec", CHECKED)
def test_the_cost_at_weight_zero_counts(field, cost, spec):
    bs.solve(problem(), time_limit_seconds=0, constraints=replace_default(get_constraints(), cost, spec(weight=0)))


def test_required_needs_one_of_the_two_costs_that_count_the_boxes():
    neither = without(without(get_constraints(), "box_type_limit_cost"), "box_type_cost")
    data = nested_data()  # no max_box_types: only `required` reads the two
    data["boxes"][2]["required"] = True
    model = BoxSizingProblemModel.model_validate(data)
    with pytest.raises(
        ValueError, match="box L sets required, but the constraints leave out box_type_limit_cost or box_type_cost"
    ):
        bs.solve(model, time_limit_seconds=0, constraints=neither)
    bs.solve(model, time_limit_seconds=0, constraints=[*neither, BoxTypeCost()])
    bs.solve(model, time_limit_seconds=0, constraints=[*neither, BoxTypeLimitCost()])


def test_without_the_parameters_no_cost_is_needed():
    bare = BoxSizingProblemModel.model_validate(nested_data())
    assert parameter_uses(bare) == []
    bs.solve(
        bare, time_limit_seconds=0, constraints=[spec for spec in get_constraints() if spec.name == "void_volume_cost"]
    )


def test_a_compiled_problem_is_solved_as_it_is():
    core = problem().to_core()
    bs.solve(core, time_limit_seconds=0, constraints=without(get_constraints(), "box_cost"))
