"""Full hard/medium/soft CP-SAT model for meeting scheduling."""

from __future__ import annotations

from collections import Counter
from collections.abc import Iterable, Mapping

from cpsat_plugins import ConstraintConfig
from meeting_scheduling import calculate_score, constraint_costs
from meeting_scheduling.constraints import Constraint
from meeting_scheduling.models import MeetingScheduleModel
from ortools.sat.python import cp_model

from .constraints import MeetingContext, REGISTRY

__all__ = ["catalog", "solve"]


def catalog() -> list[dict]:
    return REGISTRY.catalog()


def solve(
    problem: MeetingScheduleModel,
    *,
    time_limit_seconds: float = 30.0,
    random_seed: int = 0,
    num_workers: int = 1,
    constraints: Iterable[ConstraintConfig | Constraint | str | Mapping] | None = None,
    _fixed_grain_rooms: list[int] | None = None,
    _proxy_score: dict[str, int] | None = None,
) -> MeetingScheduleModel:
    if problem.meeting_assignments and (not problem.time_grains or not problem.rooms):
        raise ValueError("time_grains and rooms are required when assignments exist")
    model = cp_model.CpModel()
    room_count = len(problem.rooms)
    values = [
        (grain, room)
        for grain in range(len(problem.time_grains))
        for room in range(room_count)
    ]
    x = {
        (assignment, grain, room): model.new_bool_var(
            f"assignment_{assignment}_grain_{grain}_room_{room}"
        )
        for assignment in range(len(problem.meeting_assignments))
        for grain, room in values
    }
    for assignment, assignment_data in enumerate(problem.meeting_assignments):
        model.add_exactly_one(x[assignment, grain, room] for grain, room in values)
        if _fixed_grain_rooms is not None:
            if len(_fixed_grain_rooms) != len(problem.meeting_assignments):
                raise ValueError(
                    "_fixed_grain_rooms must contain one value per assignment"
                )
            grain, room = divmod(_fixed_grain_rooms[assignment], room_count)
            if (grain, room) not in values:
                raise ValueError("_fixed_grain_rooms contains an invalid value")
            model.add(x[assignment, grain, room] == 1)
        if assignment_data.pinned:
            if assignment_data.grain_room is None:
                raise ValueError(
                    f"pinned assignment {assignment_data.id!r} needs grain_room"
                )
            grain, room = divmod(assignment_data.grain_room, room_count)
            if (grain, room) not in values:
                raise ValueError(
                    f"pinned assignment {assignment_data.id!r} has invalid grain_room"
                )
            model.add(x[assignment, grain, room] == 1)

    grain_indices = [grain.grain_index for grain in problem.time_grains]
    days_at_index: dict[int, list[int]] = {}
    for grain in problem.time_grains:
        days_at_index.setdefault(grain.grain_index, []).append(grain.day_of_year)
    larger_room_penalty = [
        sum(max(0, other.capacity - room.capacity) for other in problem.rooms)
        for room in problem.rooms
    ]
    assignment_meetings = [
        problem.meetings[item.meeting] for item in problem.meeting_assignments
    ]
    required_counts = [
        Counter(meeting.required_attendees) for meeting in problem.meetings
    ]
    preferred_counts = [
        Counter(meeting.preferred_attendees) for meeting in problem.meetings
    ]
    objective = REGISTRY.objective(model, ("hard", "medium", "soft"), constraints)
    REGISTRY.build(
        MeetingContext(
            model,
            problem,
            x,
            values,
            grain_indices,
            days_at_index,
            larger_room_penalty,
            assignment_meetings,
            required_counts,
            preferred_counts,
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
    for assignment, assignment_data in enumerate(solved.meeting_assignments):
        grain, room = next(
            (grain, room)
            for grain, room in values
            if solver.value(x[assignment, grain, room])
        )
        assignment_data.grain_room = grain * room_count + room
    core = solved.to_core()
    calculate_score(core)
    result = MeetingScheduleModel.from_core(core)
    selected_score = objective.score_costs(constraint_costs(core))
    result.score_hard = selected_score["hard"]
    result.score_soft = float(
        selected_score["medium"] * 1_000_000_000 + selected_score["soft"]
    )
    return result
