from assembly_line_balancing import calculate_score
from assembly_line_balancing.constraints import DEFAULTS
from ortools.sat.python import cp_model

from .common import Objective, selections, solve_model
from .common import catalog as make_catalog


def catalog():
    return make_catalog(DEFAULTS)


def solve(problem, *, time_limit_seconds=30.0, random_seed=0, num_workers=1, constraints=None):
    selected = selections(DEFAULTS, constraints)
    count = len(problem.tasks)
    model = cp_model.CpModel()
    assigned = {
        (task, station): model.new_bool_var(f"task_{task}_station_{station}")
        for task in range(count)
        for station in range(count)
    }
    station_of = [model.new_int_var(0, max(0, count - 1), f"station_of_{task}") for task in range(count)]
    used = [model.new_bool_var(f"station_{station}_used") for station in range(count)]
    overflow = []
    for task in range(count):
        model.add_exactly_one(assigned[task, station] for station in range(count))
        model.add(station_of[task] == sum(station * assigned[task, station] for station in range(count)))
    for station in range(count):
        members = [assigned[task, station] for task in range(count)]
        if members:
            model.add_max_equality(used[station], members)
        load = sum(problem.tasks[task].duration * assigned[task, station] for task in range(count))
        excess = model.new_int_var(0, sum(task.duration for task in problem.tasks), f"overflow_{station}")
        model.add(excess >= load - problem.cycle_time)
        overflow.append(excess)
        if station + 1 < count:
            model.add(used[station] >= used[station + 1])
    index = {task.id: position for position, task in enumerate(problem.tasks)}
    reversed_arcs = []
    for edge_position, edge in enumerate(problem.precedences):
        reversed_arc = model.new_bool_var(f"reversed_{edge_position}")
        model.add(station_of[index[edge.before]] > station_of[index[edge.after]]).only_enforce_if(reversed_arc)
        model.add(station_of[index[edge.before]] <= station_of[index[edge.after]]).only_enforce_if(reversed_arc.Not())
        reversed_arcs.append(reversed_arc)
    objective = Objective(model, selected)
    objective.add("station_capacity", sum(overflow), sum(task.duration for task in problem.tasks))
    objective.add("task_precedence", sum(reversed_arcs), len(reversed_arcs))
    station_limit_violations = []
    if problem.max_stations is not None:
        for task in range(count):
            excess = model.new_int_var(0, count, f"task_{task}_station_limit_excess")
            model.add(excess >= station_of[task] - problem.max_stations + 1)
            station_limit_violations.append(excess)
    objective.add("station_count_limit", sum(station_limit_violations), count * count)
    bound_violations = []
    fixed_violations = []
    forbidden_violations = []
    for task_index, task in enumerate(problem.tasks):
        under = model.new_int_var(0, count, f"task_{task_index}_under_station")
        model.add(under >= task.min_station - station_of[task_index])
        bound_violations.append(under)
        if task.max_station is not None:
            over = model.new_int_var(0, count, f"task_{task_index}_over_station")
            model.add(over >= station_of[task_index] - task.max_station)
            bound_violations.append(over)
        if task.fixed_station is not None:
            fixed_violations.append(1 - assigned[task_index, task.fixed_station])
        forbidden_violations.extend(
            assigned[task_index, station] for station in task.forbidden_stations if station < count
        )
    objective.add("task_station_bounds", sum(bound_violations), count * count)
    objective.add("fixed_task_stations", sum(fixed_violations), len(fixed_violations))
    objective.add("forbidden_task_stations", sum(forbidden_violations), len(forbidden_violations))
    same_violations = []
    for group_index, group in enumerate(problem.same_station_groups):
        anchor = index[group.task_ids[0]]
        for task_id in group.task_ids[1:]:
            violation = model.new_bool_var(f"same_group_{group_index}_{task_id}_violation")
            other = index[task_id]
            model.add(station_of[anchor] != station_of[other]).only_enforce_if(violation)
            model.add(station_of[anchor] == station_of[other]).only_enforce_if(violation.Not())
            same_violations.append(violation)
    objective.add("same_station_groups", sum(same_violations), len(same_violations))
    different_violations = []
    for pair_index, pair in enumerate(problem.different_station_pairs):
        violation = model.new_bool_var(f"different_pair_{pair_index}_violation")
        first, second = index[pair.first], index[pair.second]
        model.add(station_of[first] == station_of[second]).only_enforce_if(violation)
        model.add(station_of[first] != station_of[second]).only_enforce_if(violation.Not())
        different_violations.append(violation)
    objective.add("different_station_pairs", sum(different_violations), len(different_violations))
    objective.add("minimize_stations", sum(used), count)
    max_load = model.new_int_var(0, sum(task.duration for task in problem.tasks), "max_station_load")
    min_load = model.new_int_var(0, sum(task.duration for task in problem.tasks), "min_used_station_load")
    loads = []
    for station in range(count):
        load = sum(problem.tasks[task].duration * assigned[task, station] for task in range(count))
        loads.append(load)
        model.add(max_load >= load)
        model.add(min_load <= load + sum(task.duration for task in problem.tasks) * (1 - used[station]))
    objective.add("workload_balance", max_load - min_load, sum(task.duration for task in problem.tasks))
    objective.minimize()
    solver = solve_model(model, time_limit_seconds=time_limit_seconds, random_seed=random_seed, num_workers=num_workers)
    result = problem.model_copy(deep=True)
    for task_position, task in enumerate(result.tasks):
        task.station = solver.value(station_of[task_position])
    calculate_score(result, selected)
    return result
