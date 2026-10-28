"""Python reproduction of the Java quickstart `DemoDataGenerator`.

Builds a demo instance of the same scale and distributions: 1 department (3 specialties),
10 SAME_GENDER rooms (capacity 8x1 + 1x2 + 1x3 = 13 beds), 28 nights, ~75 sequential
stays per room timeline (gender 50/50, age buckets, preferred capacity, required/preferred
equipment distributions). Java's java.util.Random stream is not reproduced — distributions
and scale are the reference.
"""

import random
from datetime import date, timedelta

from .models import BedModel, BedPlanModel, DepartmentModel, RoomModel, StayModel

__all__ = ["generate_demo_data"]

SPECIALTIES = ("Specialty1", "Specialty2", "Specialty3")
EQUIPMENT_TYPES = ("telemetry", "television", "oxygen", "nitrogen")

# Fixed reference date for reproducibility (Java uses the first Monday of the current month).
BASE_DATE = date(2026, 8, 3)


def _draw_index(rng: random.Random, cumulative: list[float]) -> int:
    """Pick an index from a cumulative distribution table with one unit random draw (Java's Pair table)."""
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
    """Build the demo `BedPlanModel` (every bed_id None — the unsolved state)."""
    rng = random.Random(seed)

    departments = [
        DepartmentModel(
            id="1",
            name="Department",
            specialty_to_priority={"Specialty1": 1, "Specialty2": 2, "Specialty3": 2},
        )
    ]

    # 10 rooms, all SAME_GENDER; capacity 80% -> 1, 10% -> 2, 10% -> 3.
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
    # Equipment in 76% of rooms: count distribution 11% one / 16% two / 42% three / 31% four.
    for room in rng.sample(rooms, int(0.76 * room_count)):
        equipment_count = _draw_index(rng, [0.11, 0.27, 0.69, 1.0]) + 1
        room.equipment = rng.sample(EQUIPMENT_TYPES, equipment_count)

    beds = [
        BedModel(id=f"{room.id}-bed{i}", room_id=room.id, index_in_room=i)
        for room in rooms
        for i in range(room.capacity)
    ]

    # Stays: fill 28 nights (days 0..27) per room timeline with sequential stays.
    # Length 5% 1 night / 25% 2 / 65% 3 / 5% 4, with a 1-day gap between stays.
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
    """Required equipment: 12% none / 47% one / 41% two."""
    count = _draw_index(rng, [0.12, 0.59, 1.0])
    if count == 0:
        return []
    if count == 1:
        return _distinct_equipment(rng, [0.22, 0.47, 0.72, 1.0], 1)
    return _distinct_equipment(rng, [0.13, 0.29, 0.49, 1.0], 2)


def _draw_preferred(rng: random.Random) -> list[str]:
    """Preferred equipment: 29% one / 53% two / 16% three / 2% four."""
    count = _draw_index(rng, [0.29, 0.82, 0.98, 1.0]) + 1
    return _distinct_equipment(rng, [0.32, 0.62, 0.90, 1.0], count)
