"""Conversion for quickstart mode only: canonical JSON <-> the quickstart domain (`_core.Timetable`).

The approximate conversion used by `solve(mode="quickstart")` — for full
parity use the canonical conversion (`st_timetabling.convert`, the st::canon
domain). The "unsupported" limits left here are limits of what the quickstart
domain (6 constraints + slotRoom composite encoding) can express; they do not
apply to canonical mode.

Unfolds the canonical problem JSON into the quickstart shape (per Lesson):
each course becomes `periods_per_week` Lessons (ids like "{course_id}#1"),
`teacher_ids[0]` becomes the teacher, and `fixed_slots` are pinned to that
time and a suitable room.

Room mapping strategy
---------------------
The engine knows a single BASIC variable per entity (slotRoom composite
encoding) and has no hard constraint saying "this lesson needs this room
type". Therefore:

- Each class gets a placeholder "home room". Regular lessons (no special
  room) conceptually always happen in the class's own room, so room_id is
  always null in the result (the API_SPEC convention). Should the engine put a
  regular lesson in another room, that is harmless canonically — class clashes
  are already prevented by studentGroupConflict.
- Lessons needing a special room have the real special rooms of that
  room_type as candidates. The engine's roomConflict prevents double use of a
  special room as a hard constraint, but cannot prevent a lesson from landing
  'outside' the special rooms (in a placeholder room, say). So the result
  conversion re-matches each timeslot's lessons to the free special rooms of
  that time (repair) — always safe, since regular lessons occupy no special
  room canonically. When a time has more lessons of a type than rooms of that
  type (the only case the engine cannot prevent), the lesson is reported with
  room_id=null + a note.
- Special rooms of a type no course uses only enlarge the search space and are
  dropped (recorded in notes).

Unsupported constraints (not convertible — stated in notes)
-----------------------------------------------------------
block_periods (consecutive blocks), parallel_group (parallel sections),
forbidden_slots, class unavailability, per-teacher period caps
(max_periods_per_week/day, max_consecutive_periods), preferred_free_days,
preferred/avoided periods per subject, and clash checks for co-teachers in team
teaching. The soft objective is replaced too: instead of the canonical weights
it uses the engine's built-in constraints (minimise teacher room changes,
favour consecutive teacher lessons, spread a class's subjects).
"""

from dataclasses import dataclass, field

from . import _core

__all__ = ["ConvertedProblem", "convert_problem", "convert_solution"]

# room_type of the placeholder home rooms (an internal marker no real special-room type uses).
CLASS_ROOM_TYPE = "__class_home_room__"


@dataclass
class ConvertedProblem:
    """Result of convert_problem: the engine solution + the metadata the reverse conversion needs."""

    table: "_core.Timetable"
    timeslots: list[tuple[int, int]]  # timeslot index -> (day, period)
    slot_index: dict[tuple[int, int], int]  # (day, period) -> timeslot index
    room_ids: list[str | None]  # room index -> canonical room id (None = placeholder room)
    room_types: list[str]  # room index -> room_type
    lesson_courses: list[str]  # lesson index -> course id
    courses_by_id: dict[str, dict]
    subjects_by_id: dict[str, dict]
    notes: list[str] = field(default_factory=list)


def _slot_key(slot: dict) -> tuple[int, int]:
    return (int(slot["day"]), int(slot["period"]))


def convert_problem(problem: dict) -> ConvertedProblem:
    """Converts a canonical TimetableProblem dict into an engine Timetable.

    Reference errors (unknown subject/class/special room) raise ValueError — the
    same layer of failure as the canonical pydantic validation (HTTP 422).
    """
    notes: list[str] = []

    # --- timeslots: periods per weekday -> (day, period) grid ---
    periods_per_day = {int(day): int(count) for day, count in problem["calendar"]["periods_per_day"].items()}
    timeslots = [(day, period) for day in sorted(periods_per_day) for period in range(1, periods_per_day[day] + 1)]
    slot_index = {slot: i for i, slot in enumerate(timeslots)}

    subjects_by_id = {s["id"]: s for s in problem["subjects"]}
    class_groups = list(problem["class_groups"])
    class_ids = {cg["id"] for cg in class_groups}
    courses = list(problem["courses"])
    courses_by_id = {c["id"]: c for c in courses}

    def required_room_type(course: dict) -> str | None:
        subject = subjects_by_id.get(course["subject_id"])
        if subject is None:
            raise ValueError(f"[{course['id']}] unknown subject: {course['subject_id']}")
        return subject.get("required_room_type")

    # --- rooms: placeholder home room per class + the special rooms actually needed ---
    rooms: list[_core.Room] = []
    room_ids: list[str | None] = []
    room_types: list[str] = []
    homeroom_index: dict[str, int] = {}
    for cg in class_groups:
        homeroom_index[cg["id"]] = len(rooms)
        rooms.append(_core.Room(cg["id"], CLASS_ROOM_TYPE))
        room_ids.append(None)
        room_types.append(CLASS_ROOM_TYPE)

    needed_types = {rt for c in courses if (rt := required_room_type(c)) is not None}
    rooms_of_type: dict[str, list[int]] = {rt: [] for rt in needed_types}
    dropped: list[str] = []
    for room in problem.get("rooms", []):
        if room["room_type"] in needed_types:
            rooms_of_type[room["room_type"]].append(len(rooms))
            rooms.append(_core.Room(room["id"], room["room_type"]))
            room_ids.append(room["id"])
            room_types.append(room["room_type"])
        else:
            dropped.append(room["id"])
    for room_type in sorted(needed_types):
        if not rooms_of_type[room_type]:
            raise ValueError(f"no room of the required type: {room_type}")
    if dropped:
        notes.append(f"rooms dropped because no course uses them: {', '.join(dropped)}")
    if needed_types:
        notes.append(
            "required_room_type is not a hard constraint in the engine: special rooms are "
            "re-matched to free rooms of the same time in the result conversion, and a "
            "shortage is reported as room_id=null + notes"
        )

    # --- lessons: course -> periods_per_week Lessons ---
    lessons: list[_core.Lesson] = []
    lesson_courses: list[str] = []
    pinned_used: set[tuple[int, int]] = set()  # (timeslot index, room index)
    team_teaching: list[str] = []
    blocked: list[str] = []
    parallel: set[str] = set()
    forbidden: list[str] = []

    for course in courses:
        course_id = course["id"]
        group_id = course["class_group_id"]
        if group_id not in class_ids:
            raise ValueError(f"[{course_id}] unknown class: {group_id}")
        room_type = required_room_type(course)
        teacher_ids = list(course["teacher_ids"])
        if len(teacher_ids) > 1:
            team_teaching.append(course_id)
        if course.get("block_periods", 0) >= 2:
            blocked.append(course_id)
        if course.get("parallel_group") is not None:
            parallel.add(course["parallel_group"])
        if course.get("forbidden_slots"):
            forbidden.append(course_id)

        # fixed_slots are pinned to the Lessons from the first one on, in (day, period) order.
        fixed = sorted(_slot_key(s) for s in course.get("fixed_slots", []))
        for k in range(1, int(course["periods_per_week"]) + 1):
            lesson = _core.Lesson(f"{course_id}#{k}", course["subject_id"], teacher_ids[0], group_id)
            if k <= len(fixed):
                slot = fixed[k - 1]
                if slot not in slot_index:
                    raise ValueError(f"[{course_id}] fixed_slot not in the calendar: {slot}")
                ts = slot_index[slot]
                if room_type is None:
                    room = homeroom_index[group_id]
                else:
                    candidates = rooms_of_type[room_type]
                    room = next(
                        (r for r in candidates if (ts, r) not in pinned_used),
                        candidates[0],
                    )
                if (ts, room) in pinned_used:
                    notes.append(
                        f"{course_id}: no free room for fixed_slot {slot}, so pinned "
                        "lessons overlap (the input itself is overbooked)"
                    )
                pinned_used.add((ts, room))
                lesson.slot_room = ts * len(rooms) + room
                lesson.pinned = True
            lessons.append(lesson)
            lesson_courses.append(course_id)

    # --- teacher unavailability (hard 4 extension) ---
    teacher_unavailable: dict[str, set[int]] = {}
    caps: list[str] = []
    free_days: list[str] = []
    for teacher in problem.get("teachers", []):
        slots = {slot_index[key] for s in teacher.get("unavailable", []) if (key := _slot_key(s)) in slot_index}
        if slots:
            teacher_unavailable[teacher["id"]] = slots
        if teacher.get("max_periods_per_week") is not None:
            caps.append(teacher["id"])
        if teacher.get("preferred_free_days"):
            free_days.append(teacher["id"])

    # --- record the constraints the conversion cannot express in notes ---
    if team_teaching:
        notes.append(
            f"team-taught courses use only teacher_ids[0] as the teacher (co-teacher clashes not checked): {', '.join(team_teaching)}"
        )
    if blocked:
        notes.append(f"block_periods (consecutive blocks) not supported — ignored: {', '.join(blocked)}")
    if parallel:
        notes.append(
            f"parallel_group (parallel sections) not supported — courses are placed independently and each takes the class's cell: {', '.join(sorted(parallel))}"
        )
    if forbidden:
        notes.append(f"forbidden_slots not supported — ignored: {', '.join(forbidden)}")
    unavailable_groups = [cg["id"] for cg in class_groups if cg.get("unavailable")]
    if unavailable_groups:
        notes.append(f"class unavailability not supported — ignored: {', '.join(unavailable_groups)}")
    if caps:
        notes.append(
            f"per-teacher period caps (max_periods_per_week/day, max_consecutive_periods) not supported — ignored: {', '.join(caps)}"
        )
    if free_days:
        notes.append(f"preferred_free_days not supported — ignored: {', '.join(free_days)}")
    if any(s.get("preferred_periods") or s.get("avoided_periods") for s in problem["subjects"]):
        notes.append("preferred/avoided periods per subject not supported — ignored")
    notes.append(
        "the engine backend's soft objective uses built-in constraints instead of the canonical weights "
        "(minimise teacher room changes · favour consecutive teacher lessons · spread a class's subjects)"
    )

    table = _core.Timetable(
        [_core.Timeslot(day, period) for day, period in timeslots],
        rooms,
        lessons,
        teacher_unavailable,
    )
    return ConvertedProblem(
        table=table,
        timeslots=timeslots,
        slot_index=slot_index,
        room_ids=room_ids,
        room_types=room_types,
        lesson_courses=lesson_courses,
        courses_by_id=courses_by_id,
        subjects_by_id=subjects_by_id,
        notes=notes,
    )


def convert_solution(converted: ConvertedProblem, *, wall_time_seconds: float = 0.0) -> dict:
    """Turns the engine result (`converted.table`, mutated by solve) back into a
    canonical Timetable JSON (dict). Lessons are regrouped per course, and
    teacher_ids get each source course's full tuple back.

    Special-room repair: the engine cannot hard-prevent placements outside the
    special rooms, so each timeslot's special-room lessons are matched again —
    a lesson already in a room of the right type keeps it, the rest get free
    special rooms. Regular lessons have room_id=null (their own room) and take
    no special room. A shortage at some time is reported as room_id=null +
    notes."""
    table = converted.table
    score = table.score
    room_count = len(converted.room_ids)
    notes = list(converted.notes)
    assignment = table.assignment()

    def required_of(index: int) -> str | None:
        course = converted.courses_by_id[converted.lesson_courses[index]]
        return converted.subjects_by_id[course["subject_id"]].get("required_room_type")

    rooms_of_type: dict[str, list[int]] = {}
    for room_index, room_type in enumerate(converted.room_types):
        if room_type != CLASS_ROOM_TYPE:
            rooms_of_type.setdefault(room_type, []).append(room_index)

    # Group special-room lessons per (timeslot, required type) and match rooms.
    special: dict[tuple[int, str], list[int]] = {}
    for index, slot_room in enumerate(assignment):
        if slot_room < 0:
            continue
        required = required_of(index)
        if required is not None:
            special.setdefault((slot_room // room_count, required), []).append(index)

    repaired_room: dict[int, int | None] = {}  # lesson index -> room index
    for (_, required), members in special.items():
        taken: set[int] = set()
        pending: list[int] = []
        for index in members:  # pass 1: keep a room of the right type it already has
            room_index = assignment[index] % room_count
            if converted.room_types[room_index] == required and room_index not in taken:
                repaired_room[index] = room_index
                taken.add(room_index)
            else:
                pending.append(index)
        free = [r for r in rooms_of_type[required] if r not in taken]
        for index in pending:  # pass 2: re-match to free special rooms; report a shortage
            repaired_room[index] = free.pop(0) if free else None

    lessons_out: list[dict] = []
    for index, slot_room in enumerate(assignment):
        course = converted.courses_by_id[converted.lesson_courses[index]]
        if slot_room < 0:
            notes.append(f"{course['id']}: unassigned lessons left (search failure)")
            continue
        day, period = converted.timeslots[slot_room // room_count]

        # room_id is filled only for lessons needing a special room. Regular lessons
        # are always null (their own room) — the API_SPEC convention.
        room_id = None
        required = required_of(index)
        if required is not None:
            room_index = repaired_room[index]
            if room_index is not None:
                room_id = converted.room_ids[room_index]
            else:
                notes.append(
                    f"{course['id']}: no special room ({required}) "
                    f"(day {day} period {period}) — no free {required} at that time"
                )
        lessons_out.append(
            {
                "course_id": course["id"],
                "class_group_id": course["class_group_id"],
                "subject_id": course["subject_id"],
                "teacher_ids": list(course["teacher_ids"]),
                "slot": {"day": day, "period": period},
                "room_id": room_id,
            }
        )

    # Local search proves neither optimality nor infeasibility: hard 0 is FEASIBLE,
    # anything else UNKNOWN (the assignment is still returned, explained in notes).
    feasible = score.hard == 0
    if not feasible:
        notes.append(f"hard violations left: {score}")
    return {
        "status": "FEASIBLE" if feasible else "UNKNOWN",
        "lessons": lessons_out,
        # The negated engine soft score (approximates the penalty sum). Its unit
        # differs from the canonical objective — see the soft-objective entry in notes.
        "objective_value": -score.soft,
        "best_objective_bound": None,
        "wall_time_seconds": wall_time_seconds,
        "penalties": {},
        "notes": notes,
    }
