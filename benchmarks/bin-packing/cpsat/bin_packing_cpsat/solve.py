"""Exact CP-SAT formulation for one-dimensional bin packing."""

from __future__ import annotations

from collections.abc import Iterable, Mapping

from bin_packing import calculate_score, constraint_costs
from bin_packing import constraints as domain_constraints
from bin_packing.constraints import Constraint
from bin_packing.models import BinPackingProblemModel
from cpsat_plugins import ConstraintConfig
from ortools.sat.python import cp_model

from .constraints import REGISTRY, BinPackingContext

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def _selection_payload(constraints):
    if constraints is None:
        return None
    payload = []
    for value in constraints:
        if isinstance(value, str):
            row = {"name": value}
        elif isinstance(value, Mapping):
            row = dict(value)
        else:
            row = {
                "name": value.name,
                "weight": value.weight,
                "hard": value.hard,
                "exponent": getattr(value, "exponent", 1.0),
            }
        exponent = row.pop("exponent", 1.0)
        if exponent != 1.0:
            raise ValueError(
                f"constraint {row.get('name')!r} exponent is not supported by CP-SAT"
            )
        payload.append({key: item for key, item in row.items() if item is not None})
    return payload


def _score_constraints(payload):
    if payload is None:
        return None
    classes = {
        "bin_capacity": domain_constraints.BinCapacity,
        "color_compatibility": domain_constraints.ColorCompatibility,
        "item_conflicts": domain_constraints.ItemConflicts,
        "minimize_bins": domain_constraints.MinimizeBins,
    }
    selected = []
    for row in payload:
        if not row.get("enabled", True):
            continue
        name = row["name"]
        if name not in classes:
            raise ValueError(
                f"unknown constraint {name!r}; available: {sorted(classes)}"
            )
        selected.append(classes[name](weight=row.get("weight"), hard=row.get("hard")))
    return selected


def solve(
    problem: BinPackingProblemModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
) -> BinPackingProblemModel:
    selection_payload = _selection_payload(constraints)
    item_count = len(problem.items)
    if item_count == 0:
        core = problem.to_core()
        calculate_score(core)
        return BinPackingProblemModel.from_core(core)

    model = cp_model.CpModel()
    assignments = {
        (item, bin_index): model.new_bool_var(f"item_{item}_bin_{bin_index}")
        for item in range(item_count)
        for bin_index in range(item_count)
    }
    used = [
        model.new_bool_var(f"bin_{bin_index}_used") for bin_index in range(item_count)
    ]
    overflow = []
    color_excess = []
    conflict_violations = []

    for item in range(item_count):
        model.add_exactly_one(
            assignments[item, bin_index] for bin_index in range(item_count)
        )
    for bin_index in range(item_count):
        members = [assignments[item, bin_index] for item in range(item_count)]
        model.add_max_equality(used[bin_index], members)
        load = sum(
            problem.items[item].weight * assignments[item, bin_index]
            for item in range(item_count)
        )
        excess = model.new_int_var(
            0, sum(item.weight for item in problem.items), f"bin_{bin_index}_overflow"
        )
        model.add(excess >= load - problem.bin_capacity)
        overflow.append(excess)
        if bin_index + 1 < item_count:
            model.add(used[bin_index] >= used[bin_index + 1])
    colors = sorted({item.color for item in problem.items if item.color is not None})
    for bin_index in range(item_count):
        for color in colors:
            count = sum(
                assignments[item, bin_index]
                for item in range(item_count)
                if problem.items[item].color == color
            )
            excess = model.new_int_var(
                0, item_count, f"bin_{bin_index}_color_{color}_excess"
            )
            model.add(excess >= count - 1)
            color_excess.append(excess)
    item_by_id = {item.id: index for index, item in enumerate(problem.items)}
    for pair_index, pair in enumerate(problem.conflicts):
        left = item_by_id[pair.first]
        right = item_by_id[pair.second]
        same_bin_terms = []
        for bin_index in range(item_count):
            together = model.new_bool_var(f"conflict_{pair_index}_bin_{bin_index}")
            model.add_multiplication_equality(
                together,
                [assignments[left, bin_index], assignments[right, bin_index]],
            )
            same_bin_terms.append(together)
        violation = model.new_bool_var(f"conflict_{pair_index}_violated")
        model.add(violation == sum(same_bin_terms))
        conflict_violations.append(violation)
    model.add(assignments[0, 0] == 1)

    objective = REGISTRY.objective(
        model,
        ("hard", "soft"),
        selection_payload,
        default_weights={
            "bin_capacity": problem.capacity_weight,
            "color_compatibility": problem.color_weight,
            "item_conflicts": problem.conflict_weight,
            "minimize_bins": problem.used_bin_weight,
        },
    )
    REGISTRY.build(
        BinPackingContext(
            model,
            problem,
            assignments,
            used,
            overflow,
            color_excess,
            conflict_violations,
        ),
        objective,
    )
    objective.minimize()

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = solver.solve(model)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )

    solved = problem.model_copy(deep=True)
    for item_index, item in enumerate(solved.items):
        item.bin = next(
            bin_index
            for bin_index in range(item_count)
            if solver.value(assignments[item_index, bin_index])
        )

    core = solved.to_core()
    calculate_score(core)
    result = BinPackingProblemModel.from_core(core)
    scoring_constraints = _score_constraints(selection_payload)
    if scoring_constraints == []:
        result.score_hard = 0
        result.score_soft = 0.0
    else:
        selected_score = domain_constraints.score(
            constraint_costs(core), scoring_constraints
        )
        result.score_hard = selected_score.hard
        result.score_soft = selected_score.soft
    return result
