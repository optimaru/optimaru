"""Assigning special rooms after solving.

The default CP-SAT model guarantees "demand <= rooms per (special-room type,
time)" through H6, and which lesson uses which room is decided here after
solving. Rooms of one type are interchangeable, so a per-time greedy pass
always succeeds. When a plugin assembly drops H6, rooms are reused so even a
violating solution can be serialized — otherwise switching a constraint off
would make the post-processing fail, and the optional-constraint API would not
really work.

The same course preferably gets the same room every time: rooms it already
used are tried first (a soft preference only, never a failure condition).
"""

from collections import defaultdict

from teacher_scheduling.domain.entities import TimeSlot
from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson


def assign_rooms(problem: TimetableProblem, lessons: list[ScheduledLesson]) -> list[ScheduledLesson]:
    """Returns a new lesson list with room_id filled for lessons needing a special room."""
    # Collect the indices of the lessons to place per (special-room type, time).
    demands: dict[tuple[str, TimeSlot], list[int]] = defaultdict(list)
    for index, lesson in enumerate(lessons):
        room_type = problem.required_room_type(problem.course_by_id[lesson.course_id])
        if room_type is not None:
            demands[room_type, lesson.slot].append(index)

    assigned = list(lessons)
    preferred: dict[str, str] = {}  # the room a course used last; tried first next time
    for (room_type, _slot), indices in sorted(
        demands.items(), key=lambda kv: (kv[0][0], kv[0][1].day, kv[0][1].period)
    ):
        rooms = [room.id for room in problem.rooms_by_type[room_type]]
        remaining = sorted(indices, key=lambda i: assigned[i].course_id)
        free = list(rooms)
        # Pass 1: keep the previous room when it is free.
        for index in list(remaining):
            room_id = preferred.get(assigned[index].course_id)
            if room_id in free:
                free.remove(room_id)
                remaining.remove(index)
                assigned[index] = assigned[index].model_copy(update={"room_id": room_id})
        # Pass 2: the remaining lessons take the remaining rooms in order. If H6 was dropped
        # from the assembly, free can run out first, so the room list is cycled after that.
        # Same-time/room clashes then remain visible in the result, preserving the meaning
        # of the constraint the caller switched off.
        choices = free + [rooms[i % len(rooms)] for i in range(max(0, len(remaining) - len(free)))] if rooms else []
        for index, room_id in zip(remaining, choices, strict=False):
            preferred[assigned[index].course_id] = room_id
            assigned[index] = assigned[index].model_copy(update={"room_id": room_id})

    return assigned
