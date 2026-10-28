from max_cut import calculate_score
from max_cut.constraints import DEFAULTS
from ortools.sat.python import cp_model

from .common import Objective, selections, solve_model
from .common import catalog as make_catalog


def catalog():
    return make_catalog(DEFAULTS)


def solve(problem, *, time_limit_seconds=30.0, random_seed=0, num_workers=1, constraints=None):
    selected = selections(DEFAULTS, constraints)
    model = cp_model.CpModel()
    side = {vertex: model.new_bool_var(f"side_{index}") for index, vertex in enumerate(problem.vertices)}
    cut = []
    for index, edge in enumerate(problem.edges):
        crossed = model.new_bool_var(f"edge_{index}_crossed")
        model.add(side[edge.source] != side[edge.target]).only_enforce_if(crossed)
        model.add(side[edge.source] == side[edge.target]).only_enforce_if(crossed.Not())
        cut.append(edge.weight * crossed)
    if problem.vertices and not problem.fixed_partition:
        model.add(side[problem.vertices[0]] == 0)
    objective = Objective(model, selected)
    fixed_violations = []
    for vertex, required in problem.fixed_partition.items():
        violation = side[vertex] if required == 0 else 1 - side[vertex]
        fixed_violations.append(violation)
    objective.add("fixed_partition", sum(fixed_violations), len(fixed_violations))
    same_violations = []
    for index, pair in enumerate(problem.same_partition_pairs):
        violation = model.new_bool_var(f"same_pair_{index}_violation")
        model.add(side[pair.first] != side[pair.second]).only_enforce_if(violation)
        model.add(side[pair.first] == side[pair.second]).only_enforce_if(violation.Not())
        same_violations.append(violation)
    objective.add("same_partition_pairs", sum(same_violations), len(same_violations))
    different_violations = []
    for index, pair in enumerate(problem.different_partition_pairs):
        violation = model.new_bool_var(f"different_pair_{index}_violation")
        model.add(side[pair.first] == side[pair.second]).only_enforce_if(violation)
        model.add(side[pair.first] != side[pair.second]).only_enforce_if(violation.Not())
        different_violations.append(violation)
    objective.add("different_partition_pairs", sum(different_violations), len(different_violations))
    side_one = sum(side.values())
    side_zero = len(problem.vertices) - side_one
    minimum = 0 if problem.min_partition_size is None else problem.min_partition_size
    maximum = len(problem.vertices) if problem.max_partition_size is None else problem.max_partition_size
    size_violations = []
    for index, count in enumerate((side_zero, side_one)):
        under = model.new_int_var(0, len(problem.vertices), f"side_{index}_under")
        over = model.new_int_var(0, len(problem.vertices), f"side_{index}_over")
        model.add(under >= minimum - count)
        model.add(over >= count - maximum)
        size_violations.extend((under, over))
    objective.add("partition_size_bounds", sum(size_violations), 2 * len(problem.vertices))
    imbalance = model.new_int_var(0, len(problem.vertices), "partition_imbalance")
    model.add_abs_equality(imbalance, side_one - side_zero)
    objective.add("partition_balance", imbalance, len(problem.vertices))
    objective.add("maximize_cut_weight", sum(cut), sum(edge.weight for edge in problem.edges), reward=True)
    objective.minimize()
    solver = solve_model(model, time_limit_seconds=time_limit_seconds, random_seed=random_seed, num_workers=num_workers)
    result = problem.model_copy(deep=True)
    result.partition = {vertex: solver.value(variable) for vertex, variable in side.items()}
    calculate_score(result, selected)
    return result
