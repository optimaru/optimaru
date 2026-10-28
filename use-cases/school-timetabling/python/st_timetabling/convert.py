"""Canonical `TimetableProblem` JSON (dict) <-> st::canon domain conversion (full parity).

Moves the canonical problem JSON of API_SPEC.md into the canonical C++
domain (`_core.canon`). st::canon implements hard H1–H10 / soft S1–S6 of the
CP-SAT reference package (teacher_scheduling in benchmarks/school-timetabling/cpsat)
with validate.py's semantics, so through this conversion the engine backend
honours every canonical constraint as well — the approximate quickstart
conversion lives in `st_timetabling.quickstart`.

Conversion rules
----------------
- Occupancy group: courses sharing a `parallel_group` form one merged group;
  every other course is a group of its own. A group becomes as many engine
  Lessons ("{key}#k") as its weekly periods; its teachers are the concatenated
  teacher_ids of the member courses (duplicates kept — a teacher taking two
  parallel sections at once then shows up as a hard clash), its special-room
  demand is the members' sum and its class occupancy is 1 (same as cpsat's
  Course.occupancy_key). H5 (parallel sections run together) holds by the
  merge itself.
- S5/S6 (avoided/preferred periods per subject) count one unit per member
  course through Lesson.preference_units, matching validate.py's per-lesson
  counts exactly.
- `fixed_slots` -> pinned Lesson (H2: the engine never moves it).
- `forbidden_slots` + class/teacher `unavailable` -> per-lesson allowed_slot
  table (H5, hard).
- `block_periods` >= 2 -> blockGroup on the group's last block_periods Lessons
  (H8: consecutive periods on one day).
- `required_room_type` -> roomTypeDemand + roomCapacityByType (H6, hard).
  Concrete rooms are chosen afterwards by a greedy pass (convert_solution), as
  in cpsat — H6 being hard, it always succeeds on a hard-0 solution.
- Per-teacher caps (max_periods_per_week/day, max_consecutive_periods),
  preferred_free_days and the per-course daily cap (max_periods_per_day) are
  all passed through.

Input errors (unknown subject/class/teacher/special room, contradictory fixed
times, ...) raise ValueError — the same layer of failure as the canonical
pydantic validation (HTTP 422). A parallel group spanning different classes
cannot be expressed as a merge and also raises ValueError (by definition a
parallel group splits one class, so valid input never has one).
"""

from dataclasses import dataclass, field

from ._core import canon

__all__ = [
    "ConvertedProblem",
    "OccupancyGroup",
    "convert_problem",
    "convert_solution",
    "inject_solution",
]


@dataclass
class OccupancyGroup:
    """One occupancy group spread over engine Lessons (a parallel group or a single course)."""

    key: str  # parallel_group name or course id
    course_ids: list[str]  # member courses (one unless it is a parallel group)
    class_group_id: str
    periods_per_week: int
    block_periods: int  # 0/1 = no block
    fixed: list[tuple[int, int]]  # sorted fixed (day, period); pinned from the first Lesson on
    lesson_indices: list[int]  # engine lesson indices (from "#1" in order)


@dataclass
class ConvertedProblem:
    """Result of convert_problem: the engine solution + the metadata the reverse conversion needs."""

    table: "canon.Timetable"
    timeslots: list[tuple[int, int]]  # slot index -> (day, period)
    slot_index: dict[tuple[int, int], int]  # (day, period) -> slot index
    groups: list[OccupancyGroup]
    courses_by_id: dict[str, dict]
    subjects_by_id: dict[str, dict]
    room_ids_by_type: dict[str, list[str]]  # room_type -> canonical room ids
    notes: list[str] = field(default_factory=list)


def _slot_key(slot: dict) -> tuple[int, int]:
    return (int(slot["day"]), int(slot["period"]))


def _period_mask(periods) -> int:
    """1-based periods -> 16-bit mask (periods outside the calendar are impossible and ignored)."""
    mask = 0
    for period in periods or ():
        if 1 <= int(period) <= 16:
            mask |= 1 << (int(period) - 1)
    return mask


def convert_problem(problem: dict) -> ConvertedProblem:
    """Converts a canonical TimetableProblem dict into an st::canon Timetable."""
    # --- timeslots: periods per weekday -> (day, period) grid ---
    periods_per_day = {int(day): int(count) for day, count in problem["calendar"]["periods_per_day"].items()}
    if max(periods_per_day.values()) > 16 or max(periods_per_day) > 7:
        # st::canon is designed around bitmasks of periods <= 16, weekdays < 8 (prepare rejects more).
        raise ValueError("the calendar exceeds the engine limits (periods <= 16, weekday index < 8)")
    timeslots = [(day, period) for day in sorted(periods_per_day) for period in range(1, periods_per_day[day] + 1)]
    slot_index = {slot: i for i, slot in enumerate(timeslots)}

    subjects_by_id = {s["id"]: s for s in problem["subjects"]}
    class_groups = list(problem["class_groups"])
    class_index = {cg["id"]: i for i, cg in enumerate(class_groups)}
    teachers = list(problem.get("teachers", []))
    teacher_index = {t["id"]: i for i, t in enumerate(teachers)}
    courses = list(problem["courses"])
    courses_by_id = {c["id"]: c for c in courses}

    def required_room_type(course: dict) -> str | None:
        subject = subjects_by_id.get(course["subject_id"])
        if subject is None:
            raise ValueError(f"[{course['id']}] unknown subject: {course['subject_id']}")
        return subject.get("required_room_type")

    # --- special-room types -> dense index, room counts (H6) ---
    needed_types = sorted({rt for c in courses if (rt := required_room_type(c)) is not None})
    room_ids_by_type: dict[str, list[str]] = {}
    for room in problem.get("rooms", []):
        room_ids_by_type.setdefault(room["room_type"], []).append(room["id"])
    for room_type in needed_types:
        if not room_ids_by_type.get(room_type):
            raise ValueError(f"no room of the required type: {room_type}")
    room_type_index = {room_type: i for i, room_type in enumerate(needed_types)}

    # --- teacher unavailability + period caps (H4 extension / H9 / H10 / S3) ---
    teacher_blocked: list[set[int]] = []
    max_per_day: list[int] = []
    max_per_week: list[int] = []
    max_consecutive: list[int] = []
    free_day_mask: list[int] = []
    for teacher in teachers:
        teacher_blocked.append(
            {slot_index[key] for s in teacher.get("unavailable", []) if (key := _slot_key(s)) in slot_index}
        )
        max_per_day.append(int(teacher.get("max_periods_per_day") or 6))
        weekly = teacher.get("max_periods_per_week")
        max_per_week.append(-1 if weekly is None else int(weekly))
        max_consecutive.append(int(teacher.get("max_consecutive_periods") or 4))
        free_day_mask.append(sum(1 << int(d) for d in set(teacher.get("preferred_free_days", []))))

    class_blocked: list[set[int]] = [
        {slot_index[key] for s in cg.get("unavailable", []) if (key := _slot_key(s)) in slot_index}
        for cg in class_groups
    ]

    # --- occupancy groups: merge parallel_group, every other course is a group of one ---
    members_by_key: dict[str, list[dict]] = {}
    for course in courses:
        if course["class_group_id"] not in class_index:
            raise ValueError(f"[{course['id']}] unknown class: {course['class_group_id']}")
        for teacher_id in course["teacher_ids"]:
            if teacher_id not in teacher_index:
                raise ValueError(f"[{course['id']}] unknown teacher: {teacher_id}")
        key = course.get("parallel_group") or course["id"]
        members_by_key.setdefault(key, []).append(course)

    lessons: list[canon.Lesson] = []
    groups: list[OccupancyGroup] = []
    daily_limits: list[int] = []
    block_group_count = 0
    for group_key, members in members_by_key.items():
        if len({c["class_group_id"] for c in members}) > 1:
            raise ValueError(f"parallel group {group_key} spans different classes (a parallel group splits one class)")
        if len({int(c["periods_per_week"]) for c in members}) > 1:
            raise ValueError(f"parallel group {group_key} has differing weekly periods")
        if len({int(c.get("block_periods", 0)) for c in members}) > 1:
            raise ValueError(f"parallel group {group_key} has differing block periods")

        class_id = members[0]["class_group_id"]
        ppw = int(members[0]["periods_per_week"])
        block = int(members[0].get("block_periods", 0))

        # Teachers: concatenated member teacher_ids (duplicates kept — see the docstring above).
        group_teachers = [teacher_index[t] for c in members for t in c["teacher_ids"]]

        # Special-room demand: the members' sum (H6).
        demand = [0] * len(needed_types)
        for course in members:
            room_type = required_room_type(course)
            if room_type is not None:
                demand[room_type_index[room_type]] += 1

        # allowed_slot: removes the union of forbidden/unavailable times (H5). Same as
        # cpsat's _course_allowed_slots + the intersection over parallel sections.
        blocked: set[int] = set(class_blocked[class_index[class_id]])
        for course in members:
            blocked.update(
                slot_index[key] for s in course.get("forbidden_slots", []) if (key := _slot_key(s)) in slot_index
            )
            for teacher_id in course["teacher_ids"]:
                blocked.update(teacher_blocked[teacher_index[teacher_id]])
        allowed = [i not in blocked for i in range(len(timeslots))]

        # fixed_slots: union over members, sorted by (day, period) -> pinned from the first Lesson on.
        fixed = sorted({_slot_key(s) for c in members for s in c.get("fixed_slots", [])})
        for slot in fixed:
            if slot not in slot_index:
                raise ValueError(f"[{group_key}] fixed_slot not in the calendar: {slot}")
            if not allowed[slot_index[slot]]:
                # Same failure as cpsat _add_fixed_slots: a fixed time contradicts a forbidden/unavailable one.
                raise ValueError(f"[{group_key}] fixed time {slot} clashes with a forbidden/unavailable time")
        if len(fixed) > ppw:
            raise ValueError(f"group {group_key} has more fixed times than weekly periods")

        # H7: minimum over members of the effective daily cap (max(own cap, block size)). A
        # merged group places the same count per day for every member, so the minimum is the whole condition.
        daily_limits.append(min(max(int(c.get("max_periods_per_day", 1)), block) for c in members))

        # S5/S6: one (avoided, preferred) mask unit per member subject.
        units = []
        for course in members:
            subject = subjects_by_id[course["subject_id"]]
            units.append((_period_mask(subject.get("avoided_periods")), _period_mask(subject.get("preferred_periods"))))

        block_group = block_group_count if block >= 2 else -1
        if block >= 2:
            block_group_count += 1

        lesson_indices = []
        for k in range(1, ppw + 1):
            lesson = canon.Lesson(
                f"{group_key}#{k}",
                class_index[class_id],
                len(groups),
                group_teachers,
                room_type_demand=list(demand),
            )
            lesson.allowed_slot = allowed
            lesson.preference_units = units
            if k <= len(fixed):
                lesson.pinned = True
                lesson.slot = slot_index[fixed[k - 1]]
            if block_group >= 0 and k > ppw - block:
                lesson.block_group = block_group
            lesson_indices.append(len(lessons))
            lessons.append(lesson)

        groups.append(
            OccupancyGroup(
                key=group_key,
                course_ids=[c["id"] for c in members],
                class_group_id=class_id,
                periods_per_week=ppw,
                block_periods=block,
                fixed=fixed,
                lesson_indices=lesson_indices,
            )
        )

    table = canon.Timetable([day for day, _ in timeslots], [period for _, period in timeslots], lessons)
    table.teacher_count = len(teachers)
    table.class_count = len(class_groups)
    table.room_capacity_by_type = [len(room_ids_by_type[rt]) for rt in needed_types]
    table.teacher_max_per_day = max_per_day
    table.teacher_max_per_week = max_per_week
    table.teacher_max_consecutive = max_consecutive
    table.teacher_preferred_free_days = free_day_mask
    table.course_daily_limit = daily_limits

    return ConvertedProblem(
        table=table,
        timeslots=timeslots,
        slot_index=slot_index,
        groups=groups,
        courses_by_id=courses_by_id,
        subjects_by_id=subjects_by_id,
        room_ids_by_type=room_ids_by_type,
    )


def inject_solution(converted: ConvertedProblem, lessons: list[dict]) -> None:
    """Injects a canonical assignment produced by an external backend (cpsat, ...) into the engine lesson slots.

    ``lessons`` is a list of dicts shaped like canonical `Timetable.lessons`
    (course_id + slot). A group's slot set is read from its first member course
    (in a valid solution every parallel section shares the slots — H5). Pinned
    Lessons get their own fixed slot, blockGroup Lessons a consecutive window on
    one day (guaranteed to exist by H8 when the input solution is valid), and
    the rest are assigned in (day, period) order. Used both for the parity
    check (engine breakdown vs cpsat validate) and for warm starts.
    """
    slot_index = converted.slot_index
    by_course: dict[str, list[int]] = {}
    for lesson in lessons:
        key = _slot_key(lesson["slot"])
        if key not in slot_index:
            raise ValueError(f"[{lesson['course_id']}] slot not in the calendar: {key}")
        by_course.setdefault(lesson["course_id"], []).append(slot_index[key])

    assignment = converted.table.assignment()  # already holds the pinned slots
    for group in converted.groups:
        pool = sorted(by_course.get(group.course_ids[0], []))
        n = len(group.lesson_indices)
        placed: dict[int, int] = {}  # position within the group -> slot

        # 1) pinned (#1..#f): their own fixed slot.
        for k, slot in enumerate(group.fixed):
            placed[k] = slot_index[slot]
            if placed[k] in pool:
                pool.remove(placed[k])

        # 2) blockGroup (the last block_periods): a consecutive window on one day.
        block = group.block_periods if group.block_periods >= 2 else 0
        block_positions = [k for k in range(n) if block and k >= n - block]
        unpinned_block = [k for k in block_positions if k not in placed]
        if unpinned_block:
            pinned_slots = {placed[k] for k in block_positions if k in placed}
            window = _find_block_window(converted, pool, pinned_slots, block)
            if window is not None:
                free_window = sorted(s for s in window if s not in pinned_slots)
                for k, slot in zip(unpinned_block, free_window):
                    placed[k] = slot
                    pool.remove(slot)
            # No window (the input already violates H8): leave it to the remaining assignment below.

        # 3) the rest: in sorted order. Missing slots (an under-filled solution) stay unassigned (-1).
        rest = [k for k in range(n) if k not in placed]
        for k, slot in zip(rest, pool):
            placed[k] = slot
        for k in range(n):
            assignment[group.lesson_indices[k]] = placed.get(k, -1)

    converted.table.set_assignment(assignment)


def _find_block_window(
    converted: ConvertedProblem, pool: list[int], pinned_slots: set[int], size: int
) -> list[int] | None:
    """Finds a window of consecutive periods on one day (slot indices, of the
    given size) inside pool ∪ pinned_slots that contains every pinned slot."""
    candidates = set(pool) | pinned_slots
    by_day: dict[int, dict[int, int]] = {}
    for slot in candidates:
        day, period = converted.timeslots[slot]
        by_day.setdefault(day, {})[period] = slot
    for day in sorted(by_day):
        periods = by_day[day]
        for start in range(1, max(periods) - size + 2):
            if not all(p in periods for p in range(start, start + size)):
                continue
            window = [periods[p] for p in range(start, start + size)]
            if pinned_slots <= set(window):
                return window
    return None


def convert_solution(
    converted: ConvertedProblem,
    *,
    wall_time_seconds: float = 0.0,
    unassigned_weight: float | None = None,
) -> dict:
    """Turns the engine assignment (the slots of `converted.table`) back into a
    canonical Timetable JSON (dict). Merged groups unfold into one canonical
    lesson per member course, and teacher_ids get each source course's full
    tuple back.

    Special rooms: the same greedy pass as cpsat's rooms.assign_rooms — per
    (type, time) keep the room a course used before, otherwise take free rooms
    in order. H6 being hard, it always succeeds on a hard-0 solution; only a
    solution with hard violations left (UNKNOWN) can report room_id=null plus a
    note.

    penalties are the engine breakdown's raw S1–S6 counts (same meaning as
    validate.py's compute_penalties); objective_value is the weighted penalty
    sum under table.weights — the same unit as the cpsat backend.

    ``unassigned_weight`` is passed only by an assembly that demotes H1 to soft
    (partial timetabling): unassigned periods are then **a result, not a search
    failure**, so they leave the hard total (status is decided by the other hard
    constraints only), the unfilled periods appear in penalties under the same
    name as cpsat (`weekly_period_counts`), and they add `unassigned_weight`
    times to objective_value — the same unit as cpsat's relaxed mode."""
    table = converted.table
    breakdown = canon.compute_breakdown(table)
    partial = unassigned_weight is not None
    # In partial timetabling H1 is not hard: unassigned lessons are the model's
    # choice and must not make the status UNKNOWN (cpsat's relaxed mode reports OPTIMAL/FEASIBLE).
    hard_total = breakdown.hard_total() - (breakdown.unassigned if partial else 0)
    notes: list[str] = list(converted.notes)
    assignment = table.assignment()

    lessons_out: list[dict] = []
    special: dict[tuple[str, int], list[int]] = {}  # (room_type, slot) -> output indices
    for group in converted.groups:
        for lesson_index in group.lesson_indices:
            slot = assignment[lesson_index]
            if slot < 0:
                if not partial:
                    notes.append(f"{group.key}: unassigned lessons left (search failure)")
                continue
            day, period = converted.timeslots[slot]
            for course_id in group.course_ids:
                course = converted.courses_by_id[course_id]
                room_type = converted.subjects_by_id[course["subject_id"]].get("required_room_type")
                if room_type is not None:
                    special.setdefault((room_type, slot), []).append(len(lessons_out))
                lessons_out.append(
                    {
                        "course_id": course_id,
                        "class_group_id": course["class_group_id"],
                        "subject_id": course["subject_id"],
                        "teacher_ids": list(course["teacher_ids"]),
                        "slot": {"day": day, "period": period},
                        "room_id": None,
                    }
                )

    # Special rooms afterwards (same greedy rule as cpsat rooms.assign_rooms).
    preferred: dict[str, str] = {}  # the room a course used last
    for (room_type, slot), indices in sorted(special.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        remaining = sorted(indices, key=lambda i: lessons_out[i]["course_id"])
        free = list(converted.room_ids_by_type[room_type])
        for index in list(remaining):  # pass 1: keep the previous room when it is free
            room_id = preferred.get(lessons_out[index]["course_id"])
            if room_id in free:
                free.remove(room_id)
                remaining.remove(index)
                lessons_out[index]["room_id"] = room_id
        for index in remaining:  # pass 2: free rooms in order; short only on a solution violating H6
            if free:
                room_id = free.pop(0)
                preferred[lessons_out[index]["course_id"]] = room_id
                lessons_out[index]["room_id"] = room_id
            else:
                day, period = converted.timeslots[slot]
                notes.append(
                    f"{lessons_out[index]['course_id']}: no special room ({room_type}) "
                    f"(day {day} period {period}) — the solution still has hard violations"
                )

    # Local search proves neither optimality nor infeasibility: hard 0 is FEASIBLE,
    # anything else UNKNOWN (the assignment is still returned, explained in notes).
    feasible = hard_total == 0
    if not feasible:
        notes.append(f"hard violations left: {hard_total}")
    penalties = {name: int(count) for name, count in breakdown.penalties().items()}
    objective = float(breakdown.soft_penalty(table.weights))
    if unassigned_weight is not None:
        # Same name and unit as cpsat's relaxed mode: the unfilled periods are the penalty term.
        penalties["weekly_period_counts"] = int(breakdown.unassigned)
        objective += unassigned_weight * breakdown.unassigned
        if breakdown.unassigned:
            notes.append(f"{breakdown.unassigned} periods unassigned (H1 relaxed — the rest is a partial solution)")
    return {
        "status": "FEASIBLE" if feasible else "UNKNOWN",
        "lessons": lessons_out,
        "objective_value": objective,
        "best_objective_bound": None,
        "wall_time_seconds": wall_time_seconds,
        "penalties": penalties,
        "notes": notes,
    }
