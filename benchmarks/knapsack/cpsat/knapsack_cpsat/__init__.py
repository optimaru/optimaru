from knapsack import calculate_score
from knapsack.constraints import DEFAULTS
from ortools.sat.python import cp_model

from .common import Objective, selections, solve_model
from .common import catalog as make_catalog


def catalog():
    return make_catalog(DEFAULTS)


def solve(problem, *, time_limit_seconds=30.0, random_seed=0, num_workers=1, constraints=None):
    selected = selections(DEFAULTS, constraints)
    model = cp_model.CpModel()
    chosen = [model.new_bool_var(f"selected_{index}") for index in range(len(problem.items))]
    total_weight = sum(item.weight * chosen[index] for index, item in enumerate(problem.items))
    overflow = model.new_int_var(0, sum(item.weight for item in problem.items), "overflow")
    model.add(overflow >= total_weight - problem.capacity)
    total_value = sum(item.value * chosen[index] for index, item in enumerate(problem.items))
    by_id = {item.id: chosen[index] for index, item in enumerate(problem.items)}
    objective = Objective(model, selected)
    objective.add("capacity", overflow, sum(item.weight for item in problem.items))
    for resource, capacity in problem.resource_capacities.items():
        total = sum(item.resource_usage.get(resource, 0) * chosen[index] for index, item in enumerate(problem.items))
        excess = model.new_int_var(
            0, sum(item.resource_usage.get(resource, 0) for item in problem.items), f"{resource}_overflow"
        )
        model.add(excess >= total - capacity)
        objective.add(
            "resource_capacities", excess, sum(item.resource_usage.get(resource, 0) for item in problem.items)
        )
    objective.add(
        "mandatory_items", sum(1 - by_id[item_id] for item_id in problem.mandatory_items), len(problem.mandatory_items)
    )
    objective.add(
        "forbidden_items", sum(by_id[item_id] for item_id in problem.forbidden_items), len(problem.forbidden_items)
    )
    group_violations = []
    for index, group in enumerate(problem.selection_groups):
        count = sum(by_id[item_id] for item_id in group.item_ids)
        maximum = len(group.item_ids) if group.max_selected is None else group.max_selected
        under = model.new_int_var(0, group.min_selected, f"group_{index}_under")
        over = model.new_int_var(0, len(group.item_ids), f"group_{index}_over")
        model.add(under >= group.min_selected - count)
        model.add(over >= count - maximum)
        group_violations.extend((under, over))
    objective.add(
        "selection_group_bounds", sum(group_violations), sum(len(group.item_ids) for group in problem.selection_groups)
    )
    dependency_violations = []
    for item in problem.items:
        for required in item.requires:
            violation = model.new_bool_var(f"requires_{item.id}_{required}")
            model.add(violation >= by_id[item.id] - by_id[required])
            dependency_violations.append(violation)
    objective.add("item_dependencies", sum(dependency_violations), len(dependency_violations))
    objective.add("maximize_value", total_value, sum(item.value for item in problem.items), reward=True)
    objective.minimize()
    solver = solve_model(model, time_limit_seconds=time_limit_seconds, random_seed=random_seed, num_workers=num_workers)
    result = problem.model_copy(deep=True)
    for index, item in enumerate(result.items):
        item.selected = bool(solver.value(chosen[index]))
    calculate_score(result, selected)
    return result
