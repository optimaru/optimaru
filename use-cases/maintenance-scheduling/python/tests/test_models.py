"""pydantic mirror checks: serialization round trip and engine <-> model conversion."""

from datetime import date

import pytest

import maintenance_scheduling as mnt
from maintenance_scheduling.models import (
    CrewModel,
    JobModel,
    MaintenanceScheduleModel,
    date_to_epoch,
    epoch_to_date,
)

MONDAY = date(2021, 2, 1)  # epoch day 18659


def test_epoch_conversion():
    assert date_to_epoch(date(1970, 1, 1)) == 0
    assert date_to_epoch(MONDAY) == 18659
    assert epoch_to_date(18659) == MONDAY


def make_model() -> MaintenanceScheduleModel:
    return MaintenanceScheduleModel(
        from_date=MONDAY,
        to_date=date(2021, 3, 29),
        crews=[CrewModel(id="1", name="Alpha crew"), CrewModel(id="2", name="Beta crew")],
        jobs=[
            JobModel(
                id="1",
                name="Downtown tunnel",
                duration_in_days=3,
                min_start_date=MONDAY,
                max_end_date=date(2021, 3, 1),
                ideal_end_date=date(2021, 2, 15),
                tags=["Downtown"],
                crew_id="1",
                start_date=date(2021, 2, 2),
            ),
            JobModel(id="2", name="Uptown bridge", duration_in_days=2, pinned=True),
        ],
    )


def test_json_roundtrip():
    model = make_model()
    clone = MaintenanceScheduleModel.model_validate_json(model.model_dump_json())
    assert clone == model


def test_to_core_and_back():
    model = make_model()
    schedule = model.to_core()

    assert schedule.crew_count() == 2
    assert schedule.from_date == 18659
    # job 1 is assigned to (2021-02-02, crew "1").
    assert schedule.job_start_date(0) == 18660
    assert schedule.crew_index_of(schedule.jobs[0].crew_date) == 0
    assert schedule.jobs[0].crew_date >= 0
    assert schedule.jobs[1].crew_date == -1
    assert schedule.jobs[1].pinned

    mnt.calculate_score(schedule)
    back = MaintenanceScheduleModel.from_core(schedule)
    assert back.from_date == model.from_date
    assert back.to_date == model.to_date
    assert back.crews == model.crews
    job = back.jobs[0]
    assert job.crew_id == "1"
    assert job.start_date == date(2021, 2, 2)
    # end_date shadow: Tue + 3 business days = Fri (exclusive).
    assert job.end_date == date(2021, 2, 5)
    assert back.jobs[1].crew_id is None
    assert back.jobs[1].pinned
    assert back.score_hard == schedule.score.hard
    assert back.score_soft == schedule.score.soft


def test_to_core_rejects_bad_assignment():
    model = make_model()
    model.jobs[0].crew_id = "nope"
    with pytest.raises(ValueError, match="unknown crew_id"):
        model.to_core()

    model = make_model()
    model.jobs[0].start_date = date(2021, 2, 6)  # Saturday
    with pytest.raises(ValueError, match="not a weekday"):
        model.to_core()


def test_solve_via_models():
    schedule = mnt.generate_demo_data("SMALL", 18659)
    model = MaintenanceScheduleModel.from_core(schedule)
    rebuilt = model.to_core()
    mnt.solve(rebuilt, time_limit_seconds=10.0, random_seed=3, stop_at_first_feasible=True)
    solved = MaintenanceScheduleModel.from_core(rebuilt)
    assert solved.score_hard == 0
    assert all(j.crew_id is not None and j.start_date is not None for j in solved.jobs)
