"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Department/Room/Bed/Stay/BedPlan 1:1, but like the REST shape of the
Java quickstart, references are id strings (`department_id`/`room_id`/`bed_id`) and
dates are `datetime.date`. The engine conversion (`to_core`) turns ids into indices and
dates into integer days (day ordinals)::

    from bed_allocation.models import BedPlanModel

    model = BedPlanModel.model_validate_json(payload)   # JSON -> pydantic
    plan = model.to_core()                              # pydantic -> engine
    ...solve...
    solved = BedPlanModel.from_core(plan)               # engine -> pydantic

`stays[i].bed_id` is the assigned bed id, or None when unassigned (including "no bed").
`pydantic` is an optional dependency (`pip install bed_allocation[models]`).
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
    "with_initial_solution",
]

GENDERS = ("MALE", "FEMALE")
GENDER_LIMITATIONS = ("ANY_GENDER", "MALE_ONLY", "FEMALE_ONLY", "SAME_GENDER")


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class DepartmentModel(_Model):
    """A department. specialty_to_priority holds the specialties the department offers
    and their priority (1 == highest)."""

    id: str
    name: str = ""
    specialty_to_priority: dict[str, int] = Field(default_factory=dict)
    minimum_age: int | None = None
    maximum_age: int | None = None


class RoomModel(_Model):
    """A room. gender_limitation: ANY_GENDER / MALE_ONLY / FEMALE_ONLY /
    SAME_GENDER (concurrent patients in one room must share a gender)."""

    id: str
    name: str = ""
    department_id: str
    capacity: int = Field(ge=0)
    gender_limitation: str = "ANY_GENDER"
    equipment: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _valid_limitation(self):
        if self.gender_limitation not in GENDER_LIMITATIONS:
            raise ValueError(f"gender_limitation must be one of {GENDER_LIMITATIONS}: {self.gender_limitation!r}")
        return self


class BedModel(_Model):
    """A bed (planning value)."""

    id: str
    room_id: str
    index_in_room: int = 0


class StayModel(_Model):
    """Planning entity: one patient stay. `bed_id` is the planning variable
    (None == unassigned)."""

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
            raise ValueError(f"patient_gender must be one of {GENDERS}: {self.patient_gender!r}")
        if self.departure_date < self.arrival_date:
            raise ValueError("departure_date cannot be before arrival_date")
        return self


class BedPlanModel(_Model):
    """Planning solution mirror. score_* are read-only results, kept as separate fields."""

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
            raise ValueError("duplicate department id")
        if len(room_ids) != len(self.rooms):
            raise ValueError("duplicate room id")
        if len(bed_ids) != len(self.beds):
            raise ValueError("duplicate bed id")
        for room in self.rooms:
            if room.department_id not in department_ids:
                raise ValueError(f"room {room.id!r} has unknown department_id {room.department_id!r}")
        for bed in self.beds:
            if bed.room_id not in room_ids:
                raise ValueError(f"bed {bed.id!r} has unknown room_id {bed.room_id!r}")
        for stay in self.stays:
            if stay.bed_id is not None and stay.bed_id not in bed_ids:
                raise ValueError(f"stay {stay.id!r} has unknown bed_id {stay.bed_id!r}")
        return self

    def to_core(self) -> "_core.BedPlan":
        """pydantic -> engine object (id references to indices, dates to day ordinals;
        the score field is ignored since the engine recomputes it). bed_id None becomes
        uninitialized (-1) — the solver decides the assignment."""
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
        """engine object -> pydantic (including the current assignment and score). Values that
        are not real beds (-1 uninitialized, the "no bed" sentinel) become bed_id None."""
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


def _solution_dict(solution) -> dict:
    if isinstance(solution, BaseModel):
        return solution.model_dump()
    if isinstance(solution, dict):
        return solution
    raise ValueError(f"solution must be a solved model or a dict of that shape: {type(solution)!r}")


def _entities_by_id(solved: dict, name: str, ids: list, key: str = "id") -> dict:
    """`solved[name]` by id. Rejects an id repeated on either side and an id outside `ids`."""
    entities = solved.get(name)
    if not isinstance(entities, list):
        raise ValueError(f"solution has no {name!r} list")
    known = set(ids)
    if len(known) != len(ids):
        raise ValueError(f"problem repeats an id in {name!r}: {next(i for i in ids if ids.count(i) > 1)!r}")
    by_id: dict = {}
    for entity in entities:
        entity_id = entity.get(key) if isinstance(entity, dict) else None
        if not isinstance(entity_id, (str, int)) or entity_id not in known:
            raise ValueError(f"solution has an unknown id in {name!r}: {entity_id!r}")
        if entity_id in by_id:
            raise ValueError(f"solution repeats an id in {name!r}: {entity_id!r}")
        by_id[entity_id] = entity
    return by_id


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns: a `BedPlanModel`, or a plain dict of that shape
    (the JSON the API returned). Stays are matched by id. A stay left out becomes
    unassigned and the construction heuristic places it, and only it, so a complete
    solution starts the search exactly from it. `problem` is not mutated.

    A "no bed" stay has no bed id, so `from_core` reports it as bed_id None and it
    comes back here as unassigned: construction places it again, on a bed or on "no
    bed" whichever scores best.

    Raises ValueError naming the stay for an unknown or repeated stay id and for an
    unknown bed id (the engine does not range-check).
    """
    if not isinstance(problem, BedPlanModel):
        raise TypeError(f"problem must be a BedPlanModel: {type(problem)!r}")
    stays = _entities_by_id(_solution_dict(solution), "stays", [s.id for s in problem.stays])
    bed_ids = {b.id for b in problem.beds}
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_medium = seeded.score_soft = None
    for stay in seeded.stays:
        stay.bed_id = stays.get(stay.id, {}).get("bed_id")
        if stay.bed_id is not None and (not isinstance(stay.bed_id, str) or stay.bed_id not in bed_ids):
            raise ValueError(f"stay {stay.id!r} has unknown bed_id {stay.bed_id!r}")
    return seeded
