"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Department/Room/Bed/Stay/BedPlan 을 1:1 로 미러링하되, Java
quickstart 의 REST 표현처럼 참조는 id 문자열(`department_id`/`room_id`/
`bed_id`)로, 날짜는 `datetime.date` 로 둔다. 엔진 변환(`to_core`)이 id 를
인덱스로, 날짜를 정수 일(day ordinal)로 옮긴다::

    from bed_allocation.models import BedPlanModel

    model = BedPlanModel.model_validate_json(payload)   # JSON -> pydantic
    plan = model.to_core()                              # pydantic -> 엔진
    ...solve...
    solved = BedPlanModel.from_core(plan)               # 엔진 -> pydantic

`stays[i].bed_id` 는 배정된 병상 id, 미배정("no bed" 포함)이면 None 이다.
`pydantic` 은 선택 의존성이다 (`pip install bed_allocation[models]`).
"""

from datetime import date

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "GENDERS",
    "GENDER_LIMITATIONS",
    "BedModel",
    "BedPlanModel",
    "DepartmentModel",
    "RoomModel",
    "StayModel",
]

GENDERS = ("MALE", "FEMALE")
GENDER_LIMITATIONS = ("ANY_GENDER", "MALE_ONLY", "FEMALE_ONLY", "SAME_GENDER")


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class DepartmentModel(_Model):
    """진료 부서. specialty_to_priority 는 부서가 제공하는 전문과목과 그
    우선순위(1 == 최우선)."""

    id: str
    name: str = ""
    specialty_to_priority: dict[str, int] = Field(default_factory=dict)
    minimum_age: int | None = None
    maximum_age: int | None = None


class RoomModel(_Model):
    """병실. gender_limitation: ANY_GENDER / MALE_ONLY / FEMALE_ONLY /
    SAME_GENDER (한 병실의 동시 재원 환자는 같은 성별이어야 함)."""

    id: str
    name: str = ""
    department_id: str
    capacity: int = Field(ge=0)
    gender_limitation: str = "ANY_GENDER"
    equipment: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _valid_limitation(self):
        if self.gender_limitation not in GENDER_LIMITATIONS:
            raise ValueError(f"gender_limitation 은 {GENDER_LIMITATIONS} 중 하나여야 한다: {self.gender_limitation!r}")
        return self


class BedModel(_Model):
    """병상 (planning value)."""

    id: str
    room_id: str
    index_in_room: int = 0


class StayModel(_Model):
    """planning entity: 환자의 재원 한 건. `bed_id` 가 planning variable
    (None == 미배정)."""

    id: str
    patient_name: str = ""
    patient_gender: str = "MALE"
    patient_age: int = Field(ge=0)
    patient_preferred_maximum_room_capacity: int | None = None
    patient_required_equipment: list[str] = Field(default_factory=list)
    patient_preferred_equipment: list[str] = Field(default_factory=list)
    arrival_date: date
    departure_date: date
    specialty: str = ""
    bed_id: str | None = None

    @model_validator(mode="after")
    def _valid(self):
        if self.patient_gender not in GENDERS:
            raise ValueError(f"patient_gender 는 {GENDERS} 중 하나여야 한다: {self.patient_gender!r}")
        if self.departure_date < self.arrival_date:
            raise ValueError("departure_date 는 arrival_date 보다 빠를 수 없다")
        return self


class BedPlanModel(_Model):
    """planning solution 미러. score_* 는 읽기 전용 결과라 별도 필드로 둔다."""

    departments: list[DepartmentModel]
    rooms: list[RoomModel]
    beds: list[BedModel]
    stays: list[StayModel]
    score_hard: int | None = None
    score_medium: int | None = None
    score_soft: int | None = None

    @model_validator(mode="after")
    def _valid_references(self):
        department_ids = {d.id for d in self.departments}
        room_ids = {r.id for r in self.rooms}
        bed_ids = {b.id for b in self.beds}
        if len(department_ids) != len(self.departments):
            raise ValueError("department id 가 중복된다")
        if len(room_ids) != len(self.rooms):
            raise ValueError("room id 가 중복된다")
        if len(bed_ids) != len(self.beds):
            raise ValueError("bed id 가 중복된다")
        for room in self.rooms:
            if room.department_id not in department_ids:
                raise ValueError(f"room {room.id!r} 의 department_id {room.department_id!r} 가 없다")
        for bed in self.beds:
            if bed.room_id not in room_ids:
                raise ValueError(f"bed {bed.id!r} 의 room_id {bed.room_id!r} 가 없다")
        for stay in self.stays:
            if stay.bed_id is not None and stay.bed_id not in bed_ids:
                raise ValueError(f"stay {stay.id!r} 의 bed_id {stay.bed_id!r} 가 없다")
        return self

    def to_core(self) -> "_core.BedPlan":
        """pydantic -> 엔진 객체 (id 참조를 인덱스로, 날짜를 day ordinal 로;
        score 필드는 엔진이 다시 계산하므로 무시). bed_id None 은 미초기화
        (-1) 로 옮긴다 — 솔버가 배정을 결정한다."""
        department_index = {d.id: i for i, d in enumerate(self.departments)}
        room_index = {r.id: i for i, r in enumerate(self.rooms)}
        bed_index = {b.id: i for i, b in enumerate(self.beds)}
        return _core.BedPlan(
            [
                _core.Department(
                    d.id,
                    d.name,
                    dict(d.specialty_to_priority),
                    d.minimum_age,
                    d.maximum_age,
                )
                for d in self.departments
            ],
            [
                _core.Room(
                    r.id,
                    r.name,
                    department_index[r.department_id],
                    r.capacity,
                    getattr(_core.GenderLimitation, r.gender_limitation),
                    list(r.equipment),
                )
                for r in self.rooms
            ],
            [_core.Bed(b.id, room_index[b.room_id], b.index_in_room) for b in self.beds],
            [
                _core.Stay(
                    s.id,
                    s.patient_name,
                    getattr(_core.Gender, s.patient_gender),
                    s.patient_age,
                    s.arrival_date.toordinal(),
                    s.departure_date.toordinal(),
                    s.specialty,
                    s.patient_preferred_maximum_room_capacity,
                    list(s.patient_required_equipment),
                    list(s.patient_preferred_equipment),
                    -1 if s.bed_id is None else bed_index[s.bed_id],
                )
                for s in self.stays
            ],
        )

    @classmethod
    def from_core(cls, plan: "_core.BedPlan") -> "BedPlanModel":
        """엔진 객체 -> pydantic (현재 배정과 score 포함). 실제 병상이 아닌
        값(-1 미초기화, "no bed" 센티널)은 bed_id None 이 된다."""
        bed_count = plan.bed_count()
        return cls(
            departments=[
                DepartmentModel(
                    id=d.id,
                    name=d.name,
                    specialty_to_priority=dict(d.specialty_to_priority),
                    minimum_age=d.minimum_age,
                    maximum_age=d.maximum_age,
                )
                for d in plan.departments
            ],
            rooms=[
                RoomModel(
                    id=r.id,
                    name=r.name,
                    department_id=plan.departments[r.department].id,
                    capacity=r.capacity,
                    gender_limitation=r.gender_limitation.name,
                    equipment=list(r.equipment),
                )
                for r in plan.rooms
            ],
            beds=[
                BedModel(
                    id=b.id,
                    room_id=plan.rooms[b.room].id,
                    index_in_room=b.index_in_room,
                )
                for b in plan.beds
            ],
            stays=[
                StayModel(
                    id=s.id,
                    patient_name=s.patient_name,
                    patient_gender=s.patient_gender.name,
                    patient_age=s.patient_age,
                    patient_preferred_maximum_room_capacity=(s.patient_preferred_maximum_room_capacity),
                    patient_required_equipment=list(s.patient_required_equipment),
                    patient_preferred_equipment=list(s.patient_preferred_equipment),
                    arrival_date=date.fromordinal(s.arrival_day),
                    departure_date=date.fromordinal(s.departure_day),
                    specialty=s.specialty,
                    bed_id=(plan.beds[s.bed].id if 0 <= s.bed < bed_count else None),
                )
                for s in plan.stays
            ],
            score_hard=plan.score.hard,
            score_medium=plan.score.medium,
            score_soft=plan.score.soft,
        )
