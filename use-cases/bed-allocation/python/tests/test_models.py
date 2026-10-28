"""pydantic 모델 직렬화/역직렬화와 엔진 변환 왕복 테스트."""

import pytest
from pydantic import ValidationError

import bed_allocation as ba
from bed_allocation.demo import generate_demo_data
from bed_allocation.models import BedModel, BedPlanModel, DepartmentModel, RoomModel, StayModel


def test_json_round_trip_preserves_model():
    model = generate_demo_data()
    clone = BedPlanModel.model_validate_json(model.model_dump_json())
    assert clone == model


def test_to_core_from_core_round_trip():
    model = generate_demo_data()
    plan = model.to_core()
    assert plan.bed_count() == len(model.beds)
    assert [s.id for s in plan.stays] == [s.id for s in model.stays]

    ba.calculate_score(plan)
    back = BedPlanModel.from_core(plan)
    # 점수 필드 외에는 원본과 같아야 한다 (배정 전이라 bed_id 전부 None).
    assert back.model_dump(exclude={"score_hard", "score_medium", "score_soft"}) == model.model_dump(
        exclude={"score_hard", "score_medium", "score_soft"}
    )
    assert back.score_hard == 0
    assert back.score_medium == 0  # bed_id None == 미초기화(-1): 기여 없음
    assert back.score_soft == 0


def test_from_core_maps_bed_values_to_ids():
    model = generate_demo_data()
    plan = model.to_core()
    assignment = [i % plan.bed_count() for i in range(len(model.stays))]
    assignment[0] = plan.no_bed_value()  # "no bed" 센티널 -> bed_id None
    plan.set_assignment(assignment)
    ba.calculate_score(plan)

    back = BedPlanModel.from_core(plan)
    assert back.stays[0].bed_id is None
    assert back.stays[1].bed_id == model.beds[assignment[1]].id
    assert back.score_medium == -plan.stays[0].night_count()


def test_night_count_uses_dates():
    model = generate_demo_data()
    plan = model.to_core()
    for stay_model, stay in zip(model.stays, plan.stays):
        nights = (stay_model.departure_date - stay_model.arrival_date).days + 1
        assert stay.night_count() == nights


def test_reference_validation():
    department = DepartmentModel(id="d0", name="Dept")
    room = RoomModel(id="r0", department_id="d0", capacity=1)
    bed = BedModel(id="b0", room_id="r0")

    with pytest.raises(ValidationError, match="room_id"):
        BedPlanModel(
            departments=[department],
            rooms=[room],
            beds=[BedModel(id="b0", room_id="missing")],
            stays=[],
        )
    with pytest.raises(ValidationError, match="bed_id"):
        BedPlanModel(
            departments=[department],
            rooms=[room],
            beds=[bed],
            stays=[
                StayModel(
                    id="s0",
                    patient_age=30,
                    arrival_date="2026-08-03",
                    departure_date="2026-08-05",
                    bed_id="missing",
                )
            ],
        )
