from __future__ import annotations

from ._core import SolverConfig as SolverConfig
from .common import anneal, configure
from .constraints import DEFAULTS, score
from .models import KnapsackProblemModel, _greedy_selection
from .models import with_initial_solution as with_initial_solution


def list_constraints() -> list[str]:
    return [constraint.name for constraint in DEFAULTS]


def constraint_costs(problem: KnapsackProblemModel) -> dict:
    weight = sum(item.weight for item in problem.items if item.selected)
    value = sum(item.value for item in problem.items if item.selected)
    selected = {item.id for item in problem.items if item.selected}
    usage = {
        resource: sum(item.resource_usage.get(resource, 0) for item in problem.items if item.selected)
        for resource in problem.resource_capacities
    }
    group_costs = []
    for group in problem.selection_groups:
        count = len(selected.intersection(group.item_ids))
        maximum = len(group.item_ids) if group.max_selected is None else group.max_selected
        group_costs.append(max(0, group.min_selected - count) + max(0, count - maximum))
    return {
        "capacity": [max(0, weight - problem.capacity)],
        "resource_capacities": [
            max(0, usage[resource] - capacity) for resource, capacity in problem.resource_capacities.items()
        ],
        "mandatory_items": [int(item_id not in selected) for item_id in problem.mandatory_items],
        "forbidden_items": [int(item_id in selected) for item_id in problem.forbidden_items],
        "selection_group_bounds": group_costs,
        "item_dependencies": [
            int(item.id in selected and required not in selected)
            for item in problem.items
            for required in item.requires
        ],
        "maximize_value": [-value],
    }


def calculate_score(problem: KnapsackProblemModel, constraints=None):
    problem.total_weight = sum(item.weight for item in problem.items if item.selected)
    problem.total_value = sum(item.value for item in problem.items if item.selected)
    result = score(constraint_costs(problem), constraints)
    problem.score_hard, problem.score_soft = result
    return result


def solve(
    problem, *, time_limit_seconds=None, random_seed=None, parallel_solver_count=None, config=None, constraints=None
):
    cfg = configure(config, time_limit_seconds, random_seed, parallel_solver_count)
    initial = [item.selected for item in problem.items] if problem.warm_start else _greedy_selection(problem)

    def evaluate(state):
        for item, selected in zip(problem.items, state, strict=True):
            item.selected = selected
        return calculate_score(problem, constraints)

    def neighbor(state, rng):
        result = state.copy()
        result[rng.randrange(len(result))] ^= True
        return result

    if problem.items:
        best, _ = anneal(initial, neighbor, evaluate, cfg)
        evaluate(best)
    else:
        calculate_score(problem, constraints)
    # A seed is used once, so a result edited and solved again starts fresh.
    problem.warm_start = False
    return problem.score_hard, problem.score_soft
