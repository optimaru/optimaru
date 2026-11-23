"""A parameter that acts only through a cost needs that cost in the selection."""

import pytest

import optimaru.semiconductor_scheduling as fab
from optimaru.semiconductor_scheduling.constraints import MinBatchSizeCost, get_constraints
from optimaru.semiconductor_scheduling.models import parameter_uses

from .test_semiconductor_scheduling import _fab_problem

QUICK = {"time_limit_seconds": 0.05, "random_seed": 1}


def problem():
    model = _fab_problem(with_assignments=False)
    lots = [lot.model_copy(update={"max_cycle_time": 40}) if lot.id == "L2" else lot for lot in model.lots]
    return model.model_copy(update={"lots": lots})


def without(name):
    return [spec for spec in get_constraints() if spec.name != name]


def test_the_default_selection_takes_every_parameter():
    assert {use.field for use in parameter_uses(problem())} == {
        "minimum_batch_size",
        "max_cycle_time",
        "max_queue_time",
    }
    fab.solve(problem(), **QUICK)


@pytest.mark.parametrize(
    "field, cost",
    [
        ("minimum_batch_size", "min_batch_size_cost"),
        ("max_cycle_time", "max_cycle_time_cost"),
        ("max_queue_time", "max_queue_time_cost"),
    ],
)
def test_a_selection_without_the_cost_is_refused(field, cost):
    with pytest.raises(ValueError, match=f"sets {field}, but the constraints leave out {cost}"):
        fab.solve(problem(), constraints=without(cost), **QUICK)


def test_what_the_decoder_enforces_is_not_checked():
    # Downtime, setups and transport are built into every schedule: no cost needed.
    picks = [
        spec
        for spec in get_constraints()
        if spec.name not in ("machine_unavailability_cost", "missing_setup_time_cost", "missing_transport_time_cost")
    ]
    fab.solve(problem(), constraints=picks, **QUICK)


def test_a_weight_zero_pick_counts():
    fab.solve(problem(), constraints=[*without("min_batch_size_cost"), MinBatchSizeCost(weight=0)], **QUICK)


def test_a_missing_cost_is_fine_when_its_parameter_is_unset():
    model = problem()
    machines = [m.model_copy(update={"minimum_batch_size": 1}) for m in model.machines]
    fab.solve(model.model_copy(update={"machines": machines}), constraints=without("min_batch_size_cost"), **QUICK)


def test_priority_and_preference_cost_need_no_cost():
    # The decoder dispatches by priority and picks the preferred machine first, whatever the costs.
    picks = [
        spec for spec in get_constraints() if spec.name not in ("weighted_tardiness_cost", "machine_preference_cost")
    ]
    fab.solve(problem(), constraints=picks, **QUICK)
