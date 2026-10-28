"""Class-based CP-SAT plugins for bed allocation."""

from dataclasses import dataclass, field
from typing import Any, ClassVar

from bed_allocation import constraints as domain
from bed_allocation.models import BedPlanModel
from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model


@dataclass
class BedContext:
    model: cp_model.CpModel
    problem: BedPlanModel
    x: dict[tuple[int, int], Any]
    no_bed: int
    room_of_bed: list[Any]
    departments: dict[str, Any]
    _and_cache: dict[tuple[Any, ...], Any] = field(default_factory=dict)

    def both(self, key: tuple[Any, ...], left: Any, right: Any) -> Any:
        if key not in self._and_cache:
            value = self.model.new_bool_var("_".join(map(str, key)))
            self.model.add(value <= left)
            self.model.add(value <= right)
            self.model.add(value >= left + right - 1)
            self._and_cache[key] = value
        return self._and_cache[key]

    @staticmethod
    def nights(stay: Any) -> int:
        return (stay.departure_date - stay.arrival_date).days + 1


class AssignEveryPatientToABed(
    domain.AssignEveryPatientToABed, ConstraintPlugin[BedContext]
):
    default_level = "medium"

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        for stay, stay_data in enumerate(context.problem.stays):
            nights = context.nights(stay_data)
            objective.add(
                self.name, nights * context.x[stay, context.no_bed], bound=nights
            )


class _StayRoomPlugin(ConstraintPlugin[BedContext]):
    rule: ClassVar[str]

    def amount(self, stay: Any, room: Any, department: Any, nights: int) -> int:
        if self.rule == "female_in_male":
            return nights * int(
                stay.patient_gender == "FEMALE"
                and room.gender_limitation == "MALE_ONLY"
            )
        if self.rule == "male_in_female":
            return nights * int(
                stay.patient_gender == "MALE"
                and room.gender_limitation == "FEMALE_ONLY"
            )
        if self.rule == "minimum_age":
            return nights * int(
                department.minimum_age is not None
                and stay.patient_age < department.minimum_age
            )
        if self.rule == "maximum_age":
            return nights * int(
                department.maximum_age is not None
                and stay.patient_age > department.maximum_age
            )
        if self.rule == "required_equipment":
            present = len(set(stay.patient_required_equipment) & set(room.equipment))
            return (
                nights * present
                if present < len(set(stay.patient_required_equipment))
                else 0
            )
        if self.rule == "preferred_capacity":
            return nights * int(
                stay.patient_preferred_maximum_room_capacity is not None
                and room.capacity > stay.patient_preferred_maximum_room_capacity
            )
        priority = department.specialty_to_priority.get(stay.specialty)
        if self.rule == "specialty":
            return nights * int(priority is None)
        if self.rule == "specialty_priority":
            return (
                (priority - 1) * nights if priority is not None and priority > 1 else 0
            )
        return nights * len(set(stay.patient_preferred_equipment) - set(room.equipment))

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        for stay_index, stay in enumerate(context.problem.stays):
            nights = context.nights(stay)
            for bed, room in enumerate(context.room_of_bed):
                amount = self.amount(
                    stay, room, context.departments[room.department_id], nights
                )
                objective.add(
                    self.name, amount * context.x[stay_index, bed], bound=amount
                )


class FemaleInMaleRoom(domain.FemaleInMaleRoom, _StayRoomPlugin):
    default_level = "hard"
    default_weight = 50
    rule = "female_in_male"


class MaleInFemaleRoom(domain.MaleInFemaleRoom, _StayRoomPlugin):
    default_level = "hard"
    default_weight = 50
    rule = "male_in_female"


class DepartmentMinimumAge(domain.DepartmentMinimumAge, _StayRoomPlugin):
    default_level = "hard"
    default_weight = 100
    rule = "minimum_age"


class DepartmentMaximumAge(domain.DepartmentMaximumAge, _StayRoomPlugin):
    default_level = "hard"
    default_weight = 100
    rule = "maximum_age"


class RequiredPatientEquipment(domain.RequiredPatientEquipment, _StayRoomPlugin):
    default_level = "hard"
    default_weight = 50
    rule = "required_equipment"


class PreferredMaximumRoomCapacity(
    domain.PreferredMaximumRoomCapacity, _StayRoomPlugin
):
    default_level = "soft"
    default_weight = 8
    rule = "preferred_capacity"


class DepartmentSpecialty(domain.DepartmentSpecialty, _StayRoomPlugin):
    default_level = "soft"
    default_weight = 10
    rule = "specialty"


class DepartmentSpecialtyNotFirstPriority(
    domain.DepartmentSpecialtyNotFirstPriority, _StayRoomPlugin
):
    default_level = "soft"
    default_weight = 10
    rule = "specialty_priority"


class PreferredPatientEquipment(domain.PreferredPatientEquipment, _StayRoomPlugin):
    default_level = "soft"
    default_weight = 50
    rule = "preferred_equipment"


class SameBedInSameNight(domain.SameBedInSameNight, ConstraintPlugin[BedContext]):
    default_level = "hard"
    default_weight = 1_000

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.stays)):
            for right in range(left + 1, len(context.problem.stays)):
                a, b = context.problem.stays[left], context.problem.stays[right]
                overlap = max(
                    0,
                    (
                        min(a.departure_date, b.departure_date)
                        - max(a.arrival_date, b.arrival_date)
                    ).days
                    + 1,
                )
                for bed in range(len(context.problem.beds)):
                    both = context.both(
                        ("same_bed", left, right, bed),
                        context.x[left, bed],
                        context.x[right, bed],
                    )
                    objective.add(self.name, overlap * both, bound=overlap)


class DifferentGenderInSameGenderRoomInSameNight(
    domain.DifferentGenderInSameGenderRoomInSameNight,
    ConstraintPlugin[BedContext],
):
    default_level = "hard"
    default_weight = 1_000

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.stays)):
            for right in range(left + 1, len(context.problem.stays)):
                a, b = context.problem.stays[left], context.problem.stays[right]
                if a.patient_gender == b.patient_gender:
                    continue
                overlap = max(
                    0,
                    (
                        min(a.departure_date, b.departure_date)
                        - max(a.arrival_date, b.arrival_date)
                    ).days
                    + 1,
                )
                if not overlap:
                    continue
                for room in context.problem.rooms:
                    if room.gender_limitation != "SAME_GENDER":
                        continue
                    beds = [
                        index
                        for index, bed in enumerate(context.problem.beds)
                        if bed.room_id == room.id
                    ]
                    left_in_room = context.model.new_bool_var(
                        f"stay_{left}_room_{room.id}"
                    )
                    right_in_room = context.model.new_bool_var(
                        f"stay_{right}_room_{room.id}"
                    )
                    context.model.add(
                        left_in_room == sum(context.x[left, bed] for bed in beds)
                    )
                    context.model.add(
                        right_in_room == sum(context.x[right, bed] for bed in beds)
                    )
                    both = context.both(
                        ("mixed", left, right, room.id),
                        left_in_room,
                        right_in_room,
                    )
                    objective.add(self.name, overlap * both, bound=overlap)


REGISTRY = ConstraintRegistry(
    [
        SameBedInSameNight(),
        FemaleInMaleRoom(),
        MaleInFemaleRoom(),
        DepartmentMinimumAge(),
        DepartmentMaximumAge(),
        RequiredPatientEquipment(),
        AssignEveryPatientToABed(),
        PreferredMaximumRoomCapacity(),
        DepartmentSpecialty(),
        DepartmentSpecialtyNotFirstPriority(),
        PreferredPatientEquipment(),
        DifferentGenderInSameGenderRoomInSameNight(),
    ]
)
