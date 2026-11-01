"""Class-based CP-SAT plugins for semiconductor fab scheduling.

Each plugin subclasses the domain spec of the same name; its default level and weight come
from the engine's ``semiconductor_scheduling.constraint_info()`` (the C++ components'
kDefaultLevel / kDefaultWeight), so they are not restated here. The registry order is the
``list_constraints()`` order.

Two kinds of plugin, one semantics (the engine's raw cost; 0 = no violation):

- STRUCTURAL (12): the rule is posted as model constraints whenever the constraint is
  selected, so every CP-SAT solution has raw cost 0 for it and the objective term is 0.
  These plugins refuse a selection the model cannot honour: ``level="soft"`` or a weight
  other than None/1 (ValueError). MachineEligibilityCost is the variable layout itself:
  every operation runs exactly one eligible option (for a machine listed twice, the LAST
  option, the one the engine scores), so an ineligible assignment cannot be represented.
- PENALIZED (8): MaxQueueTimeCost and MaxCycleTimeCost (hard) and the six soft terms add
  their exact raw cost to the objective, on the level the selection asks for.

Where CP-SAT is stricter than the engine (documented, never laxer): MissingSetupTimeCost is
posted for every pair of runs on a machine, not only consecutive ones, so setup matrices
that break the triangle inequality may exclude schedules the engine scores 0.
"""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from semiconductor_scheduling import constraints as domain
from semiconductor_scheduling.models import SemiconductorScheduleModel


@dataclass(frozen=True)
class Choice:
    """One eligible option of one operation row, as model variables."""

    row: int
    machine_id: str
    duration: int
    preference_cost: int
    present: Any
    interval: Any


@dataclass(frozen=True)
class FabContext:
    model: cp_model.CpModel
    problem: SemiconductorScheduleModel
    rows: list[tuple[int, int, Any, Any]]  # (lot index, route position, lot, operation)
    first_row: list[int]  # per lot
    starts: list[Any]
    ends: list[Any]
    choices: list[list[Choice]]  # per row, deduplicated by machine (last option wins)
    machine_by_id: dict[str, Any]
    setups: dict[tuple[str, str, str], int]
    transports: dict[tuple[str, str], int]
    horizon: int
    total_duration: int

    def on_machine(self, machine_id: str) -> list[Choice]:
        return [choice for options in self.choices for choice in options if choice.machine_id == machine_id]

    def family(self, row: int) -> str:
        return self.rows[row][3].family

    def route(self, lot: int) -> range:
        operations = self.problem.lots[lot].operations
        return range(self.first_row[lot], self.first_row[lot] + len(operations))


def _structural(plugin: ConstraintPlugin, objective: ObjectiveBuilder) -> bool:
    """Whether the structural plugin is selected; refuses what the model cannot honour."""
    selected = objective.selected.get(plugin.name)
    if selected is None:
        return False
    _spec, config = selected
    if config.level not in (None, "hard") or config.weight not in (None, 1):
        raise ValueError(f"{plugin.name}: the CP-SAT model enforces this constraint structurally (hard, weight 1)")
    return True


def _disjoint(
    model: cp_model.CpModel,
    a: Choice,
    b: Choice,
    ends: list,
    starts: list,
    gap_ab: int = 0,
    gap_ba: int = 0,
    name: str = "",
) -> None:
    """When both options are chosen: a before b with gap_ab, or b before a with gap_ba."""
    before = model.new_bool_var(name)
    model.add(ends[a.row] + gap_ab <= starts[b.row]).only_enforce_if([a.present, b.present, before])
    model.add(ends[b.row] + gap_ba <= starts[a.row]).only_enforce_if([a.present, b.present, before.negated()])


class MachineEligibilityCost(domain.MachineEligibilityCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        # The variable layout: exactly one eligible option per operation (solve.py).
        _structural(self, objective)


class LotRoutePrecedenceCost(domain.LotRoutePrecedenceCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for lot_index, lot in enumerate(context.problem.lots):
            route = context.route(lot_index)
            context.model.add(context.starts[route[0]] >= lot.release_time)
            for row in route[1:]:
                context.model.add(context.starts[row] >= context.ends[row - 1])


class MachineCapacityCost(domain.MachineCapacityCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for machine in context.problem.machines:
            if machine.batch_capacity == 1:
                intervals = [choice.interval for choice in context.on_machine(machine.id)]
                if intervals:
                    context.model.add_no_overlap(intervals)


class BatchCapacityCost(domain.BatchCapacityCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for machine in context.problem.machines:
            if machine.batch_capacity > 1:
                intervals = [choice.interval for choice in context.on_machine(machine.id)]
                if intervals:
                    context.model.add_cumulative(intervals, [1] * len(intervals), machine.batch_capacity)


class BatchFamilyConflictCost(domain.BatchFamilyConflictCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for machine in context.problem.machines:
            if machine.batch_capacity == 1:
                continue
            items = context.on_machine(machine.id)
            for i, a in enumerate(items):
                for b in items[i + 1 :]:
                    if context.family(a.row) != context.family(b.row):
                        _disjoint(
                            context.model,
                            a,
                            b,
                            context.ends,
                            context.starts,
                            name=f"{machine.id}_{a.row}_{b.row}_family",
                        )


class MissingSetupTimeCost(domain.MissingSetupTimeCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for machine in context.problem.machines:
            items = context.on_machine(machine.id)
            for i, a in enumerate(items):
                for b in items[i + 1 :]:
                    family_a, family_b = context.family(a.row), context.family(b.row)
                    if machine.batch_capacity > 1 and family_a == family_b:
                        continue
                    ab = context.setups.get((machine.group, family_a, family_b), 0)
                    ba = context.setups.get((machine.group, family_b, family_a), 0)
                    if ab or ba:
                        _disjoint(
                            context.model,
                            a,
                            b,
                            context.ends,
                            context.starts,
                            ab,
                            ba,
                            name=f"{machine.id}_{a.row}_{b.row}_setup",
                        )


class MachineUnavailabilityCost(domain.MachineUnavailabilityCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        model = context.model
        for machine in context.problem.machines:
            for choice in context.on_machine(machine.id):
                for k, window in enumerate(machine.unavailable):
                    before = model.new_bool_var(f"{machine.id}_{choice.row}_before_down_{k}")
                    model.add(context.ends[choice.row] <= window.start).only_enforce_if([choice.present, before])
                    model.add(context.starts[choice.row] >= window.end).only_enforce_if(
                        [choice.present, before.negated()]
                    )


class MaxQueueTimeCost(domain.MaxQueueTimeCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        excesses = []
        for lot_index, lot in enumerate(context.problem.lots):
            for row in context.route(lot_index)[1:]:
                limit = context.rows[row - 1][3].max_queue_time
                if limit is None:
                    continue
                excess = model.new_int_var(0, context.horizon, f"queue_excess_{row}")
                model.add_max_equality(excess, [0, context.starts[row] - context.ends[row - 1] - limit])
                excesses.append(excess)
        objective.add(self.name, sum(excesses), bound=len(excesses) * context.horizon)


class MinQueueTimeCost(domain.MinQueueTimeCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for lot_index, _lot in enumerate(context.problem.lots):
            for row in context.route(lot_index)[1:]:
                hold = context.rows[row - 1][3].min_queue_time
                context.model.add(context.starts[row] >= context.ends[row - 1] + hold)


class MissingTransportTimeCost(domain.MissingTransportTimeCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        model = context.model
        for lot_index, _lot in enumerate(context.problem.lots):
            for row in context.route(lot_index)[1:]:
                # A missing entry is a transport time of 0: still no overlap.
                model.add(context.starts[row] >= context.ends[row - 1])
                for previous in context.choices[row - 1]:
                    for current in context.choices[row]:
                        duration = context.transports.get(
                            (
                                context.machine_by_id[previous.machine_id].group,
                                context.machine_by_id[current.machine_id].group,
                            ),
                            0,
                        )
                        if duration:
                            model.add(context.starts[row] >= context.ends[row - 1] + duration).only_enforce_if(
                                [previous.present, current.present]
                            )


def _users(context: FabContext, resource_id: str) -> list[Choice]:
    """The options of every row needing the resource, once per listing."""
    users = []
    for row, (_lot, _position, _lot_model, operation) in enumerate(context.rows):
        for listed in operation.required_resources:
            if listed == resource_id:
                users.extend(context.choices[row])
    return users


class AuxiliaryResourceCapacityCost(domain.AuxiliaryResourceCapacityCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        for resource in context.problem.auxiliary_resources:
            intervals = [choice.interval for choice in _users(context, resource.id)]
            if intervals:
                context.model.add_cumulative(intervals, [1] * len(intervals), resource.capacity)


class AuxiliaryResourceUnavailabilityCost(domain.AuxiliaryResourceUnavailabilityCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        model = context.model
        for resource in context.problem.auxiliary_resources:
            for n, choice in enumerate(_users(context, resource.id)):
                for k, window in enumerate(resource.unavailable):
                    before = model.new_bool_var(f"{resource.id}_{n}_before_down_{k}")
                    model.add(context.ends[choice.row] <= window.start).only_enforce_if([choice.present, before])
                    model.add(context.starts[choice.row] >= window.end).only_enforce_if(
                        [choice.present, before.negated()]
                    )


class PinnedAssignmentsCost(domain.PinnedAssignmentsCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        if not _structural(self, objective):
            return
        row_of = {(lot.id, operation.id): row for row, (_l, _p, lot, operation) in enumerate(context.rows)}
        for pin in context.problem.pinned_assignments:
            row = row_of[(pin.lot_id, pin.operation_id)]
            choice = next(choice for choice in context.choices[row] if choice.machine_id == pin.machine_id)
            context.model.add(choice.present == 1)
            context.model.add(context.starts[row] == pin.start)
            context.model.add(context.ends[row] == pin.end)


class MaxCycleTimeCost(domain.MaxCycleTimeCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        excesses = []
        for lot_index, lot in enumerate(context.problem.lots):
            if lot.max_cycle_time is None:
                continue
            last = context.route(lot_index)[-1]
            excess = model.new_int_var(0, context.horizon, f"cycle_excess_{lot_index}")
            model.add_max_equality(excess, [0, context.ends[last] - lot.release_time - lot.max_cycle_time])
            excesses.append(excess)
        objective.add(self.name, sum(excesses), bound=len(excesses) * context.horizon)


class MachinePreferenceCost(domain.MachinePreferenceCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        terms = [choice.preference_cost * choice.present for options in context.choices for choice in options]
        bound = sum(max((choice.preference_cost for choice in options), default=0) for options in context.choices)
        objective.add(self.name, sum(terms), bound=bound)


class MinBatchSizeCost(domain.MinBatchSizeCost, ConstraintPlugin[FabContext]):
    """Groups of equal start and family on a batch tool (the engine groups by batch id, and
    the result's batch ids are given per (machine, family, start))."""

    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        shortfalls = []
        for machine in context.problem.machines:
            if machine.batch_capacity == 1 or machine.minimum_batch_size == 1:
                continue
            items = context.on_machine(machine.id)
            same = defaultdict(list)  # item index -> [(other index, together)]
            for i, a in enumerate(items):
                for j in range(i + 1, len(items)):
                    b = items[j]
                    if context.family(a.row) != context.family(b.row):
                        continue
                    together = model.new_bool_var(f"{machine.id}_{a.row}_{b.row}_together")
                    model.add(together <= a.present)
                    model.add(together <= b.present)
                    model.add(context.starts[a.row] == context.starts[b.row]).only_enforce_if(together)
                    # Both chosen and not together: different starts (a distinct batch).
                    model.add(context.starts[a.row] != context.starts[b.row]).only_enforce_if(
                        [a.present, b.present, together.negated()]
                    )
                    same[i].append((j, together))
                    same[j].append((i, together))
            for i, item in enumerate(items):
                earlier = [together for j, together in same[i] if j < i]
                leader = model.new_bool_var(f"{machine.id}_{item.row}_leader")
                model.add(leader <= item.present)
                model.add(leader >= item.present - sum(earlier))
                for together in earlier:
                    model.add(leader + together <= 1)
                size = item.present + sum(together for _j, together in same[i])
                shortfall = model.new_int_var(0, machine.minimum_batch_size, f"{machine.id}_{item.row}_shortfall")
                model.add(shortfall >= machine.minimum_batch_size * leader - size)
                shortfalls.append(shortfall)
        bound = sum(machine.minimum_batch_size for machine in context.problem.machines) * max(1, len(context.rows))
        objective.add(self.name, sum(shortfalls), bound=bound if shortfalls else 0)


class MachineLoadImbalanceCost(domain.MachineLoadImbalanceCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        groups = defaultdict(list)
        for machine in context.problem.machines:
            groups[machine.group].append(machine)
        ranges = []
        for group, machines in groups.items():
            if len(machines) < 2:
                continue
            loads = [
                sum(choice.duration * choice.present for choice in context.on_machine(machine.id))
                for machine in machines
            ]
            highest = model.new_int_var(0, context.total_duration, f"{group}_max_load")
            lowest = model.new_int_var(0, context.total_duration, f"{group}_min_load")
            model.add_max_equality(highest, loads)
            model.add_min_equality(lowest, loads)
            ranges.append(highest - lowest)
        objective.add(self.name, sum(ranges), bound=len(ranges) * context.total_duration)


class WeightedTardinessCost(domain.WeightedTardinessCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        terms = []
        for lot_index, lot in enumerate(context.problem.lots):
            tardiness = model.new_int_var(0, context.horizon, f"tardiness_{lot_index}")
            model.add_max_equality(tardiness, [0, context.ends[context.route(lot_index)[-1]] - lot.due_time])
            terms.append(lot.priority * tardiness)
        bound = sum(lot.priority for lot in context.problem.lots) * context.horizon
        objective.add(self.name, sum(terms), bound=bound)


class CycleTimeCost(domain.CycleTimeCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        model = context.model
        terms = []
        for lot_index, lot in enumerate(context.problem.lots):
            cycle = model.new_int_var(0, context.horizon, f"cycle_{lot_index}")
            model.add_max_equality(cycle, [0, context.ends[context.route(lot_index)[-1]] - lot.release_time])
            terms.append(cycle)
        objective.add(self.name, sum(terms), bound=len(terms) * context.horizon)


class MakespanCost(domain.MakespanCost, ConstraintPlugin[FabContext]):
    def build(self, context: FabContext, objective: ObjectiveBuilder) -> None:
        makespan = context.model.new_int_var(0, context.horizon, "makespan")
        context.model.add_max_equality(
            makespan, [context.ends[context.route(lot)[-1]] for lot in range(len(context.problem.lots))]
        )
        objective.add(self.name, makespan, bound=context.horizon)


REGISTRY = ConstraintRegistry(
    [
        MachineEligibilityCost(),
        LotRoutePrecedenceCost(),
        MachineCapacityCost(),
        BatchCapacityCost(),
        BatchFamilyConflictCost(),
        MissingSetupTimeCost(),
        MachineUnavailabilityCost(),
        MaxQueueTimeCost(),
        MinQueueTimeCost(),
        MissingTransportTimeCost(),
        AuxiliaryResourceCapacityCost(),
        AuxiliaryResourceUnavailabilityCost(),
        PinnedAssignmentsCost(),
        MaxCycleTimeCost(),
        MachinePreferenceCost(),
        MinBatchSizeCost(),
        MachineLoadImbalanceCost(),
        WeightedTardinessCost(),
        CycleTimeCost(),
        MakespanCost(),
    ]
)
