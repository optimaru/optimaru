"""Class-based CP-SAT plugins for bed allocation.

Each plugin subclasses the domain spec of the same name and carries only ``build()``;
its default level and weight come from ``bed_allocation.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so nothing is restated here. The registry
keeps the ``list_constraints()`` order (the global ``MixedGenderRoomCost`` last).

Every plugin adds one term per unit the engine evaluates: the per-bed constraints one
per real bed (an empty one included; the "no bed" sentinel is no bed), the scoped
``UnassignedStaysCost`` one for the sentinel only, and the global ``MixedGenderRoomCost``
one for its total. A pick's ``offset`` / ``clip`` therefore shape each unit exactly as
the engine does (``terms_per_unit``), and a constraint picked several times adds one set
of terms per pick.
"""

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


class UnassignedStaysCost(domain.UnassignedStaysCost, ConstraintPlugin[BedContext]):
    terms_per_unit = True  # scoped to the sentinel: its one unit

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        nights = [context.nights(stay) for stay in context.problem.stays]
        objective.add(
            self.name,
            sum(
                count * context.x[stay, context.no_bed]
                for stay, count in enumerate(nights)
            ),
            bound=sum(nights),
        )


class _StayRoomPlugin(ConstraintPlugin[BedContext]):
    rule: ClassVar[str]
    terms_per_unit = True  # one term per real bed

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
        stays = context.problem.stays
        for bed, room in enumerate(context.room_of_bed):
            department = context.departments[room.department_id]
            amounts = [
                self.amount(stay, room, department, context.nights(stay))
                for stay in stays
            ]
            objective.add(
                self.name,
                sum(
                    amount * context.x[stay, bed] for stay, amount in enumerate(amounts)
                ),
                bound=sum(amounts),
            )


class FemaleInMaleRoomCost(domain.FemaleInMaleRoomCost, _StayRoomPlugin):
    rule = "female_in_male"


class MaleInFemaleRoomCost(domain.MaleInFemaleRoomCost, _StayRoomPlugin):
    rule = "male_in_female"


class DepartmentMinAgeCost(domain.DepartmentMinAgeCost, _StayRoomPlugin):
    rule = "minimum_age"


class DepartmentMaxAgeCost(domain.DepartmentMaxAgeCost, _StayRoomPlugin):
    rule = "maximum_age"


class RequiredPatientEquipmentCost(
    domain.RequiredPatientEquipmentCost, _StayRoomPlugin
):
    rule = "required_equipment"


class PreferredMaxRoomCapacityCost(
    domain.PreferredMaxRoomCapacityCost, _StayRoomPlugin
):
    rule = "preferred_capacity"


class DepartmentSpecialtyCost(domain.DepartmentSpecialtyCost, _StayRoomPlugin):
    rule = "specialty"


class DepartmentSpecialtyPriorityCost(
    domain.DepartmentSpecialtyPriorityCost, _StayRoomPlugin
):
    rule = "specialty_priority"


class PreferredPatientEquipmentCost(
    domain.PreferredPatientEquipmentCost, _StayRoomPlugin
):
    rule = "preferred_equipment"


class BedConflictCost(domain.BedConflictCost, ConstraintPlugin[BedContext]):
    terms_per_unit = True  # one term per real bed

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        bed_count = len(context.problem.beds)
        terms: list[list[Any]] = [[] for _ in range(bed_count)]
        bounds = [0] * bed_count
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
                for bed in range(bed_count):
                    both = context.both(
                        ("same_bed", left, right, bed),
                        context.x[left, bed],
                        context.x[right, bed],
                    )
                    if overlap:
                        terms[bed].append(overlap * both)
                        bounds[bed] += overlap
        for bed in range(bed_count):
            objective.add(self.name, sum(terms[bed]), bound=bounds[bed])


class MixedGenderRoomCost(domain.MixedGenderRoomCost, ConstraintPlugin[BedContext]):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: BedContext, objective: ObjectiveBuilder) -> None:
        terms: list[Any] = []
        bound = 0
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
                    terms.append(overlap * both)
                    bound += overlap
        objective.add(self.name, sum(terms), bound=bound)


REGISTRY = ConstraintRegistry(
    [
        BedConflictCost(),
        FemaleInMaleRoomCost(),
        MaleInFemaleRoomCost(),
        DepartmentMinAgeCost(),
        DepartmentMaxAgeCost(),
        RequiredPatientEquipmentCost(),
        UnassignedStaysCost(),
        PreferredMaxRoomCapacityCost(),
        DepartmentSpecialtyCost(),
        DepartmentSpecialtyPriorityCost(),
        PreferredPatientEquipmentCost(),
        MixedGenderRoomCost(),
    ]
)
