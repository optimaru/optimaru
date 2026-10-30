from __future__ import annotations

from ._core import SolverConfig as SolverConfig
from .common import anneal, configure
from .constraints import DEFAULTS, score
from .models import QuadraticAssignmentProblemModel
from .models import with_initial_solution as with_initial_solution


def list_constraints() -> list[str]:
    return [constraint.name for constraint in DEFAULTS]


def constraint_costs(problem: QuadraticAssignmentProblemModel) -> dict:
    facility_index = {name: index for index, name in enumerate(problem.facilities)}
    permutation = [facility_index[problem.assignment[location]] for location in problem.locations]
    cost = sum(
        problem.distances[i][j] * problem.flows[permutation[i]][permutation[j]]
        for i in range(len(permutation))
        for j in range(len(permutation))
    )
    assignment_cost = 0
    if problem.assignment_costs is not None:
        assignment_cost = sum(
            problem.assignment_costs[facility_index[problem.assignment[location]]][location_index]
            for location_index, location in enumerate(problem.locations)
        )
    forbidden = {(row.location, row.facility) for row in problem.forbidden_assignments}
    return {
        "fixed_assignments": [
            int(problem.assignment.get(location) != facility)
            for location, facility in problem.fixed_assignments.items()
        ],
        "forbidden_assignments": [
            int((location, facility) in forbidden) for location, facility in problem.assignment.items()
        ],
        "location_capacities": [
            max(
                0,
                problem.facility_demands.get(problem.assignment[location], 0)
                - problem.location_capacities.get(
                    location, problem.facility_demands.get(problem.assignment[location], 0)
                ),
            )
            for location in problem.locations
        ],
        "minimize_assignment_cost": [assignment_cost],
        "minimize_flow_distance": [cost],
    }


def calculate_score(problem: QuadraticAssignmentProblemModel, constraints=None):
    costs = constraint_costs(problem)
    problem.total_cost = costs["minimize_flow_distance"][0] + costs["minimize_assignment_cost"][0]
    result = score(costs, constraints)
    problem.score_hard, problem.score_soft = result
    return result


def solve(
    problem, *, time_limit_seconds=None, random_seed=None, parallel_solver_count=None, config=None, constraints=None
):
    cfg = configure(config, time_limit_seconds, random_seed, parallel_solver_count)
    initial = list(range(len(problem.facilities)))
    if problem.warm_start:
        facility_index = {facility: index for index, facility in enumerate(problem.facilities)}
        initial = [facility_index[problem.assignment[location]] for location in problem.locations]

    def evaluate(state):
        problem.assignment = {
            location: problem.facilities[state[index]] for index, location in enumerate(problem.locations)
        }
        return calculate_score(problem, constraints)

    def neighbor(state, rng):
        result = state.copy()
        if len(result) > 1:
            left, right = rng.sample(range(len(result)), 2)
            result[left], result[right] = result[right], result[left]
        return result

    if problem.facilities:
        best, _ = anneal(initial, neighbor, evaluate, cfg)
        evaluate(best)
    else:
        problem.assignment = {}
        problem.total_cost = 0
        problem.score_hard, problem.score_soft = 0, 0.0
    # A seed is used once, so a result edited and solved again starts fresh.
    problem.warm_start = False
    return problem.score_hard, problem.score_soft
