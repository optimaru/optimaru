from ortools.sat.python import cp_model
from quadratic_assignment import calculate_score
from quadratic_assignment.constraints import DEFAULTS

from .common import Objective, selections, solve_model
from .common import catalog as make_catalog


def catalog():
    return make_catalog(DEFAULTS)


def solve(problem, *, time_limit_seconds=30.0, random_seed=0, num_workers=1, constraints=None):
    selected = selections(DEFAULTS, constraints)
    size = len(problem.facilities)
    model = cp_model.CpModel()
    assigned = {
        (facility, location): model.new_bool_var(f"f{facility}_l{location}")
        for facility in range(size)
        for location in range(size)
    }
    for facility in range(size):
        model.add_exactly_one(assigned[facility, location] for location in range(size))
    for location in range(size):
        model.add_exactly_one(assigned[facility, location] for facility in range(size))
    terms = []
    bound = 0
    for facility_a in range(size):
        for facility_b in range(size):
            flow = problem.flows[facility_a][facility_b]
            if not flow:
                continue
            for location_a in range(size):
                for location_b in range(size):
                    coefficient = flow * problem.distances[location_a][location_b]
                    if not coefficient:
                        continue
                    both = model.new_bool_var(f"pair_{facility_a}_{facility_b}_{location_a}_{location_b}")
                    model.add_multiplication_equality(
                        both, [assigned[facility_a, location_a], assigned[facility_b, location_b]]
                    )
                    terms.append(coefficient * both)
                    bound += coefficient
    objective = Objective(model, selected)
    fixed_violations = [
        1 - assigned[problem.facilities.index(facility), problem.locations.index(location)]
        for location, facility in problem.fixed_assignments.items()
    ]
    objective.add("fixed_assignments", sum(fixed_violations), len(fixed_violations))
    forbidden_violations = [
        assigned[problem.facilities.index(row.facility), problem.locations.index(row.location)]
        for row in problem.forbidden_assignments
    ]
    objective.add("forbidden_assignments", sum(forbidden_violations), len(forbidden_violations))
    capacity_violations = []
    for location, location_name in enumerate(problem.locations):
        capacity = problem.location_capacities.get(location_name)
        if capacity is None:
            continue
        demand = sum(
            problem.facility_demands.get(problem.facilities[facility], 0) * assigned[facility, location]
            for facility in range(size)
        )
        excess = model.new_int_var(
            0,
            max(problem.facility_demands.values(), default=0),
            f"location_{location}_excess",
        )
        model.add(excess >= demand - capacity)
        capacity_violations.append(excess)
    objective.add("location_capacities", sum(capacity_violations), sum(problem.facility_demands.values()))
    if problem.assignment_costs is not None:
        direct_cost = sum(
            problem.assignment_costs[facility][location] * assigned[facility, location]
            for facility in range(size)
            for location in range(size)
        )
        objective.add(
            "minimize_assignment_cost",
            direct_cost,
            sum(max((abs(value) for value in row), default=0) for row in problem.assignment_costs),
        )
    else:
        objective.add("minimize_assignment_cost", 0, 0)
    objective.add("minimize_flow_distance", sum(terms), bound)
    objective.minimize()
    solver = solve_model(model, time_limit_seconds=time_limit_seconds, random_seed=random_seed, num_workers=num_workers)
    result = problem.model_copy(deep=True)
    result.assignment = {
        result.locations[location]: result.facilities[
            next(facility for facility in range(size) if solver.value(assigned[facility, location]))
        ]
        for location in range(size)
    }
    calculate_score(result, selected)
    return result
