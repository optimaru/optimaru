"""Lexicographic CP-SAT formulation of the bed-allocation score."""

from __future__ import annotations

from collections.abc import Iterable, Mapping

from bed_allocation import calculate_score, constraint_costs
from bed_allocation.constraints import Constraint
from bed_allocation.models import BedPlanModel
from cpsat_plugins import ConstraintConfig
from ortools.sat.python import cp_model

from .constraints import BedContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: BedPlanModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    _fixed_beds: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> BedPlanModel:
    model = cp_model.CpModel()
    stay_count, bed_count = len(problem.stays), len(problem.beds)
    no_bed = bed_count
    x = {
        (stay, bed): model.new_bool_var(f"stay_{stay}_bed_{bed}")
        for stay in range(stay_count)
        for bed in range(bed_count + 1)
    }
    for stay in range(stay_count):
        model.add_exactly_one(x[stay, bed] for bed in range(bed_count + 1))
    if _fixed_beds is not None:
        if len(_fixed_beds) != stay_count:
            raise ValueError("_fixed_beds must contain one value per stay")
        for stay, bed in enumerate(_fixed_beds):
            if not 0 <= bed <= no_bed:
                raise ValueError("_fixed_beds contains an invalid bed")
            model.add(x[stay, bed] == 1)

    rooms = {room.id: room for room in problem.rooms}
    departments = {department.id: department for department in problem.departments}
    room_of_bed = [rooms[bed.room_id] for bed in problem.beds]
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints)
    REGISTRY.build(
        BedContext(model, problem, x, no_bed, room_of_bed, departments), objective
    )

    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = time_limit_seconds
    solver.parameters.random_seed = random_seed
    solver.parameters.num_search_workers = max(1, num_workers)
    status = objective.solve(solver)
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        raise RuntimeError(
            f"CP-SAT did not find a solution (status={solver.status_name(status)})"
        )
    if _proxy_score is not None:
        _proxy_score.update(objective.score(solver))

    solved = problem.model_copy(deep=True)
    for stay, stay_data in enumerate(solved.stays):
        value = next(bed for bed in range(bed_count + 1) if solver.value(x[stay, bed]))
        stay_data.bed_id = None if value == no_bed else solved.beds[value].id
    core = solved.to_core()
    assignment = [
        no_bed
        if stay.bed_id is None
        else next(
            index for index, bed in enumerate(solved.beds) if bed.id == stay.bed_id
        )
        for stay in solved.stays
    ]
    core.set_assignment(assignment)
    calculate_score(core)
    result = BedPlanModel.from_core(core)
    selected_score = objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_medium = selected_score["medium"]
    result.score_soft = selected_score["soft"]
    return result
