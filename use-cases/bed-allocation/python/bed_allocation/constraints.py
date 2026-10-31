"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`ba.solve(plan)`) enables all twelve constraints at their default
weights (the quickstart's). This module is the alternative path next to it: pick
constraints, change weights, flip hard/soft, and pass the list to
`solve(plan, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import bed_allocation as ba
    from bed_allocation.constraints import (
        DepartmentSpecialtyCost, bed_conflict_cost, get_constraints,
    )

    ba.solve(plan, constraints=get_constraints())              # defaults as-is
    ba.solve(plan, constraints=get_constraints([               # picked
        bed_conflict_cost, DepartmentSpecialtyCost(weight=40),
    ]))

Each constraint has a default instance at module level (`bed_conflict_cost`,
`unassigned_stays_cost`, ...), so you can also pass a list of just a few. Only
listed constraints are scored — leave one out and its term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost, in units of its level.
               ``None`` = the component default (``ba.constraint_info()`` reports
               them; the plan carries none).
- ``hard``     ``True`` = hard, ``False`` = soft, ``None`` = the default level.
               ``unassigned_stays_cost`` is the one MEDIUM constraint: medium is a
               default-only level, so ``hard=True`` makes it one hard unit per
               night and ``hard=False`` one soft unit per night (outside the 10^9
               medium fold), in the solver and in :func:`score` alike. An explicit
               ``weight`` keeps it on the medium level, in medium units.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``, per bed for the unit constraints and on the single total
               for the global ``mixed_gender_room_cost``. 2.0 = squared, so
               concentrated violations grow much costlier.

Names (`name`) are the C++ component names in snake_case
(``BedConflictCost`` -> ``bed_conflict_cost``). The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`ba.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

from ._constraint import Constraint, Score, score_costs, select

__all__ = [
    "Constraint",
    "Score",
    "score",
    "BedConflictCost",
    "FemaleInMaleRoomCost",
    "MaleInFemaleRoomCost",
    "MixedGenderRoomCost",
    "DepartmentMinAgeCost",
    "DepartmentMaxAgeCost",
    "RequiredPatientEquipmentCost",
    "UnassignedStaysCost",
    "PreferredMaxRoomCapacityCost",
    "DepartmentSpecialtyCost",
    "DepartmentSpecialtyPriorityCost",
    "PreferredPatientEquipmentCost",
    "bed_conflict_cost",
    "female_in_male_room_cost",
    "male_in_female_room_cost",
    "mixed_gender_room_cost",
    "department_min_age_cost",
    "department_max_age_cost",
    "required_patient_equipment_cost",
    "unassigned_stays_cost",
    "preferred_max_room_capacity_cost",
    "department_specialty_cost",
    "department_specialty_priority_cost",
    "preferred_patient_equipment_cost",
    "get_constraints",
]


class BedConflictCost(Constraint):
    """Nights in which two stays use the same bed. Default: hard, weight 1000."""

    name = "bed_conflict_cost"
    label_en = "Bed conflict"
    label_ko = "병상 중복 배정"
    description_en = (
        "Nights on which two stays share one bed, summed over every pair of stays in the same bed. "
        "One unit of cost is one shared night; 0 means no bed is double-booked on any night."
    )
    description_ko = (
        "같은 병상에 배정된 두 입원이 겹치는 밤을 모든 쌍에 대해 더합니다. "
        "겹치는 밤 하루가 비용 1이며, 0이면 어느 밤에도 한 병상에 두 환자가 배정되지 않습니다."
    )


class FemaleInMaleRoomCost(Constraint):
    """Nights a female patient spends in a MALE_ONLY room. Default: hard, weight 50."""

    name = "female_in_male_room_cost"
    label_en = "Female patient in a male-only room"
    label_ko = "남성 전용 병실의 여성 환자"
    description_en = (
        "The nights female patients spend in rooms limited to men. "
        "One unit of cost is one such night; 0 means no female patient is in a male-only room."
    )
    description_ko = (
        "여성 환자가 남성 전용 병실에서 지내는 밤을 모두 더합니다. "
        "그런 밤 하루가 비용 1이며, 0이면 남성 전용 병실에 여성 환자가 없습니다."
    )


class MaleInFemaleRoomCost(Constraint):
    """Nights a male patient spends in a FEMALE_ONLY room. Default: hard, weight 50."""

    name = "male_in_female_room_cost"
    label_en = "Male patient in a female-only room"
    label_ko = "여성 전용 병실의 남성 환자"
    description_en = (
        "The nights male patients spend in rooms limited to women. "
        "One unit of cost is one such night; 0 means no male patient is in a female-only room."
    )
    description_ko = (
        "남성 환자가 여성 전용 병실에서 지내는 밤을 모두 더합니다. "
        "그런 밤 하루가 비용 1이며, 0이면 여성 전용 병실에 남성 환자가 없습니다."
    )


class MixedGenderRoomCost(Constraint):
    """Nights in which men and women share a SAME_GENDER room. Default: hard, weight 1000.

    The pairing is per room while the calculator's unit is the bed, so this is a global constraint."""

    name = "mixed_gender_room_cost"
    label_en = "Mixed genders in a same-gender room"
    label_ko = "동성 병실의 혼성 배정"
    description_en = (
        "Nights on which a male and a female patient share a same-gender room, summed over every "
        "such pair of stays. One unit of cost is one shared night; 0 means every same-gender room "
        "holds patients of one gender on each night."
    )
    description_ko = (
        "동성 병실에서 남성 환자와 여성 환자가 함께 지내는 밤을 그런 입원 쌍마다 더합니다. "
        "함께 지내는 밤 하루가 비용 1이며, 0이면 모든 동성 병실이 밤마다 한 성별의 환자만 받습니다."
    )


class DepartmentMinAgeCost(Constraint):
    """Nights a patient below the department minimum age spends there. Default: hard, weight 100."""

    name = "department_min_age_cost"
    label_en = "Below the department's minimum age"
    label_ko = "진료과 최소 연령 미달"
    description_en = (
        "The nights patients younger than their department's minimum age spend there "
        "(a department without a minimum age never counts). "
        "One unit of cost is one such night; 0 means every patient is old enough for their department."
    )
    description_ko = (
        "진료과의 최소 연령보다 어린 환자가 그 진료과에서 지내는 밤을 모두 더합니다"
        "(최소 연령이 없는 진료과는 세지 않습니다). "
        "그런 밤 하루가 비용 1이며, 0이면 모든 환자가 진료과의 최소 연령을 충족합니다."
    )


class DepartmentMaxAgeCost(Constraint):
    """Nights a patient above the department maximum age spends there. Default: hard, weight 100."""

    name = "department_max_age_cost"
    label_en = "Above the department's maximum age"
    label_ko = "진료과 최대 연령 초과"
    description_en = (
        "The nights patients older than their department's maximum age spend there "
        "(a department without a maximum age never counts). "
        "One unit of cost is one such night; 0 means every patient is young enough for their department."
    )
    description_ko = (
        "진료과의 최대 연령보다 나이가 많은 환자가 그 진료과에서 지내는 밤을 모두 더합니다"
        "(최대 연령이 없는 진료과는 세지 않습니다). "
        "그런 밤 하루가 비용 1이며, 0이면 모든 환자가 진료과의 최대 연령을 넘지 않습니다."
    )


class RequiredPatientEquipmentCost(Constraint):
    """Room assignments missing required equipment. Default: hard, weight 50.

    If the room lacks any required equipment, it is valued by the number of required
    equipment items the room **has** — exactly the semantics pinned by the quickstart's
    unit test (so a room with none of the required equipment costs 0). Kept as is in 0.2.0."""

    name = "required_patient_equipment_cost"
    label_en = "Required equipment"
    label_ko = "필수 장비"
    description_en = (
        "For a stay whose room lacks at least one required equipment item: its nights times the number "
        "of required items the room does have (the quickstart's rule, so a room with none of the "
        "required items also costs 0). 0 means every stay's room has all of its required equipment, or none of it."
    )
    description_ko = (
        "필수 장비가 하나라도 빠진 병실에 배정된 입원마다, 재원 밤 수에 그 병실이 갖춘 필수 장비 수를 곱해 더합니다"
        "(퀵스타트 규칙이라 필수 장비가 하나도 없는 병실도 0입니다). "
        "0이면 모든 입원의 병실이 필수 장비를 모두 갖추었거나 하나도 갖추지 않았습니다."
    )


class UnassignedStaysCost(Constraint):
    """Nights spent on the "no bed" sentinel. Default: medium, weight 1 (medium units).

    The domain folds medium into the engine soft level as ``medium * 10^9``; the fold
    belongs to the domain (every ``constraint_costs()`` entry carries it as
    ``medium_fold``), so the weight carries no 10^9 factor."""

    name = "unassigned_stays_cost"
    label_en = "Unassigned stays"
    label_ko = "병상 미배정 입원"
    description_en = (
        'The nights of the stays left without a bed (the solver\'s "no bed" value). '
        "One unit of cost is one night without a bed; 0 means every stay has a bed."
    )
    description_ko = (
        "병상을 배정받지 못한 입원(솔버의 '병상 없음' 값)의 재원 밤을 모두 더합니다. "
        "병상 없이 보내는 밤 하루가 비용 1이며, 0이면 모든 입원이 병상을 배정받았습니다."
    )


class PreferredMaxRoomCapacityCost(Constraint):
    """Nights spent in a room larger than the preferred capacity. Default: soft, weight 8."""

    name = "preferred_max_room_capacity_cost"
    label_en = "Preferred room size"
    label_ko = "선호 병실 규모"
    description_en = (
        "The nights patients spend in a room with a larger capacity than the most they prefer "
        "(a patient without a preference never counts). "
        "One unit of cost is one such night; 0 means every patient's room is small enough."
    )
    description_ko = (
        "환자가 선호하는 최대 인원보다 정원이 큰 병실에서 지내는 밤을 모두 더합니다"
        "(선호가 없는 환자는 세지 않습니다). "
        "그런 밤 하루가 비용 1이며, 0이면 모든 환자의 병실이 선호 규모 이내입니다."
    )


class DepartmentSpecialtyCost(Constraint):
    """Nights spent in a department that does not offer the specialty at all. Default: soft, weight 10."""

    name = "department_specialty_cost"
    label_en = "Department specialty"
    label_ko = "진료과 전문과 불일치"
    description_en = (
        "The nights stays spend in a department that does not offer their specialty. "
        "One unit of cost is one such night; 0 means every stay is in a department offering its specialty."
    )
    description_ko = (
        "입원의 전문과를 다루지 않는 진료과에서 지내는 밤을 모두 더합니다. "
        "그런 밤 하루가 비용 1이며, 0이면 모든 입원이 자기 전문과를 다루는 진료과에 있습니다."
    )


class DepartmentSpecialtyPriorityCost(Constraint):
    """Nights spent in a department offering the specialty only at priority > 1. Default: soft, weight 10.

    Valued by (priority - 1). A department not offering the specialty at all is already
    valued by `department_specialty_cost`, so this stays silent."""

    name = "department_specialty_priority_cost"
    label_en = "Department specialty priority"
    label_ko = "진료과 전문과 우선순위"
    description_en = (
        "For a stay in a department that offers its specialty at priority p > 1: (p - 1) times its nights. "
        "0 means every stay is in a department with its specialty at priority 1 "
        "(a department without the specialty is counted by department_specialty_cost instead)."
    )
    description_ko = (
        "입원의 전문과를 우선순위 p > 1로 다루는 진료과에 있으면 (p - 1)에 재원 밤 수를 곱해 더합니다. "
        "0이면 모든 입원이 자기 전문과를 1순위로 다루는 진료과에 있습니다"
        "(전문과를 다루지 않는 진료과는 department_specialty_cost가 셉니다)."
    )


class PreferredPatientEquipmentCost(Constraint):
    """Nights spent in a room missing preferred equipment. Default: soft, weight 50.

    Valued by the number of preferred equipment items the room lacks."""

    name = "preferred_patient_equipment_cost"
    label_en = "Preferred equipment"
    label_ko = "선호 장비"
    description_en = (
        "For each stay: its nights times the number of preferred equipment items its room lacks. "
        "One unit of cost is one missing item for one night; 0 means every stay's room has all of its "
        "preferred equipment."
    )
    description_ko = (
        "입원마다 재원 밤 수에 병실에 없는 선호 장비 수를 곱해 더합니다. "
        "하룻밤 동안 빠진 선호 장비 하나가 비용 1이며, 0이면 모든 입원의 병실이 선호 장비를 모두 갖추었습니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
bed_conflict_cost = BedConflictCost()
female_in_male_room_cost = FemaleInMaleRoomCost()
male_in_female_room_cost = MaleInFemaleRoomCost()
mixed_gender_room_cost = MixedGenderRoomCost()
department_min_age_cost = DepartmentMinAgeCost()
department_max_age_cost = DepartmentMaxAgeCost()
required_patient_equipment_cost = RequiredPatientEquipmentCost()
unassigned_stays_cost = UnassignedStaysCost()
preferred_max_room_capacity_cost = PreferredMaxRoomCapacityCost()
department_specialty_cost = DepartmentSpecialtyCost()
department_specialty_priority_cost = DepartmentSpecialtyPriorityCost()
preferred_patient_equipment_cost = PreferredPatientEquipmentCost()

#: Default set — same order as `ba.list_constraints()`. The assembly lists unit
#: constraints first and global constraints after, so the global
#: `mixed_gender_room_cost` comes last (unlike the emplace order of C++
#: `defaultConstraints()`).
_DEFAULTS = (
    bed_conflict_cost,
    female_in_male_room_cost,
    male_in_female_room_cost,
    department_min_age_cost,
    department_max_age_cost,
    required_patient_equipment_cost,
    unassigned_stays_cost,
    preferred_max_room_capacity_cost,
    department_specialty_cost,
    department_specialty_priority_cost,
    preferred_patient_equipment_cost,
    mixed_gender_room_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all twelve default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([bed_conflict_cost, DepartmentSpecialtyCost(weight=40)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) and a constraint named twice raise ValueError.
    """
    return select(constraints, _DEFAULTS)


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
    engine; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.

    The returned value is the **engine-level** (hard, soft): the domain's medium
    fold (``medium_fold`` on every entry, into soft at 10^9) is applied, so it
    equals ``(score.hard, score.medium * 10**9 + score.soft)`` of a plan scored by
    the same constraints.
    """
    return score_costs(costs, get_constraints(constraints))
