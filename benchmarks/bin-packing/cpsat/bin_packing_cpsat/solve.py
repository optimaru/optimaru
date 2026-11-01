"""Exact CP-SAT formulation for one-dimensional bin packing.

The returned object uses the canonical ``BinPackingProblemModel`` JSON contract.
Every item goes in exactly one of the bins 0..n-1 (n = item count, the engine's
value range too); hard costs are optimized lexicographically before the soft
ones, matching the engine's ``HardSoftScore``, and the result is re-scored by the
shared C++ evaluator (``constraint_costs``) under the same selection.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass

from bin_packing import calculate_score, constraint_costs
from bin_packing.constraints import Constraint
from bin_packing.models import BinPackingProblemModel
from cpsat_plugins import ConstraintConfig, ObjectiveBuilder
from ortools.sat.python import cp_model

from .constraints import REGISTRY, BinPackingContext

__all__ = ["Built", "build", "catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


@dataclass(frozen=True)
class Built:
    """A built model: the CP-SAT model, its variables and the selected objective."""

    model: cp_model.CpModel
    context: BinPackingContext
    objective: ObjectiveBuilder


def build(
    problem: BinPackingProblemModel,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    *,
    symmetry_breaking: bool = True,
) -> Built:
    """Build the CP-SAT model of `problem` under a constraint selection (None = the defaults).

    Bins are interchangeable, so ``symmetry_breaking`` puts item 0 in bin 0 and makes
    the used bins a prefix 0..k-1 (every constraint is invariant under relabelling
    bins, so this loses no optimum); pass False to pin an arbitrary assignment.
    """
    model = cp_model.CpModel()
    count = len(problem.items)
    total_weight = sum(item.weight for item in problem.items)
    assignments = {
        (item, bin_index): model.new_bool_var(f"item_{item}_bin_{bin_index}")
        for item in range(count)
        for bin_index in range(count)
    }
    used = [model.new_bool_var(f"bin_{bin_index}_used") for bin_index in range(count)]
    overflow = []
    for item in range(count):
        model.add_exactly_one(assignments[item, bin_index] for bin_index in range(count))
    for bin_index in range(count):
        model.add_max_equality(used[bin_index], [assignments[item, bin_index] for item in range(count)])
        load = sum(problem.items[item].weight * assignments[item, bin_index] for item in range(count))
        excess = model.new_int_var(0, total_weight, f"bin_{bin_index}_overflow")
        model.add(excess >= load - problem.bin_capacity)
        overflow.append(excess)
        if symmetry_breaking and bin_index + 1 < count:
            model.add(used[bin_index] >= used[bin_index + 1])
    if symmetry_breaking and count:
        model.add(assignments[0, 0] == 1)

    # Only a colour two or more items carry can conflict (as the engine's colour classes).
    members: dict[str, list[int]] = {}
    for index, item in enumerate(problem.items):
        if item.color is not None:
            members.setdefault(item.color, []).append(index)
    shared = {color: items for color, items in members.items() if len(items) >= 2}
    color_excess = []
    for bin_index in range(count):
        for color, items in sorted(shared.items()):
            excess = model.new_int_var(0, len(items) - 1, f"bin_{bin_index}_color_{color}_excess")
            model.add(excess >= sum(assignments[item, bin_index] for item in items) - 1)
            color_excess.append(excess)
    color_bound = sum(len(items) - 1 for items in shared.values())

    item_by_id = {item.id: index for index, item in enumerate(problem.items)}
    conflict_violations = []
    for pair_index, pair in enumerate(problem.conflicts):
        left = item_by_id[pair.first]
        right = item_by_id[pair.second]
        together = []
        for bin_index in range(count):
            both = model.new_bool_var(f"conflict_{pair_index}_bin_{bin_index}")
            model.add_multiplication_equality(both, [assignments[left, bin_index], assignments[right, bin_index]])
            together.append(both)
        violation = model.new_bool_var(f"conflict_{pair_index}_violated")
        model.add(violation == sum(together))
        conflict_violations.append(violation)

    # Default weights and levels come from the engine (constraint_info()); the
    # canonical problem carries none.
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    context = BinPackingContext(
        model, problem, assignments, used, overflow, color_excess, color_bound, conflict_violations
    )
    REGISTRY.build(context, objective)
    return Built(model, context, objective)


def solve(
    problem: BinPackingProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> BinPackingProblemModel:
    """Solve ``problem`` without mutating it and return a scored solution."""
    built = build(problem, constraints)
    solved = problem.model_copy(deep=True)
    solved.warm_start = False
    if problem.items:
        solver = cp_model.CpSolver()
        solver.parameters.max_time_in_seconds = time_limit_seconds
        solver.parameters.random_seed = random_seed
        solver.parameters.num_search_workers = max(1, num_workers)
        status = built.objective.solve(solver)
        if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
            raise RuntimeError(f"CP-SAT did not find a solution (status={solver.status_name(status)})")
        count = len(problem.items)
        for index, item in enumerate(solved.items):
            item.bin = next(b for b in range(count) if solver.value(built.context.assignments[index, b]))

    core = solved.to_core()
    calculate_score(core)
    result = BinPackingProblemModel.from_core(core)
    selected_score = built.objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(selected_score["soft"])
    return result
