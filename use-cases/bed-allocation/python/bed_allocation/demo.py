"""Java quickstart `DemoDataGenerator` 의 Python 재현.

같은 규모/분포의 데모 인스턴스를 만든다: 부서 1개(전문과목 3개), SAME_GENDER
병실 10개(정원 8x1 + 1x2 + 1x3 = 병상 13), 28박, 병실 타임라인당 순차 재원
~75건 (성별 50/50, 연령 버킷, 선호 정원, 필수/선호 장비 분포). Java 의
java.util.Random 스트림까지 재현하지는 않는다 — 분포와 규모가 기준이다.
"""

import random
from datetime import date, timedelta

from .models import BedModel, BedPlanModel, DepartmentModel, RoomModel, StayModel

__all__ = ["generate_demo_data"]

SPECIALTIES = ("Specialty1", "Specialty2", "Specialty3")
EQUIPMENT_TYPES = ("telemetry", "television", "oxygen", "nitrogen")

# 재현성을 위한 고정 기준일 (Java 는 이번 달 첫 월요일을 쓴다).
BASE_DATE = date(2026, 8, 3)


def _draw_index(rng: random.Random, cumulative: list[float]) -> int:
    """단위 난수 하나로 누적 분포표의 인덱스를 뽑는다 (Java 의 Pair 테이블)."""
    draw = rng.random()
    for i, threshold in enumerate(cumulative):
        if draw <= threshold:
            return i
    return len(cumulative) - 1


def _distinct_equipment(rng: random.Random, cumulative: list[float], count: int) -> list[str]:
    chosen: list[str] = []
    while len(chosen) < count:
        equipment = EQUIPMENT_TYPES[_draw_index(rng, cumulative)]
        if equipment not in chosen:
            chosen.append(equipment)
    return chosen


def generate_demo_data(seed: int = 0) -> BedPlanModel:
    """데모 `BedPlanModel` 을 만든다 (bed_id 는 전부 None — 풀기 전 상태)."""
    rng = random.Random(seed)

    departments = [
        DepartmentModel(
            id="1",
            name="Department",
            specialty_to_priority={"Specialty1": 1, "Specialty2": 2, "Specialty3": 2},
        )
    ]

    # 병실 10개, 전부 SAME_GENDER; 정원 80% -> 1, 10% -> 2, 10% -> 3.
    room_count = 10
    capacities = [1] * 8 + [2, 3]
    rng.shuffle(capacities)
    rooms = [
        RoomModel(
            id=str(r),
            name=f"Room{r}",
            department_id="1",
            capacity=capacities[r],
            gender_limitation="SAME_GENDER",
        )
        for r in range(room_count)
    ]
    # 76% 의 병실에 장비: 개수 분포 11% 1개 / 16% 2개 / 42% 3개 / 31% 4개.
    for room in rng.sample(rooms, int(0.76 * room_count)):
        equipment_count = _draw_index(rng, [0.11, 0.27, 0.69, 1.0]) + 1
        room.equipment = rng.sample(EQUIPMENT_TYPES, equipment_count)

    beds = [
        BedModel(id=f"{room.id}-bed{i}", room_id=room.id, index_in_room=i)
        for room in rooms
        for i in range(room.capacity)
    ]

    # 재원: 병실 타임라인마다 28박(일 0..27)을 순차 재원으로 채운다.
    # 길이 5% 1박 / 25% 2박 / 65% 3박 / 5% 4박, 재원 사이 1일 공백.
    last_day = 27
    stays: list[StayModel] = []
    for _ in range(room_count):
        current = 0
        while current < last_day:
            num_days = _draw_index(rng, [0.05, 0.30, 0.95, 1.0]) + 1
            departure = min(current + num_days, last_day)
            index = len(stays)
            stays.append(
                StayModel(
                    id=f"stay-{index}",
                    patient_name=f"patient-{index}",
                    patient_gender=rng.choice(("MALE", "FEMALE")),
                    patient_age=_draw_age(rng),
                    patient_preferred_maximum_room_capacity=_draw_index(rng, [0.34, 0.68, 1.0]) + 1,
                    patient_required_equipment=_draw_required(rng),
                    patient_preferred_equipment=_draw_preferred(rng),
                    arrival_date=BASE_DATE + timedelta(days=current),
                    departure_date=BASE_DATE + timedelta(days=departure),
                    specialty=SPECIALTIES[_draw_index(rng, [0.27, 0.63, 1.0])],
                )
            )
            current = departure + 1
    return BedPlanModel(departments=departments, rooms=rooms, beds=beds, stays=stays)


_AGE_BUCKETS = (
    (0, 10),
    (11, 20),
    (21, 30),
    (31, 40),
    (41, 50),
    (51, 60),
    (61, 70),
    (71, 80),
    (81, 90),
    (91, 100),
    (101, 109),
)
_AGE_CUMULATIVE = [0.10, 0.19, 0.26, 0.36, 0.45, 0.53, 0.61, 0.74, 0.82, 0.91, 1.0]


def _draw_age(rng: random.Random) -> int:
    low, high = _AGE_BUCKETS[_draw_index(rng, _AGE_CUMULATIVE)]
    return rng.randint(low, high)


def _draw_required(rng: random.Random) -> list[str]:
    """필수 장비: 12% 없음 / 47% 1개 / 41% 2개."""
    count = _draw_index(rng, [0.12, 0.59, 1.0])
    if count == 0:
        return []
    if count == 1:
        return _distinct_equipment(rng, [0.22, 0.47, 0.72, 1.0], 1)
    return _distinct_equipment(rng, [0.13, 0.29, 0.49, 1.0], 2)


def _draw_preferred(rng: random.Random) -> list[str]:
    """선호 장비: 29% 1개 / 53% 2개 / 16% 3개 / 2% 4개."""
    count = _draw_index(rng, [0.29, 0.82, 0.98, 1.0]) + 1
    return _distinct_equipment(rng, [0.32, 0.62, 0.90, 1.0], count)
