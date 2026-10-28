from __future__ import annotations

from collections import defaultdict

from ortools.sat.python import cp_model
from semiconductor_scheduling import calculate_score
from semiconductor_scheduling.constraints import DEFAULTS
from semiconductor_scheduling.models import AssignmentModel

from .common import Objective, selections, solve_model
from .common import catalog as make_catalog


def catalog():
    return make_catalog(DEFAULTS)


def solve(problem, *, time_limit_seconds=30.0, random_seed=0, num_workers=1, constraints=None):
    selected = selections(DEFAULTS, constraints)
    model = cp_model.CpModel()
    objective = Objective(model, selected)
    machine_by_id = {machine.id: machine for machine in problem.machines}
    setups = {
        (setup.machine_group, setup.from_family, setup.to_family): setup.duration for setup in problem.setup_times
    }
    transports = {(item.from_group, item.to_group): item.duration for item in problem.transport_times}
    operation_rows = [
        (lot_index, operation_index, lot, operation)
        for lot_index, lot in enumerate(problem.lots)
        for operation_index, operation in enumerate(lot.operations)
    ]
    max_due = max(lot.due_time for lot in problem.lots)
    total_duration = sum(
        max(option.duration for option in operation.eligible_machines)
        for _lot_index, _operation_index, _lot, operation in operation_rows
    )
    total_downtime = sum(window.end - window.start for machine in problem.machines for window in machine.unavailable)
    total_downtime += sum(
        window.end - window.start for resource in problem.auxiliary_resources for window in resource.unavailable
    )
    total_setup = sum(setup.duration for setup in problem.setup_times)
    latest_unavailability = max(
        (
            window.end
            for resource in [*problem.machines, *problem.auxiliary_resources]
            for window in resource.unavailable
        ),
        default=0,
    )
    latest_pin = max((item.end for item in problem.pinned_assignments), default=0)
    horizon = max(
        1,
        max_due + total_duration + total_downtime + total_setup,
        latest_unavailability + total_duration + total_setup,
        latest_pin,
    )

    starts = {}
    ends = {}
    presence = {}
    intervals_by_machine = defaultdict(list)
    operations_by_machine = defaultdict(list)
    intervals_by_resource = defaultdict(list)
    preference_terms = []
    for lot_index, operation_index, _lot, operation in operation_rows:
        key = (lot_index, operation_index)
        starts[key] = model.new_int_var(0, horizon, f"lot_{lot_index}_operation_{operation_index}_start")
        ends[key] = model.new_int_var(0, horizon, f"lot_{lot_index}_operation_{operation_index}_end")
        choices = []
        for option_index, option in enumerate(operation.eligible_machines):
            chosen = model.new_bool_var(f"lot_{lot_index}_operation_{operation_index}_machine_{option.machine_id}")
            presence[lot_index, operation_index, option.machine_id] = chosen
            choices.append(chosen)
            interval = model.new_optional_interval_var(
                starts[key],
                option.duration,
                ends[key],
                chosen,
                f"lot_{lot_index}_operation_{operation_index}_option_{option_index}",
            )
            intervals_by_machine[option.machine_id].append(interval)
            for resource_id in operation.required_resources:
                intervals_by_resource[resource_id].append(interval)
            operations_by_machine[option.machine_id].append(
                (key, operation.family, option.duration, chosen, starts[key], ends[key])
            )
            preference_terms.append(option.preference_cost * chosen)
        model.add_exactly_one(choices)

    for resource in problem.auxiliary_resources:
        intervals = list(intervals_by_resource[resource.id])
        demands = [1] * len(intervals)
        for window_index, window in enumerate(resource.unavailable):
            intervals.append(
                model.new_fixed_size_interval_var(
                    window.start,
                    window.end - window.start,
                    f"{resource.id}_downtime_{window_index}",
                )
            )
            demands.append(resource.capacity)
        if intervals:
            model.add_cumulative(intervals, demands, resource.capacity)

    for pinned in problem.pinned_assignments:
        lot_index = next(index for index, lot in enumerate(problem.lots) if lot.id == pinned.lot_id)
        operation_index = next(
            index
            for index, operation in enumerate(problem.lots[lot_index].operations)
            if operation.id == pinned.operation_id
        )
        model.add(presence[lot_index, operation_index, pinned.machine_id] == 1)
        model.add(starts[lot_index, operation_index] == pinned.start)
        model.add(ends[lot_index, operation_index] == pinned.end)

    for lot_index, lot in enumerate(problem.lots):
        model.add(starts[lot_index, 0] >= lot.release_time)
        for operation_index in range(1, len(lot.operations)):
            model.add(starts[lot_index, operation_index] >= ends[lot_index, operation_index - 1])
            previous = lot.operations[operation_index - 1]
            current = lot.operations[operation_index]
            if previous.min_queue_time:
                model.add(
                    starts[lot_index, operation_index] >= ends[lot_index, operation_index - 1] + previous.min_queue_time
                )
            for previous_option in previous.eligible_machines:
                for current_option in current.eligible_machines:
                    previous_machine = machine_by_id[previous_option.machine_id]
                    current_machine = machine_by_id[current_option.machine_id]
                    duration = transports.get((previous_machine.group, current_machine.group), 0)
                    if duration:
                        model.add(
                            starts[lot_index, operation_index] >= ends[lot_index, operation_index - 1] + duration
                        ).only_enforce_if(
                            [
                                presence[lot_index, operation_index - 1, previous_option.machine_id],
                                presence[lot_index, operation_index, current_option.machine_id],
                            ]
                        )

    batch_mates = defaultdict(list)
    load_by_machine = {}
    for machine in problem.machines:
        rows = operations_by_machine[machine.id]
        load_by_machine[machine.id] = sum(duration * chosen for _key, _family, duration, chosen, _start, _end in rows)
        if machine.batch_capacity > 1:
            model.add_cumulative(intervals_by_machine[machine.id], [1] * len(rows), machine.batch_capacity)
        for left_index, left in enumerate(rows):
            left_key, left_family, _left_duration, left_present, left_start, left_end = left
            for right in rows[left_index + 1 :]:
                right_key, right_family, _right_duration, right_present, right_start, right_end = right
                if left_key == right_key:
                    continue
                if machine.batch_capacity > 1 and left_family == right_family:
                    same_batch = model.new_bool_var(
                        f"{machine.id}_{left_key[0]}_{left_key[1]}_batch_{right_key[0]}_{right_key[1]}"
                    )
                    left_before = model.new_bool_var(
                        f"{machine.id}_{left_key[0]}_{left_key[1]}_before_{right_key[0]}_{right_key[1]}"
                    )
                    right_before = model.new_bool_var(
                        f"{machine.id}_{right_key[0]}_{right_key[1]}_before_{left_key[0]}_{left_key[1]}"
                    )
                    model.add(same_batch + left_before + right_before == 1).only_enforce_if(
                        [left_present, right_present]
                    )
                    for mode in (same_batch, left_before, right_before):
                        model.add(mode <= left_present)
                        model.add(mode <= right_present)
                    model.add(left_start == right_start).only_enforce_if([left_present, right_present, same_batch])
                    model.add(left_end <= right_start).only_enforce_if([left_present, right_present, left_before])
                    model.add(right_end <= left_start).only_enforce_if([left_present, right_present, right_before])
                    batch_mates[machine.id, left_key].append((right_key, same_batch))
                    batch_mates[machine.id, right_key].append((left_key, same_batch))
                    continue
                left_before = model.new_bool_var(
                    f"{machine.id}_{left_key[0]}_{left_key[1]}_before_{right_key[0]}_{right_key[1]}"
                )
                left_setup = setups.get((machine.group, left_family, right_family), 0)
                right_setup = setups.get((machine.group, right_family, left_family), 0)
                model.add(left_end + left_setup <= right_start).only_enforce_if(
                    [left_present, right_present, left_before]
                )
                model.add(right_end + right_setup <= left_start).only_enforce_if(
                    [left_present, right_present, left_before.negated()]
                )
        for key, _family, _duration, chosen, start, end in rows:
            for window_index, window in enumerate(machine.unavailable):
                before = model.new_bool_var(f"{machine.id}_{key[0]}_{key[1]}_before_downtime_{window_index}")
                model.add(end <= window.start).only_enforce_if([chosen, before])
                model.add(start >= window.end).only_enforce_if([chosen, before.negated()])

    queue_excesses = []
    tardiness_terms = []
    cycle_terms = []
    maximum_cycle_excesses = []
    last_ends = []
    for lot_index, lot in enumerate(problem.lots):
        for operation_index, operation in enumerate(lot.operations[:-1]):
            if operation.max_queue_time is None:
                continue
            excess = model.new_int_var(0, horizon, f"lot_{lot_index}_queue_excess_{operation_index}")
            model.add_max_equality(
                excess,
                [
                    0,
                    starts[lot_index, operation_index + 1]
                    - ends[lot_index, operation_index]
                    - operation.max_queue_time,
                ],
            )
            queue_excesses.append(excess)
        completion = ends[lot_index, len(lot.operations) - 1]
        last_ends.append(completion)
        tardiness = model.new_int_var(0, horizon, f"lot_{lot_index}_tardiness")
        model.add_max_equality(tardiness, [0, completion - lot.due_time])
        tardiness_terms.append(lot.priority * tardiness)
        cycle_terms.append(completion - lot.release_time)
        if lot.max_cycle_time is not None:
            excess = model.new_int_var(0, horizon, f"lot_{lot_index}_maximum_cycle_excess")
            model.add_max_equality(excess, [0, completion - lot.release_time - lot.max_cycle_time])
            maximum_cycle_excesses.append(excess)

    makespan = model.new_int_var(0, horizon, "makespan")
    model.add_max_equality(makespan, last_ends)
    batch_shortfalls = []
    for machine in problem.machines:
        if machine.batch_capacity == 1 or machine.minimum_batch_size == 1:
            continue
        for key, _family, _duration, chosen, _start, _end in operations_by_machine[machine.id]:
            mates = batch_mates[machine.id, key]
            earlier = [same_batch for other_key, same_batch in mates if other_key < key]
            leader = model.new_bool_var(f"{machine.id}_{key[0]}_{key[1]}_batch_leader")
            model.add(leader <= chosen)
            model.add(leader >= chosen - sum(earlier))
            for same_batch in earlier:
                model.add(leader + same_batch <= 1)
            batch_size = chosen + sum(same_batch for _other_key, same_batch in mates)
            shortfall = model.new_int_var(
                0,
                machine.minimum_batch_size,
                f"{machine.id}_{key[0]}_{key[1]}_batch_shortfall",
            )
            model.add(shortfall >= machine.minimum_batch_size * leader - batch_size)
            batch_shortfalls.append(shortfall)

    load_ranges = []
    machines_by_group = defaultdict(list)
    for machine in problem.machines:
        machines_by_group[machine.group].append(machine)
    for group, machines in machines_by_group.items():
        if len(machines) < 2:
            continue
        maximum = model.new_int_var(0, total_duration, f"{group}_maximum_load")
        minimum = model.new_int_var(0, total_duration, f"{group}_minimum_load")
        model.add_max_equality(maximum, [load_by_machine[machine.id] for machine in machines])
        model.add_min_equality(minimum, [load_by_machine[machine.id] for machine in machines])
        load_ranges.append(maximum - minimum)
    zero = model.new_int_var(0, 0, "structural_constraints_satisfied")
    for name in (
        "machine_eligibility",
        "lot_route_precedence",
        "machine_capacity",
        "batch_capacity",
        "batch_family_compatibility",
        "setup_transitions",
        "maintenance_windows",
        "minimum_queue_time",
        "inter_tool_transport",
        "auxiliary_resource_capacity",
        "auxiliary_resource_availability",
        "pinned_assignments",
    ):
        objective.add(name, zero, 0)
    objective.add("maximum_queue_time", sum(queue_excesses), len(queue_excesses) * horizon)
    objective.add("maximum_cycle_time", sum(maximum_cycle_excesses), len(maximum_cycle_excesses) * horizon)
    objective.add(
        "minimum_batch_fill",
        sum(batch_shortfalls),
        len(batch_shortfalls) * max((machine.minimum_batch_size for machine in problem.machines), default=1),
    )
    objective.add("machine_load_balance", sum(load_ranges), len(load_ranges) * total_duration)
    objective.add(
        "machine_preference",
        sum(preference_terms),
        sum(preference_terms)
        if not preference_terms
        else sum(
            max(option.preference_cost for option in operation.eligible_machines)
            for _lot_index, _operation_index, _lot, operation in operation_rows
        ),
    )
    objective.add("weighted_tardiness", sum(tardiness_terms), sum(lot.priority for lot in problem.lots) * horizon)
    objective.add("total_cycle_time", sum(cycle_terms), len(problem.lots) * horizon)
    objective.add("makespan", makespan, horizon)
    objective.minimize()

    solver = solve_model(
        model,
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        num_workers=num_workers,
    )
    result = problem.model_copy(deep=True)
    result.assignments = []
    batch_groups: dict[tuple[str, str, int], str] = {}
    for lot_index, operation_index, lot, operation in operation_rows:
        selected_option = next(
            option
            for option in operation.eligible_machines
            if solver.boolean_value(presence[lot_index, operation_index, option.machine_id])
        )
        start = solver.value(starts[lot_index, operation_index])
        machine = machine_by_id[selected_option.machine_id]
        batch_id = None
        if machine.batch_capacity > 1:
            batch_key = (machine.id, operation.family, start)
            batch_id = batch_groups.setdefault(batch_key, f"B{len(batch_groups) + 1:03d}")
        result.assignments.append(
            AssignmentModel(
                lot_id=lot.id,
                operation_id=operation.id,
                machine_id=machine.id,
                start=start,
                end=solver.value(ends[lot_index, operation_index]),
                batch_id=batch_id,
            )
        )
    calculate_score(result, selected)
    return result
