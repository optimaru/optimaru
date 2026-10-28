"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ba.solve(plan)`) enables all twelve constraints at the quickstart weights.
This module is the alternative path next to it: pick constraints, change weights, flip
hard/soft, and pass the list to `solve(plan, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import bed_allocation as ba
    from bed_allocation.constraints import (
        DepartmentSpecialty, get_constraints, same_bed_in_same_night,
    )

    ba.solve(plan, constraints=get_constraints())              # defaults as-is
    ba.solve(plan, constraints=get_constraints([               # picked
        same_bed_in_same_night, DepartmentSpecialty(weight=40),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default.
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
               **Medium constraints in this domain cannot be expressed with
               this knob** — ``hard=False`` folds them into soft and the medium
               meaning is lost. Leave medium terms at ``None``.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ba.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "SameBedInSameNight",
    "FemaleInMaleRoom",
    "MaleInFemaleRoom",
    "DifferentGenderInSameGenderRoomInSameNight",
    "DepartmentMinimumAge",
    "DepartmentMaximumAge",
    "RequiredPatientEquipment",
    "AssignEveryPatientToABed",
    "PreferredMaximumRoomCapacity",
    "DepartmentSpecialty",
    "DepartmentSpecialtyNotFirstPriority",
    "PreferredPatientEquipment",
    "same_bed_in_same_night",
    "female_in_male_room",
    "male_in_female_room",
    "different_gender_in_same_gender_room_in_same_night",
    "department_minimum_age",
    "department_maximum_age",
    "required_patient_equipment",
    "assign_every_patient_to_a_bed",
    "preferred_maximum_room_capacity",
    "department_specialty",
    "department_specialty_not_first_priority",
    "preferred_patient_equipment",
    "get_constraints",
]


@dataclass(frozen=True)
class Constraint:
    """Spec selecting one constraint: weight / level / shaping."""

    weight: float | None = None  # None = component default weight
    hard: bool | None = None  # None = component default level
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class SameBedInSameNight(Constraint):
    """Nights in which two stays use the same bed. Default: hard, weight 1000."""

    name = "same_bed_in_same_night"


class FemaleInMaleRoom(Constraint):
    """Nights a female patient spends in a MALE_ONLY room. Default: hard, weight 50."""

    name = "female_in_male_room"


class MaleInFemaleRoom(Constraint):
    """Nights a male patient spends in a FEMALE_ONLY room. Default: hard, weight 50."""

    name = "male_in_female_room"


class DifferentGenderInSameGenderRoomInSameNight(Constraint):
    """Nights in which men and women share a SAME_GENDER room. Default: hard, weight 1000.

    The pairing is per room while the calculator's unit is the bed, so this is a global constraint."""

    name = "different_gender_in_same_gender_room_in_same_night"


class DepartmentMinimumAge(Constraint):
    """Nights a patient below the department minimum age spends there. Default: hard, weight 100."""

    name = "department_minimum_age"


class DepartmentMaximumAge(Constraint):
    """Nights a patient above the department maximum age spends there. Default: hard, weight 100."""

    name = "department_maximum_age"


class RequiredPatientEquipment(Constraint):
    """Room assignments missing required equipment. Default: hard, weight 50.

    If the room lacks any required equipment, it is valued by the number of required
    equipment items the room **has** — exactly the semantics pinned by the quickstart's
    unit test (so a room with none of the required equipment costs 0)."""

    name = "required_patient_equipment"


class AssignEveryPatientToABed(Constraint):
    """ Nights spent on the "no bed" sentinel. Default: medium, weight 10^9.

    The medium level is folded into the engine soft level as `medium * kMediumUnit`,
    so the default weight already carries that multiple (kMediumUnit = 10^9)."""

    name = "assign_every_patient_to_a_bed"


class PreferredMaximumRoomCapacity(Constraint):
    """Nights spent in a room larger than the preferred capacity. Default: soft, weight 8."""

    name = "preferred_maximum_room_capacity"


class DepartmentSpecialty(Constraint):
    """Nights spent in a department that does not offer the specialty at all. Default: soft, weight 10."""

    name = "department_specialty"


class DepartmentSpecialtyNotFirstPriority(Constraint):
    """Nights spent in a department offering the specialty only at priority > 1. Default: soft, weight 10.

    Valued by (priority - 1). A department not offering the specialty at all is already
    valued by `department_specialty`, so this stays silent."""

    name = "department_specialty_not_first_priority"


class PreferredPatientEquipment(Constraint):
    """Nights spent in a room missing preferred equipment. Default: soft, weight 50.

    Valued by the number of preferred equipment items the room lacks."""

    name = "preferred_patient_equipment"


#: Instances at the default weights. Reference them directly when picking.
same_bed_in_same_night = SameBedInSameNight()
female_in_male_room = FemaleInMaleRoom()
male_in_female_room = MaleInFemaleRoom()
different_gender_in_same_gender_room_in_same_night = DifferentGenderInSameGenderRoomInSameNight()
department_minimum_age = DepartmentMinimumAge()
department_maximum_age = DepartmentMaximumAge()
required_patient_equipment = RequiredPatientEquipment()
assign_every_patient_to_a_bed = AssignEveryPatientToABed()
preferred_maximum_room_capacity = PreferredMaximumRoomCapacity()
department_specialty = DepartmentSpecialty()
department_specialty_not_first_priority = DepartmentSpecialtyNotFirstPriority()
preferred_patient_equipment = PreferredPatientEquipment()

#: Default set — same order as `ba.list_constraints()`. Assembly lists unit
#: constraints first and global constraints after, so the global
#: `different_gender_in_same_gender_room_in_same_night` comes last
#: (unlike the emplace order of C++ `defaultConstraints()`).
_DEFAULTS: list[Constraint] = [
    same_bed_in_same_night,
    female_in_male_room,
    male_in_female_room,
    department_minimum_age,
    department_maximum_age,
    required_patient_equipment,
    assign_every_patient_to_a_bed,
    preferred_maximum_room_capacity,
    department_specialty,
    department_specialty_not_first_priority,
    preferred_patient_equipment,
    different_gender_in_same_gender_room_in_same_night,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all twelve default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([same_bed_in_same_night, DepartmentSpecialty(weight=40)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # Same contract as C++ applyConstraints: an empty list means "score
        # nothing", which is most likely a mistake. A filter that happens to come
        # out empty must not silently flip to "score everything".
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """(hard, soft) returned by `score()`. Both are penalties, so <= 0."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """Sign-preserving power. Rewards (negative costs) never flip into penalties."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `ba.constraint_costs(plan)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = ba.constraint_costs(plan)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft). This domain folds
    medium into engine soft as multiples of kMediumUnit (10^9), so to compare
    with the split levels of `solution.score`, fold the same way first.
    """
    hard = 0
    soft = 0.0
    for spec in get_constraints(constraints):
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: " + ", ".join(repr(name) for name in sorted(costs))
            )
        weight = entry["weight"] if spec.weight is None else spec.weight
        is_hard = entry["hard"] if spec.hard is None else spec.hard
        exponent = spec.exponent
        for cost in entry["costs"]:
            value = weight * _shape(cost, exponent)
            if is_hard:
                # Round per unit contribution like the engine (half-away-from-zero
                # like llround; Python's round is banker's rounding).
                hard += int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
