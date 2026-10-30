from __future__ import annotations

from collections import defaultdict
from itertools import pairwise

from ._core import SolverConfig as SolverConfig
from .common import anneal, configure
from .constraints import DEFAULTS, score
from .models import AssignmentModel, MachineModel, OperationModel, SemiconductorScheduleModel
from .models import with_initial_solution as with_initial_solution


def list_constraints() -> list[str]:
    return [constraint.name for constraint in DEFAULTS]


def _operation_rows(problem: SemiconductorScheduleModel):
    return [
        (lot, operation_index, operation)
        for lot in problem.lots
        for operation_index, operation in enumerate(lot.operations)
    ]


def _overlap(start_a: int, end_a: int, start_b: int, end_b: int) -> int:
    return max(0, min(end_a, end_b) - max(start_a, start_b))


def _overload_area(assignments: list[AssignmentModel], capacity: int) -> int:
    events: dict[int, int] = defaultdict(int)
    for assignment in assignments:
        events[assignment.start] += 1
        events[assignment.end] -= 1
    active = 0
    previous = None
    excess = 0
    for moment in sorted(events):
        if previous is not None:
            excess += max(0, active - capacity) * (moment - previous)
        active += events[moment]
        previous = moment
    return excess


def _setup_map(problem: SemiconductorScheduleModel) -> dict[tuple[str, str, str], int]:
    return {(setup.machine_group, setup.from_family, setup.to_family): setup.duration for setup in problem.setup_times}


def _transport_map(problem: SemiconductorScheduleModel) -> dict[tuple[str, str], int]:
    return {(item.from_group, item.to_group): item.duration for item in problem.transport_times}


def constraint_costs(problem: SemiconductorScheduleModel) -> dict[str, list[int]]:
    machine_by_id = {machine.id: machine for machine in problem.machines}
    transports = _transport_map(problem)
    operation_by_key = {
        (lot.id, operation.id): (lot, index, operation)
        for lot in problem.lots
        for index, operation in enumerate(lot.operations)
    }
    assignment_by_key = {(item.lot_id, item.operation_id): item for item in problem.assignments}
    assignments_by_machine: dict[str, list[AssignmentModel]] = defaultdict(list)
    assignments_by_resource: dict[str, list[AssignmentModel]] = defaultdict(list)
    for assignment in problem.assignments:
        assignments_by_machine[assignment.machine_id].append(assignment)
        operation_row = operation_by_key.get((assignment.lot_id, assignment.operation_id))
        if operation_row is not None:
            for resource_id in operation_row[2].required_resources:
                assignments_by_resource[resource_id].append(assignment)

    eligibility = []
    preferences = []
    for key, (_lot, _index, operation) in operation_by_key.items():
        assignment = assignment_by_key.get(key)
        option_by_machine = {option.machine_id: option for option in operation.eligible_machines}
        option = option_by_machine.get(assignment.machine_id) if assignment is not None else None
        eligibility.append(
            int(assignment is None or option is None or assignment.end - assignment.start != option.duration)
        )
        preferences.append(option.preference_cost if option is not None else 0)

    precedence = []
    queue_time = []
    minimum_queue_time = []
    transport_violations = []
    tardiness = []
    cycle_time = []
    completion_times = []
    for lot in problem.lots:
        lot_assignments = [assignment_by_key.get((lot.id, operation.id)) for operation in lot.operations]
        first = lot_assignments[0]
        if first is not None:
            precedence.append(max(0, lot.release_time - first.start))
        for index in range(1, len(lot.operations)):
            previous = lot_assignments[index - 1]
            current = lot_assignments[index]
            if previous is None or current is None:
                continue
            wait = current.start - previous.end
            precedence.append(max(0, -wait))
            previous_machine = machine_by_id[previous.machine_id]
            current_machine = machine_by_id[current.machine_id]
            required_transport = transports.get((previous_machine.group, current_machine.group), 0)
            transport_violations.append(max(0, required_transport - wait))
            limit = lot.operations[index - 1].max_queue_time
            if limit is not None:
                queue_time.append(max(0, wait - limit))
            minimum_queue_time.append(max(0, lot.operations[index - 1].min_queue_time - wait))
        last = lot_assignments[-1]
        if last is not None:
            completion_times.append(last.end)
            tardiness.append(max(0, last.end - lot.due_time) * lot.priority)
            cycle_time.append(max(0, last.end - lot.release_time))

    maximum_cycle_time = [
        max(0, assignment_by_key[(lot.id, lot.operations[-1].id)].end - lot.release_time - lot.max_cycle_time)
        for lot in problem.lots
        if lot.max_cycle_time is not None and (lot.id, lot.operations[-1].id) in assignment_by_key
    ]

    auxiliary_capacity = [
        _overload_area(assignments_by_resource[resource.id], resource.capacity)
        for resource in problem.auxiliary_resources
    ]
    auxiliary_availability = [
        _overlap(assignment.start, assignment.end, window.start, window.end)
        for resource in problem.auxiliary_resources
        for assignment in assignments_by_resource[resource.id]
        for window in resource.unavailable
    ]
    pinned = {(item.lot_id, item.operation_id): item for item in problem.pinned_assignments}
    pinned_violations = [
        int(
            actual is None
            or (actual.machine_id, actual.start, actual.end) != (expected.machine_id, expected.start, expected.end)
        )
        for key, expected in pinned.items()
        for actual in [assignment_by_key.get(key)]
    ]

    unary_capacity = []
    batch_capacity = []
    batch_compatibility = []
    maintenance = []
    setup_violations = []
    minimum_batch_fill = []
    setups = _setup_map(problem)
    for machine in problem.machines:
        rows = sorted(assignments_by_machine[machine.id], key=lambda item: (item.start, item.end, item.lot_id))
        if machine.batch_capacity == 1:
            unary_capacity.append(_overload_area(rows, 1))
        else:
            batch_capacity.append(_overload_area(rows, machine.batch_capacity))
            for left_index, left in enumerate(rows):
                left_operation = operation_by_key[(left.lot_id, left.operation_id)][2]
                for right in rows[left_index + 1 :]:
                    if right.start >= left.end:
                        break
                    right_operation = operation_by_key[(right.lot_id, right.operation_id)][2]
                    if left_operation.family != right_operation.family:
                        batch_compatibility.append(_overlap(left.start, left.end, right.start, right.end))
            batches: dict[tuple[str, str | int], list[AssignmentModel]] = defaultdict(list)
            for assignment in rows:
                operation = operation_by_key[(assignment.lot_id, assignment.operation_id)][2]
                batch_key: str | int = assignment.batch_id or assignment.start
                batches[(operation.family, batch_key)].append(assignment)
            minimum_batch_fill.extend(max(0, machine.minimum_batch_size - len(batch)) for batch in batches.values())
        for assignment in rows:
            maintenance.extend(
                _overlap(assignment.start, assignment.end, window.start, window.end) for window in machine.unavailable
            )
        for previous, current in pairwise(rows):
            previous_operation = operation_by_key[(previous.lot_id, previous.operation_id)][2]
            current_operation = operation_by_key[(current.lot_id, current.operation_id)][2]
            if machine.batch_capacity > 1 and previous_operation.family == current_operation.family:
                continue
            required = setups.get((machine.group, previous_operation.family, current_operation.family), 0)
            if required:
                setup_violations.append(max(0, previous.end + required - current.start))

    loads_by_machine = {
        machine.id: sum(item.end - item.start for item in assignments_by_machine[machine.id])
        for machine in problem.machines
    }
    groups: dict[str, list[int]] = defaultdict(list)
    for machine in problem.machines:
        groups[machine.group].append(loads_by_machine[machine.id])
    load_balance = [max(loads) - min(loads) for loads in groups.values() if len(loads) > 1]

    return {
        "machine_eligibility": eligibility,
        "lot_route_precedence": precedence,
        "machine_capacity": unary_capacity,
        "batch_capacity": batch_capacity,
        "batch_family_compatibility": batch_compatibility,
        "setup_transitions": setup_violations,
        "maintenance_windows": maintenance,
        "maximum_queue_time": queue_time,
        "minimum_queue_time": minimum_queue_time,
        "inter_tool_transport": transport_violations,
        "auxiliary_resource_capacity": auxiliary_capacity,
        "auxiliary_resource_availability": auxiliary_availability,
        "pinned_assignments": pinned_violations,
        "maximum_cycle_time": maximum_cycle_time,
        "machine_preference": preferences,
        "minimum_batch_fill": minimum_batch_fill,
        "machine_load_balance": load_balance,
        "weighted_tardiness": tardiness,
        "total_cycle_time": cycle_time,
        "makespan": [max(completion_times, default=0)],
    }


def calculate_score(problem: SemiconductorScheduleModel, constraints=None):
    costs = constraint_costs(problem)
    problem.makespan = costs["makespan"][0]
    problem.total_weighted_tardiness = sum(costs["weighted_tardiness"])
    problem.total_cycle_time = sum(costs["total_cycle_time"])
    result = score(costs, constraints)
    problem.score_hard, problem.score_soft = result
    return result


def _setup_duration(
    setup_by_key: dict[tuple[str, str, str], int], machine: MachineModel, before: str, after: str
) -> int:
    return setup_by_key.get((machine.group, before, after), 0)


def _advance_past_downtime(machine: MachineModel, start: int, duration: int) -> int:
    while True:
        conflict = next(
            (
                window
                for window in sorted(machine.unavailable, key=lambda item: item.start)
                if _overlap(start, start + duration, window.start, window.end)
            ),
            None,
        )
        if conflict is None:
            return start
        start = conflict.end


def _earliest_start(
    machine: MachineModel,
    operation: OperationModel,
    ready: int,
    duration: int,
    scheduled: list[tuple[AssignmentModel, str]],
    setups: dict[tuple[str, str, str], int],
    resource_by_id: dict,
    scheduled_by_resource: dict[str, list[AssignmentModel]],
) -> int:
    start = max(0, ready)
    for _ in range(10_000):
        start = _advance_past_downtime(machine, start, duration)
        end = start + duration
        resource_conflict_end = None
        for resource_id in operation.required_resources:
            resource = resource_by_id[resource_id]
            downtime = [window.end for window in resource.unavailable if _overlap(start, end, window.start, window.end)]
            overlapping_resource = [
                item for item in scheduled_by_resource[resource_id] if _overlap(start, end, item.start, item.end)
            ]
            if downtime:
                resource_conflict_end = max(resource_conflict_end or 0, max(downtime))
            if len(overlapping_resource) >= resource.capacity:
                resource_conflict_end = max(
                    resource_conflict_end or 0,
                    min(item.end for item in overlapping_resource),
                )
        if resource_conflict_end is not None:
            start = resource_conflict_end
            continue
        overlapping = [(item, family) for item, family in scheduled if _overlap(start, end, item.start, item.end)]
        if machine.batch_capacity == 1 and overlapping:
            start = max(item.end for item, _family in overlapping)
            continue
        if machine.batch_capacity > 1:
            incompatible = [(item, family) for item, family in overlapping if family != operation.family]
            if incompatible:
                item, family = max(incompatible, key=lambda row: row[0].end)
                start = item.end + _setup_duration(setups, machine, family, operation.family)
                continue
            if overlapping:
                start = max(item.end for item, _family in overlapping)
                continue
        previous = max((row for row in scheduled if row[0].end <= start), key=lambda row: row[0].end, default=None)
        if previous is not None and not (
            machine.batch_capacity > 1 and previous[1] == operation.family and previous[0].end == start
        ):
            required_start = previous[0].end + _setup_duration(setups, machine, previous[1], operation.family)
            if start < required_start:
                start = required_start
                continue
        return start
    raise RuntimeError("failed to place operation inside the scheduling horizon")


def _construct(problem: SemiconductorScheduleModel, choices: list[int], ranks: list[int]) -> list[AssignmentModel]:
    machine_by_id = {machine.id: machine for machine in problem.machines}
    resource_by_id = {resource.id: resource for resource in problem.auxiliary_resources}
    setups = _setup_map(problem)
    transports = _transport_map(problem)
    rows = _operation_rows(problem)
    row_index = {(lot.id, operation.id): index for index, (lot, _position, operation) in enumerate(rows)}
    next_operation = {lot.id: 0 for lot in problem.lots}
    completion: dict[tuple[str, str], int] = {}
    assignment_by_key: dict[tuple[str, str], AssignmentModel] = {}
    scheduled_by_machine: dict[str, list[tuple[AssignmentModel, str]]] = defaultdict(list)
    scheduled_by_resource: dict[str, list[AssignmentModel]] = defaultdict(list)
    assignments: list[AssignmentModel] = []
    pinned_by_key = {
        (item.lot_id, item.operation_id): item.model_copy(deep=True) for item in problem.pinned_assignments
    }
    for key, assignment in pinned_by_key.items():
        operation = next(operation for lot, _position, operation in rows if (lot.id, operation.id) == key)
        scheduled_by_machine[assignment.machine_id].append((assignment, operation.family))
        for resource_id in operation.required_resources:
            scheduled_by_resource[resource_id].append(assignment)

    def option_of(lot, operation):
        index = row_index[(lot.id, operation.id)]
        return operation.eligible_machines[choices[index] % len(operation.eligible_machines)]

    def ready_time(lot, position, machine):
        if not position:
            return lot.release_time
        previous_key = (lot.id, lot.operations[position - 1].id)
        previous = assignment_by_key[previous_key]
        previous_machine = machine_by_id[previous.machine_id]
        minimum_wait = max(
            transports.get((previous_machine.group, machine.group), 0),
            lot.operations[position - 1].min_queue_time,
        )
        return completion[previous_key] + minimum_wait

    remaining = len(rows)
    while remaining:
        ready_rows = [
            (ranks[row_index[(lot.id, lot.operations[position].id)]], lot, position, lot.operations[position])
            for lot in problem.lots
            for position in [next_operation[lot.id]]
            if position < len(lot.operations)
        ]
        selected = min(ready_rows, key=lambda row: (row[0], -row[1].priority, row[1].id))
        _rank, lot, _position, operation = selected
        pinned_assignment = pinned_by_key.get((lot.id, operation.id))
        if pinned_assignment is not None:
            assignments.append(pinned_assignment)
            assignment_by_key[(lot.id, operation.id)] = pinned_assignment
            completion[(lot.id, operation.id)] = pinned_assignment.end
            next_operation[lot.id] += 1
            remaining -= 1
            continue
        option = option_of(lot, operation)
        machine = machine_by_id[option.machine_id]
        batch_rows = [selected]
        if machine.batch_capacity > 1:
            for candidate in sorted(ready_rows, key=lambda row: (row[0], -row[1].priority, row[1].id)):
                if candidate is selected:
                    continue
                _candidate_rank, candidate_lot, _candidate_position, candidate_operation = candidate
                candidate_option = option_of(candidate_lot, candidate_operation)
                if (
                    candidate_option.machine_id == machine.id
                    and candidate_operation.family == operation.family
                    and candidate_option.duration == option.duration
                ):
                    batch_rows.append(candidate)
                if len(batch_rows) >= machine.batch_capacity:
                    break
        ready = max(ready_time(batch_lot, batch_position, machine) for _r, batch_lot, batch_position, _o in batch_rows)
        start = _earliest_start(
            machine,
            operation,
            ready,
            option.duration,
            scheduled_by_machine[machine.id],
            setups,
            resource_by_id,
            scheduled_by_resource,
        )
        for _batch_rank, batch_lot, _batch_position, batch_operation in batch_rows:
            assignment = AssignmentModel(
                lot_id=batch_lot.id,
                operation_id=batch_operation.id,
                machine_id=machine.id,
                start=start,
                end=start + option.duration,
            )
            scheduled_by_machine[machine.id].append((assignment, operation.family))
            for resource_id in batch_operation.required_resources:
                scheduled_by_resource[resource_id].append(assignment)
            assignments.append(assignment)
            assignment_by_key[(batch_lot.id, batch_operation.id)] = assignment
            completion[(batch_lot.id, batch_operation.id)] = assignment.end
            next_operation[batch_lot.id] += 1
            remaining -= 1

    batch_counter = 0
    for machine in problem.machines:
        if machine.batch_capacity == 1:
            continue
        machine_rows = sorted(scheduled_by_machine[machine.id], key=lambda row: (row[0].start, row[0].end))
        groups: list[list[tuple[AssignmentModel, str]]] = []
        for row in machine_rows:
            group = next(
                (
                    candidate
                    for candidate in groups
                    if candidate[0][1] == row[1]
                    and candidate[0][0].start == row[0].start
                    and candidate[0][0].end == row[0].end
                ),
                None,
            )
            if group is None:
                group = []
                groups.append(group)
            group.append(row)
        for group in groups:
            batch_counter += 1
            for assignment, _family in group:
                assignment.batch_id = f"B{batch_counter:03d}"
    return assignments


def _seed_state(problem: SemiconductorScheduleModel, rows: list, choices: list[int]) -> tuple[list[int], list[int]]:
    """Convert `problem.assignments` to the (choices, ranks) state that `_construct` decodes.

    Rows without an assignment keep the choice they have in `choices` and rank last.
    """
    assignment_by_key = {(item.lot_id, item.operation_id): item for item in problem.assignments}
    seeded = [assignment_by_key.get((lot.id, operation.id)) for lot, _position, operation in rows]
    selected = choices.copy()
    for row, (assignment, (_lot, _position, operation)) in enumerate(zip(seeded, rows, strict=True)):
        if assignment is not None:
            options = {
                (option.machine_id, option.duration): index for index, option in enumerate(operation.eligible_machines)
            }
            selected[row] = options.get((assignment.machine_id, assignment.end - assignment.start), selected[row])
    order = sorted(
        range(len(rows)),
        key=lambda row: (seeded[row] is None, seeded[row].start if seeded[row] is not None else 0, row),
    )
    ranks = [0] * len(rows)
    for rank, row in enumerate(order):
        ranks[row] = rank
    return selected, ranks


def solve(
    problem,
    *,
    time_limit_seconds=None,
    random_seed=None,
    parallel_solver_count=None,
    config=None,
    constraints=None,
):
    cfg = configure(config, time_limit_seconds, random_seed, parallel_solver_count)
    rows = _operation_rows(problem)
    choices = [
        min(
            range(len(operation.eligible_machines)),
            key=lambda index: (
                operation.eligible_machines[index].preference_cost,
                operation.eligible_machines[index].duration,
            ),
        )
        for _lot, _position, operation in rows
    ]
    ranks = list(range(len(rows)))
    initial = _seed_state(problem, rows, choices) if problem.warm_start else (choices, ranks)
    # A complete seed may not survive decoding, so the initial state stands for the assignments as supplied.
    supplied = None
    if problem.warm_start and len({(item.lot_id, item.operation_id) for item in problem.assignments}) == len(rows):
        supplied = problem.assignments

    def evaluate(state):
        if state is initial and supplied is not None:
            problem.assignments = supplied
        else:
            problem.assignments = _construct(problem, state[0], state[1])
        return calculate_score(problem, constraints)

    def neighbor(state, rng):
        selected = state[0].copy()
        order = state[1].copy()
        if rows and rng.random() < 0.55:
            index = rng.randrange(len(rows))
            option_count = len(rows[index][2].eligible_machines)
            if option_count > 1:
                selected[index] = (selected[index] + rng.randrange(1, option_count)) % option_count
        elif len(order) > 1:
            left, right = rng.sample(range(len(order)), 2)
            order[left], order[right] = order[right], order[left]
        return selected, order

    best, _best_score = anneal(initial, neighbor, evaluate, cfg)
    evaluate(best)
    return problem.score_hard, problem.score_soft
