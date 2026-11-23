"""A job parameter that only a cost reads is refused when the constraints leave that cost out."""

from datetime import date

import pytest

import optimaru.maintenance_scheduling as mnt
from optimaru.maintenance_scheduling.constraints import (
    AfterIdealEndDateCost,
    CrewConflictCost,
    MaxEndDateCost,
    MinStartDateCost,
    TagConflictCost,
    get_constraints,
)
from optimaru.maintenance_scheduling.models import (
    CrewModel,
    JobModel,
    MaintenanceScheduleModel,
    MaintenanceScheduleRequest,
    parameter_uses,
    solve_request,
)


def _schedule(**job_fields) -> MaintenanceScheduleModel:
    return MaintenanceScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 2, 2),
        crews=[CrewModel(id="c1", name="Crew 1")],
        jobs=[
            JobModel(id="j1", name="Job 1", duration_in_days=2, **job_fields),
            JobModel(id="j2", name="Job 2", duration_in_days=1),
        ],
    )


EVERY_PARAMETER = {
    "min_start_date": date(2026, 1, 7),
    "max_end_date": date(2026, 1, 30),
    "ideal_end_date": date(2026, 1, 20),
    "tags": ["paint"],
}


def test_parameter_uses_lists_what_a_job_sets_away_from_its_default():
    assert parameter_uses(_schedule()) == []
    assert [use.field for use in parameter_uses(_schedule(**EVERY_PARAMETER))] == list(EVERY_PARAMETER)


def test_the_default_constraints_take_every_parameter():
    solved = mnt.solve(_schedule(**EVERY_PARAMETER), time_limit_seconds=0, random_seed=1)
    assert solved.score_hard is not None


@pytest.mark.parametrize(
    ("field", "cost"),
    [
        ("min_start_date", "min_start_date_cost"),
        ("max_end_date", "max_end_date_cost"),
        ("ideal_end_date", "before_ideal_end_date_cost or after_ideal_end_date_cost"),
        ("tags", "tag_conflict_cost"),
    ],
)
def test_a_selection_without_the_cost_is_refused(field, cost):
    schedule = _schedule(**{field: EVERY_PARAMETER[field]})
    with pytest.raises(ValueError, match=f"job j1 sets {field}, but the constraints leave out {cost}"):
        mnt.solve(schedule, time_limit_seconds=0, constraints=[CrewConflictCost()])
    request = MaintenanceScheduleRequest(model_input=schedule, time_limit_seconds=0)
    with pytest.raises(ValueError, match=f"sets {field}"):
        solve_request(request, constraints=[CrewConflictCost()])


def test_one_of_several_costs_is_enough_and_a_weight_of_zero_counts():
    schedule = _schedule(**EVERY_PARAMETER)
    picks = [
        CrewConflictCost(),
        MinStartDateCost(weight=0),
        MaxEndDateCost(),
        AfterIdealEndDateCost(),
        TagConflictCost(),
    ]
    mnt.solve(schedule, time_limit_seconds=0, constraints=picks)


def test_a_parameter_left_at_its_default_needs_no_cost():
    mnt.solve(_schedule(), time_limit_seconds=0, constraints=[CrewConflictCost()])
    assert len(get_constraints()) == 6


def test_a_compiled_schedule_is_not_checked():
    core = _schedule(**EVERY_PARAMETER).to_core()
    mnt.solve(core, time_limit_seconds=0, constraints=[CrewConflictCost()])
