"""CP-SAT formulation for conference talk/room assignment."""

from __future__ import annotations

from typing import Any

from conference_scheduling import constraint_costs
from conference_scheduling.constraints import Constraint
from conference_scheduling.models import ConferenceScheduleModel
from cpsat_plugins import ConstraintConfig
from ortools.sat.python import cp_model

from .constraints import ConferenceContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: ConferenceScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: list[ConstraintConfig | Constraint | str | dict[str, Any]]
    | None = None,
    _fixed_slot_rooms: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> ConferenceScheduleModel:
    if problem.talks and (not problem.timeslots or not problem.rooms):
        raise ValueError("talks require at least one timeslot and room")
    model = cp_model.CpModel()
    objective = REGISTRY.objective(model, ("hard", "soft"), constraints)
    talk_count, slot_count, room_count = (
        len(problem.talks),
        len(problem.timeslots),
        len(problem.rooms),
    )
    x = {
        (talk, slot, room): model.new_bool_var(f"talk_{talk}_slot_{slot}_room_{room}")
        for talk in range(talk_count)
        for slot in range(slot_count)
        for room in range(room_count)
    }
    by_slot = {}
    by_room = {}
    for talk, talk_data in enumerate(problem.talks):
        model.add_exactly_one(
            x[talk, slot, room]
            for slot in range(slot_count)
            for room in range(room_count)
        )
        if talk_data.pinned:
            if not 0 <= talk_data.slot_room < slot_count * room_count:
                raise ValueError(
                    f"pinned talk {talk_data.code!r} needs a valid slot_room"
                )
            model.add(
                x[
                    talk,
                    talk_data.slot_room // room_count,
                    talk_data.slot_room % room_count,
                ]
                == 1
            )
        if _fixed_slot_rooms is not None:
            if len(_fixed_slot_rooms) != talk_count:
                raise ValueError("_fixed_slot_rooms must contain one value per talk")
            value = _fixed_slot_rooms[talk]
            if not 0 <= value < slot_count * room_count:
                raise ValueError("_fixed_slot_rooms contains an invalid value")
            model.add(x[talk, value // room_count, value % room_count] == 1)
        for slot in range(slot_count):
            slot_literal = model.new_bool_var(f"talk_{talk}_slot_{slot}")
            model.add(
                slot_literal == sum(x[talk, slot, room] for room in range(room_count))
            )
            by_slot[talk, slot] = slot_literal
        for room in range(room_count):
            room_literal = model.new_bool_var(f"talk_{talk}_room_{room}")
            model.add(
                room_literal == sum(x[talk, slot, room] for slot in range(slot_count))
            )
            by_room[talk, room] = room_literal

    REGISTRY.build(
        ConferenceContext(
            model=model,
            problem=problem,
            x=x,
            by_slot=by_slot,
            by_room=by_room,
        ),
        objective,
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
    for talk, talk_data in enumerate(solved.talks):
        slot, room = next(
            (slot, room)
            for slot in range(slot_count)
            for room in range(room_count)
            if solver.value(x[talk, slot, room])
        )
        talk_data.slot_room = slot * room_count + room
    score = objective.score_costs(constraint_costs(solved.to_core()))
    solved.score_hard = score["hard"]
    solved.score_soft = score["soft"]
    return solved
