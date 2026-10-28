"""Serialization/deserialization of the pydantic mirror and engine object round trips."""

from datetime import date, datetime, timedelta

import food_packaging as fp
from food_packaging.models import PackagingScheduleModel, datetime_to_minute, minute_to_datetime

MONDAY = datetime(2021, 2, 1)  # reproducible reference time


def sample_model() -> PackagingScheduleModel:
    return PackagingScheduleModel(
        from_date=date(2021, 2, 1),
        to_date=date(2021, 2, 15),
        products=[
            {
                "id": "A",
                "name": "Carrots small bag",
                "cleaning_durations": {"A": timedelta(0), "B": timedelta(minutes=60)},
            },
            {
                "id": "B",
                "name": "Tomato large bag",
                "cleaning_durations": {"A": timedelta(minutes=40), "B": timedelta(0)},
            },
        ],
        operators=[{"id": "Operator 1"}, {"id": "Operator 2"}],
        lines=[
            {
                "id": "0",
                "name": "Line 1",
                "start_date_time": MONDAY,
                "operator_id": "Operator 1",
                "job_ids": ["j0", "j1"],
            },
            {"id": "1", "name": "Line 2", "start_date_time": MONDAY, "operator_id": None, "job_ids": []},
        ],
        jobs=[
            {
                "id": "j0",
                "name": "Tomato large bag",
                "product_id": "B",
                "duration": timedelta(minutes=100),
                "min_start_time": MONDAY,
                "ideal_end_time": MONDAY + timedelta(minutes=100),
                "max_end_time": MONDAY + timedelta(days=2),
            },
            {
                "id": "j1",
                "name": "Carrots small bag",
                "product_id": "A",
                "duration": timedelta(minutes=200),
                "min_start_time": MONDAY,
                "ideal_end_time": MONDAY + timedelta(days=1),
                "max_end_time": MONDAY + timedelta(days=2),
                "pinned": False,
            },
            {
                "id": "j2",
                "name": "unassigned job",
                "product_id": "A",
                "duration": timedelta(minutes=50),
                "min_start_time": MONDAY,
                "ideal_end_time": MONDAY + timedelta(days=1),
                "max_end_time": MONDAY + timedelta(days=2),
            },
        ],
    )


def test_minute_datetime_roundtrip():
    assert datetime_to_minute(MONDAY) == 18659 * 1440  # 2021-02-01 in epoch minutes
    assert minute_to_datetime(datetime_to_minute(MONDAY)) == MONDAY


def test_to_core_and_score():
    schedule = sample_model().to_core()
    score = fp.calculate_score(schedule)
    # Chain: j0 runs 0..100 minutes (from the line start), j1 after 60 minutes of cleaning
    # (B->A) runs 160..360. j0 hits ideal 100 exactly, and j2 (50 minutes) is unassigned.
    base = datetime_to_minute(MONDAY)
    jobs = schedule.jobs
    assert jobs[0].end_minute == base + 100
    assert jobs[1].start_production_minute == base + 160
    assert jobs[1].end_minute == base + 360
    assert score == fp.Score(0, -50, -(360 * 360))


def test_json_roundtrip_preserves_score_and_assignment():
    model = sample_model()
    schedule = model.to_core()
    fp.calculate_score(schedule)

    exported = PackagingScheduleModel.from_core(schedule)
    assert exported.score_hard == 0
    assert exported.score_medium == -50
    assert exported.score_soft == -(360 * 360)
    assert exported.jobs[0].line_id == "0"
    assert exported.jobs[2].line_id is None
    assert exported.jobs[1].end_date_time == MONDAY + timedelta(minutes=360)

    clone = PackagingScheduleModel.model_validate_json(exported.model_dump_json())
    assert clone == exported

    # Sending it back to the engine must give the same score.
    schedule2 = clone.to_core()
    assert fp.calculate_score(schedule2) == fp.Score(0, -50, -(360 * 360))


def test_from_core_on_demo():
    schedule = fp.generate_demo_data()
    fp.calculate_score(schedule)
    model = PackagingScheduleModel.from_core(schedule)
    assert len(model.products) == 60
    assert len(model.jobs) == 100
    assert len(model.lines) == 5
    # The demo starts fully unassigned.
    assert all(job.line_id is None for job in model.jobs)
    assert (model.to_date - model.from_date).days == 14
