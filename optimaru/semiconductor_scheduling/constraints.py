"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (``solve(problem)``) scores all 20 constraints at their default weights.
This module is the alternative path next to it: pick constraints, change weights, change
levels, shape the cost, and pass the list to ``solve(problem, constraints=...)`` or
``calculate_score(problem, constraints=...)``. A constraint list reads one way: one class
call per constraint::

    import optimaru.semiconductor_scheduling as fab
    from optimaru.semiconductor_scheduling.constraints import (
        MachineEligibilityCost, MakespanCost, WeightedTardinessCost, get_constraints,
    )

    problem = fab.solve(problem, constraints=get_constraints())                       # defaults as-is
    problem = fab.solve(problem, constraints=[MachineEligibilityCost(), MakespanCost(),
                                              WeightedTardinessCost(weight=50)])      # picked

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``makespan_cost`` is
``MakespanCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component default
               (every constraint 1, WeightedTardinessCost 10); ``constraint_info()``
               reports them. The problem carries no weights.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
- ``exponent`` sign-preserving power shaping of the constraint's TOTAL raw cost:
               ``weight * total ** exponent`` (every constraint is global, so the
               total is shaped, not each item — a lot, a pair, a window).
- ``offset``   (keyword) shifts that shaping of the total; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``max(0, total - offset) ** exponent``; ``False`` shapes
               ``total - offset`` as it is, so a total below the offset — a raw 0
               included — lowers the penalty. With an offset it is refused on
               hard (the 14 constraints that are hard by default, or a soft one
               moved to hard), because hard 0 must mean no violation.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs (its level included) and the picks add up. The makespan plus a steep
penalty on every minute beyond 480::

    problem = fab.solve(problem, constraints=[
        *(spec for spec in get_constraints() if spec.name != "makespan_cost"),
        MakespanCost(),
        MakespanCost(weight=100, offset=480),
    ])

``fab.constraint_costs(problem, picks)`` keys the second pick ``"makespan_cost#2"``.

Names (``name``) are the C++ component names in snake_case
(``MachineEligibilityCost`` -> ``machine_eligibility_cost``); check them with
``fab.list_constraints()``.
"""

from .._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "AuxiliaryResourceCapacityCost",
    "AuxiliaryResourceUnavailabilityCost",
    "BatchCapacityCost",
    "BatchFamilyConflictCost",
    "Constraint",
    "CycleTimeCost",
    "Level",
    "LotRoutePrecedenceCost",
    "MachineCapacityCost",
    "MachineEligibilityCost",
    "MachineLoadImbalanceCost",
    "MachinePreferenceCost",
    "MachineUnavailabilityCost",
    "MakespanCost",
    "MaxCycleTimeCost",
    "MaxQueueTimeCost",
    "MinBatchSizeCost",
    "MinQueueTimeCost",
    "MissingSetupTimeCost",
    "MissingTransportTimeCost",
    "PinnedAssignmentsCost",
    "Score",
    "WeightedTardinessCost",
    "auxiliary_resource_capacity_cost",
    "auxiliary_resource_unavailability_cost",
    "batch_capacity_cost",
    "batch_family_conflict_cost",
    "cycle_time_cost",
    "get_constraints",
    "lot_route_precedence_cost",
    "machine_capacity_cost",
    "machine_eligibility_cost",
    "machine_load_imbalance_cost",
    "machine_preference_cost",
    "machine_unavailability_cost",
    "makespan_cost",
    "max_cycle_time_cost",
    "max_queue_time_cost",
    "min_batch_size_cost",
    "min_queue_time_cost",
    "missing_setup_time_cost",
    "missing_transport_time_cost",
    "pinned_assignments_cost",
    "score",
    "weighted_tardiness_cost",
]


class MachineEligibilityCost(Constraint):
    """Operations without an assignment on an eligible machine. Default: hard, weight 1."""

    name = "machine_eligibility_cost"
    label_en = "Eligible equipment"
    label_ko = "적격 설비"
    description_en = (
        "Counts every operation that has no assignment, runs on a machine none of its options names, "
        "or runs longer or shorter than that option's duration. One unit is one such operation; "
        "0 means every operation runs on an eligible tool for exactly its duration."
    )
    description_ko = (
        "배정되지 않았거나, 어느 선택지에도 없는 설비에서 처리되거나, 처리 시간이 선택지의 처리 시간과 다른 공정을 셉니다. "
        "그런 공정 하나가 비용 1이며, 0이면 모든 공정이 적격 설비에서 정해진 처리 시간 동안 처리됩니다."
    )


class LotRoutePrecedenceCost(Constraint):
    """Route steps that start before their release or their previous step's end. Default: hard, weight 1."""

    name = "lot_route_precedence_cost"
    label_en = "Lot route precedence"
    label_ko = "로트 공정 순서"
    description_en = (
        "The time a lot's first step starts before the lot's release, plus the time each route step "
        "starts before the previous step ends. One unit is one time unit too early; 0 means every lot "
        "starts after its release and follows its route in order."
    )
    description_ko = (
        "첫 공정이 로트 투입 시각보다 일찍 시작한 시간과, 각 공정이 앞 공정이 끝나기 전에 시작한 시간을 더합니다. "
        "앞당겨진 시간 1단위가 비용 1이며, 0이면 모든 로트가 투입 후 공정 순서대로 진행됩니다."
    )


class MachineCapacityCost(Constraint):
    """Overlapping processing time on unary tools. Default: hard, weight 1."""

    name = "machine_capacity_cost"
    label_en = "Unary equipment capacity"
    label_ko = "단일 처리 설비 용량"
    description_en = (
        "On every tool that processes one lot at a time, the time during which more than one lot runs, "
        "weighted by how many lots are too many. One unit is one lot-time unit of overload; 0 means no "
        "two lots overlap on a unary tool."
    )
    description_ko = (
        "한 번에 한 로트만 처리하는 설비마다 둘 이상의 로트가 겹쳐 처리되는 시간에 초과 로트 수를 곱해 더합니다. "
        "로트 하나가 시간 1단위만큼 초과하면 비용 1이며, 0이면 단일 처리 설비에서 겹치는 로트가 없습니다."
    )


class BatchCapacityCost(Constraint):
    """Capacity-time above a batch tool's simultaneous lot limit. Default: hard, weight 1."""

    name = "batch_capacity_cost"
    label_en = "Batch capacity"
    label_ko = "배치 용량"
    description_en = (
        "On every batch tool, the time during which it runs more lots than its batch capacity, weighted "
        "by the excess. One unit is one lot-time unit above capacity; 0 means no batch tool is overfilled."
    )
    description_ko = (
        "배치 설비마다 배치 용량보다 많은 로트를 처리하는 시간에 초과 로트 수를 곱해 더합니다. "
        "로트 하나가 시간 1단위만큼 용량을 넘으면 비용 1이며, 0이면 용량을 넘긴 배치 설비가 없습니다."
    )


class BatchFamilyConflictCost(Constraint):
    """Overlap time of different recipe families on one batch tool. Default: hard, weight 1."""

    name = "batch_family_conflict_cost"
    label_en = "Batch recipe-family conflict"
    label_ko = "배치 레시피 계열 충돌"
    description_en = (
        "On every batch tool, the time two lots of different recipe families run at the same time, summed "
        "over the overlapping pairs. One unit is one time unit of mixed families; 0 means every batch run "
        "holds one family."
    )
    description_ko = (
        "배치 설비마다 레시피 계열이 다른 두 로트가 동시에 처리되는 시간을 겹치는 쌍마다 더합니다. "
        "서로 다른 계열이 함께 처리된 시간 1단위가 비용 1이며, 0이면 모든 배치에 한 계열만 들어갑니다."
    )


class MissingSetupTimeCost(Constraint):
    """Sequence-dependent setup time missing between consecutive runs. Default: hard, weight 1."""

    name = "missing_setup_time_cost"
    label_en = "Missing setup time"
    label_ko = "부족한 셋업 시간"
    description_en = (
        "Between consecutive runs on a machine that need a setup for their recipe change, the part of the "
        "setup time that does not fit before the next run starts. One unit is one time unit of missing "
        "setup; 0 means every required setup fits."
    )
    description_ko = (
        "한 설비에서 레시피가 바뀌어 셋업이 필요한 연속 작업 사이에, 다음 작업 시작 전에 들어가지 못한 셋업 시간을 더합니다. "
        "모자란 셋업 시간 1단위가 비용 1이며, 0이면 필요한 셋업 시간이 모두 확보됩니다."
    )


class MachineUnavailabilityCost(Constraint):
    """Processing time inside machine downtime windows. Default: hard, weight 1."""

    name = "machine_unavailability_cost"
    label_en = "Equipment unavailability"
    label_ko = "설비 사용 불가 시간"
    description_en = (
        "The processing time that falls inside a machine's unavailable windows (preventive maintenance or "
        "other downtime). One unit is one time unit of work during downtime; 0 means nothing runs while its "
        "machine is down."
    )
    description_ko = (
        "설비의 사용 불가 시간대(예방 정비나 기타 정지) 안에 걸친 처리 시간을 더합니다. "
        "정지 중 처리 시간 1단위가 비용 1이며, 0이면 정지 중인 설비에서 처리되는 작업이 없습니다."
    )


class MaxQueueTimeCost(Constraint):
    """Waiting time beyond a route step's maximum queue time. Default: hard, weight 1."""

    name = "max_queue_time_cost"
    label_en = "Maximum queue time"
    label_ko = "최대 대기 시간"
    description_en = (
        "After every route step with a maximum queue time, the wait before the next step starts beyond that "
        "limit. One unit is one time unit of excess waiting; 0 means every lot moves on in time."
    )
    description_ko = (
        "최대 대기 시간이 정해진 공정마다, 다음 공정이 시작되기까지 기다린 시간 중 한도를 넘는 부분을 더합니다. "
        "초과 대기 시간 1단위가 비용 1이며, 0이면 모든 로트가 제때 다음 공정으로 넘어갑니다."
    )


class MinQueueTimeCost(Constraint):
    """Minimum hold time missing between consecutive route steps. Default: hard, weight 1."""

    name = "min_queue_time_cost"
    label_en = "Minimum queue time"
    label_ko = "최소 대기 시간"
    description_en = (
        "After every route step, the part of its minimum hold time that the lot did not wait before the next "
        "step started (with no minimum, an overlap of the two steps counts too). One unit is one time unit "
        "short; 0 means every hold time is respected."
    )
    description_ko = (
        "공정마다 다음 공정 시작 전에 채우지 못한 최소 대기 시간을 더합니다(최소 대기가 없어도 두 공정이 겹치면 그만큼 셉니다). "
        "모자란 대기 시간 1단위가 비용 1이며, 0이면 모든 대기 시간이 지켜집니다."
    )


class MissingTransportTimeCost(Constraint):
    """Material-handling time missing between consecutive route steps. Default: hard, weight 1."""

    name = "missing_transport_time_cost"
    label_en = "Missing transport time"
    label_ko = "부족한 반송 시간"
    description_en = (
        "Between consecutive route steps, the part of the transport time between their equipment groups that "
        "does not fit into the wait (without a transport entry, an overlap of the two steps counts too). One "
        "unit is one time unit short; 0 means every transfer fits."
    )
    description_ko = (
        "연속한 두 공정 사이에서 설비 그룹 간 반송 시간 중 대기 시간 안에 들어가지 못한 부분을 더합니다(반송 시간 항목이 없어도 두 공정이 겹치면 그만큼 셉니다). "
        "모자란 반송 시간 1단위가 비용 1이며, 0이면 모든 반송이 시간 안에 이뤄집니다."
    )


class AuxiliaryResourceCapacityCost(Constraint):
    """Concurrent use above an auxiliary resource's capacity. Default: hard, weight 1."""

    name = "auxiliary_resource_capacity_cost"
    label_en = "Auxiliary resource capacity"
    label_ko = "보조 자원 용량"
    description_en = (
        "For every operator, reticle, fixture or transporter, the time during which more operations use it "
        "than its capacity, weighted by the excess (an operation listing a resource twice uses it twice). "
        "One unit is one use-time unit above capacity; 0 means no resource is over-used."
    )
    description_ko = (
        "작업자·레티클·지그·반송기마다 용량보다 많은 공정이 쓰는 시간에 초과 사용 수를 곱해 더합니다(한 공정이 같은 자원을 두 번 지정하면 두 번 쓰는 것으로 셉니다). "
        "사용 하나가 시간 1단위만큼 용량을 넘으면 비용 1이며, 0이면 용량을 넘겨 쓰인 자원이 없습니다."
    )


class AuxiliaryResourceUnavailabilityCost(Constraint):
    """Use of an auxiliary resource inside its unavailable windows. Default: hard, weight 1."""

    name = "auxiliary_resource_unavailability_cost"
    label_en = "Auxiliary resource unavailability"
    label_ko = "보조 자원 사용 불가 시간"
    description_en = (
        "The time operations use an auxiliary resource inside its unavailable windows. One unit is one "
        "time unit of use while unavailable; 0 means no resource is used while it is unavailable."
    )
    description_ko = (
        "공정이 보조 자원의 사용 불가 시간대 안에서 그 자원을 쓴 시간을 더합니다. "
        "사용 불가 중 사용 시간 1단위가 비용 1이며, 0이면 사용 불가 시간대에 쓰인 자원이 없습니다."
    )


class PinnedAssignmentsCost(Constraint):
    """Frozen operations that moved off their pinned machine or time. Default: hard, weight 1."""

    name = "pinned_assignments_cost"
    label_en = "Pinned assignments"
    label_ko = "고정 배정"
    description_en = (
        "Counts the pinned assignments whose operation is missing or runs on another machine, start or end. "
        "One unit is one moved pin; 0 means every pin is kept."
    )
    description_ko = (
        "공정이 배정되지 않았거나 설비, 시작 시각, 종료 시각 중 하나라도 달라진 고정 배정을 셉니다. "
        "어긋난 고정 배정 하나가 비용 1이며, 0이면 모든 고정 배정이 지켜집니다."
    )


class MaxCycleTimeCost(Constraint):
    """Lot cycle time beyond its maximum. Default: hard, weight 1."""

    name = "max_cycle_time_cost"
    label_en = "Maximum cycle time"
    label_ko = "최대 사이클 타임"
    description_en = (
        "For every lot with a maximum cycle time and a completed route, the time from release to completion "
        "beyond that maximum. One unit is one time unit over; 0 means every lot completes within its maximum."
    )
    description_ko = (
        "최대 사이클 타임이 있고 공정을 모두 마친 로트마다, 투입부터 완료까지 걸린 시간 중 한도를 넘는 부분을 더합니다. "
        "초과 시간 1단위가 비용 1이며, 0이면 모든 로트가 한도 안에 완료됩니다."
    )


class MachinePreferenceCost(Constraint):
    """Preference cost of the chosen equipment. Default: soft, weight 1."""

    name = "machine_preference_cost"
    label_en = "Equipment preference"
    label_ko = "설비 선호도"
    description_en = (
        "The qualification, yield or operating preference cost of the machine option each operation runs "
        "on. One unit is one unit of preference cost; 0 means only zero-cost tools are used."
    )
    description_ko = (
        "공정마다 실제로 쓴 설비 선택지의 인증·수율·운영 선호 비용을 더합니다. "
        "선호 비용 1이 비용 1이며, 0이면 선호 비용이 0인 설비만 사용합니다."
    )


class MinBatchSizeCost(Constraint):
    """Lots missing below a batch tool's preferred minimum run size. Default: soft, weight 1."""

    name = "min_batch_size_cost"
    label_en = "Batch below minimum size"
    label_ko = "최소 배치 크기 미달"
    description_en = (
        "For every batch run (one recipe family and batch id on a batch tool), the lots missing to reach the "
        "tool's minimum batch size. One unit is one missing lot; 0 means every batch run is full enough."
    )
    description_ko = (
        "배치 설비의 배치 작업(같은 레시피 계열·배치 ID)마다 최소 배치 크기에 모자란 로트 수를 더합니다. "
        "모자란 로트 하나가 비용 1이며, 0이면 모든 배치가 최소 크기를 채웁니다."
    )


class MachineLoadImbalanceCost(Constraint):
    """Processing-load range between the tools of each equipment group. Default: soft, weight 1."""

    name = "machine_load_imbalance_cost"
    label_en = "Equipment-group load imbalance"
    label_ko = "설비 그룹 부하 불균형"
    description_en = (
        "In every equipment group with two or more machines, the difference between the most and the least "
        "loaded machine, a load being the summed processing time. One unit is one time unit of difference; "
        "0 means every group is evenly loaded."
    )
    description_ko = (
        "설비가 둘 이상인 설비 그룹마다 부하(처리 시간 합)가 가장 큰 설비와 가장 작은 설비의 차이를 더합니다. "
        "차이 시간 1단위가 비용 1이며, 0이면 모든 그룹에서 설비 부하가 같습니다."
    )


class WeightedTardinessCost(Constraint):
    """Lot tardiness multiplied by its priority. Default: soft, weight 10."""

    name = "weighted_tardiness_cost"
    label_en = "Weighted lot tardiness"
    label_ko = "가중 로트 납기 지연"
    description_en = (
        "For every lot with a completed route, the time its completion passes its due time, multiplied by "
        "the lot's priority. One unit is one priority-weighted time unit late; 0 means every lot is on time."
    )
    description_ko = (
        "공정을 모두 마친 로트마다 완료 시각이 납기를 넘긴 시간에 로트 우선순위를 곱해 더합니다. "
        "우선순위를 곱한 지연 시간 1단위가 비용 1이며, 0이면 모든 로트가 납기를 지킵니다."
    )


class CycleTimeCost(Constraint):
    """Elapsed time from lot release to completion. Default: soft, weight 1."""

    name = "cycle_time_cost"
    label_en = "Total cycle time"
    label_ko = "총 사이클 타임"
    description_en = (
        "For every lot with a completed route, the time from its release to its completion, summed. One "
        "unit is one time unit; 0 means no lot has completed its route."
    )
    description_ko = (
        "공정을 모두 마친 로트마다 투입부터 완료까지 걸린 시간을 더합니다. "
        "시간 1단위가 비용 1이며, 0이면 공정을 마친 로트가 없습니다."
    )


class MakespanCost(Constraint):
    """Completion time of the last lot. Default: soft, weight 1."""

    name = "makespan_cost"
    label_en = "Makespan"
    label_ko = "메이크스팬"
    description_en = (
        "The latest completion time among the lots that completed their route. One unit is one time unit; "
        "0 means no lot has completed its route."
    )
    description_ko = (
        "공정을 모두 마친 로트 가운데 가장 늦은 완료 시각입니다. "
        "시간 1단위가 비용 1이며, 0이면 공정을 마친 로트가 없습니다."
    )


#: Ready-made default instances (``makespan_cost`` is ``MakespanCost()``).
machine_eligibility_cost = MachineEligibilityCost()
lot_route_precedence_cost = LotRoutePrecedenceCost()
machine_capacity_cost = MachineCapacityCost()
batch_capacity_cost = BatchCapacityCost()
batch_family_conflict_cost = BatchFamilyConflictCost()
missing_setup_time_cost = MissingSetupTimeCost()
machine_unavailability_cost = MachineUnavailabilityCost()
max_queue_time_cost = MaxQueueTimeCost()
min_queue_time_cost = MinQueueTimeCost()
missing_transport_time_cost = MissingTransportTimeCost()
auxiliary_resource_capacity_cost = AuxiliaryResourceCapacityCost()
auxiliary_resource_unavailability_cost = AuxiliaryResourceUnavailabilityCost()
pinned_assignments_cost = PinnedAssignmentsCost()
max_cycle_time_cost = MaxCycleTimeCost()
machine_preference_cost = MachinePreferenceCost()
min_batch_size_cost = MinBatchSizeCost()
machine_load_imbalance_cost = MachineLoadImbalanceCost()
weighted_tardiness_cost = WeightedTardinessCost()
cycle_time_cost = CycleTimeCost()
makespan_cost = MakespanCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    machine_eligibility_cost,
    lot_route_precedence_cost,
    machine_capacity_cost,
    batch_capacity_cost,
    batch_family_conflict_cost,
    missing_setup_time_cost,
    machine_unavailability_cost,
    max_queue_time_cost,
    min_queue_time_cost,
    missing_transport_time_cost,
    auxiliary_resource_capacity_cost,
    auxiliary_resource_unavailability_cost,
    pinned_assignments_cost,
    max_cycle_time_cost,
    machine_preference_cost,
    min_batch_size_cost,
    machine_load_imbalance_cost,
    weighted_tardiness_cost,
    cycle_time_cost,
    makespan_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all 20 default constraints.

    When `constraints` is given, that list is used as-is — only the listed constraints are
    scored. Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is scored once
    per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what ``fab.constraint_costs(problem)`` returns: per constraint its raw
    total (one element: every constraint is global), its default weight and level. Fetch it
    once and rescore with different weights without re-solving::

        raw = fab.constraint_costs(problem)
        score(raw)                                              # default set
        score(raw, [MachineEligibilityCost(), WeightedTardinessCost(weight=50)])
        score(raw, [MakespanCost(), MakespanCost(weight=100, offset=480)])

    The arithmetic matches the engine: ``weight * shape(total, exponent, offset, clip)`` per
    constraint, hard contributions rounded per constraint (half away from zero). A second
    pick of a constraint reads the same raw total with its own knobs. Unknown names raise
    ValueError, and so does ``level="medium"`` (no medium level here).
    """
    return score_costs(costs, get_constraints(constraints))
