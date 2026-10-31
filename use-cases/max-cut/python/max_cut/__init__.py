from __future__ import annotations

from ._core import SolverConfig as SolverConfig
from .common import anneal, configure
from .constraints import DEFAULTS, score
from .models import MaxCutProblemModel
from .models import with_initial_solution as with_initial_solution


def list_constraints() -> list[str]:
    return [constraint.name for constraint in DEFAULTS]


def constraint_costs(problem: MaxCutProblemModel) -> dict:
    cut = sum(
        edge.weight
        for edge in problem.edges
        if problem.partition.get(edge.source) != problem.partition.get(edge.target)
    )
    side_one = sum(problem.partition.get(vertex, 0) == 1 for vertex in problem.vertices)
    side_zero = len(problem.vertices) - side_one
    minimum = 0 if problem.min_partition_size is None else problem.min_partition_size
    maximum = len(problem.vertices) if problem.max_partition_size is None else problem.max_partition_size
    return {
        "fixed_partition": [
            int(problem.partition.get(vertex) != side) for vertex, side in problem.fixed_partition.items()
        ],
        "same_partition_pairs": [
            int(problem.partition.get(pair.first) != problem.partition.get(pair.second))
            for pair in problem.same_partition_pairs
        ],
        "different_partition_pairs": [
            int(problem.partition.get(pair.first) == problem.partition.get(pair.second))
            for pair in problem.different_partition_pairs
        ],
        "partition_size_bounds": [
            max(0, minimum - side_zero) + max(0, side_zero - maximum),
            max(0, minimum - side_one) + max(0, side_one - maximum),
        ],
        "partition_balance": [abs(side_zero - side_one)],
        "maximize_cut_weight": [-cut],
    }


def calculate_score(problem: MaxCutProblemModel, constraints=None):
    problem.cut_weight = -constraint_costs(problem)["maximize_cut_weight"][0]
    result = score(constraint_costs(problem), constraints)
    problem.score_hard, problem.score_soft = result
    return result


def solve(
    problem, *, time_limit_seconds=None, random_seed=None, parallel_solver_count=None, config=None, constraints=None
):
    cfg = configure(config, time_limit_seconds, random_seed, parallel_solver_count)
    initial = [index % 2 for index in range(len(problem.vertices))]
    vertex_index = {vertex: index for index, vertex in enumerate(problem.vertices)}
    for vertex, side in problem.fixed_partition.items():
        initial[vertex_index[vertex]] = side
    if problem.warm_start:
        for vertex, side in problem.partition.items():
            initial[vertex_index[vertex]] = side

    def evaluate(state):
        problem.partition = dict(zip(problem.vertices, state, strict=True))
        return calculate_score(problem, constraints)

    def neighbor(state, rng):
        result = state.copy()
        movable = [index for index, vertex in enumerate(problem.vertices) if vertex not in problem.fixed_partition]
        exact_size = problem.min_partition_size is not None and problem.min_partition_size == problem.max_partition_size
        if exact_size:
            left = [index for index in movable if result[index] == 0]
            right = [index for index in movable if result[index] == 1]
            if left and right:
                left_index, right_index = rng.choice(left), rng.choice(right)
                result[left_index] = 1
                result[right_index] = 0
        elif movable:
            result[rng.choice(movable)] ^= 1
        return result

    if problem.vertices:
        best, _ = anneal(initial, neighbor, evaluate, cfg)
        evaluate(best)
    else:
        calculate_score(problem, constraints)
    # A seed is used once, so a result edited and solved again starts fresh.
    problem.warm_start = False
    return problem.score_hard, problem.score_soft
