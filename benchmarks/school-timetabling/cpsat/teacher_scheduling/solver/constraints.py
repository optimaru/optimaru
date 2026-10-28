"""Hard constraints: rules that make a timetable unusable if even one is broken.

Which constraints are switched on is decided by the constraint classes in
`registry`; this module only collects the `model.add(...)` calls they make.


H1  weekly periods met        each course gets exactly its periods
H2  fixed-time placement      fixed cells always land at that time
H3  no class clashes          a class has one lesson at a time (a parallel group counts as one)
H4  no teacher clashes        a teacher has one lesson at a time
H5  parallel sections         a parallel group always runs at the same time
H6  special-room capacity     special rooms needed at a time do not exceed the rooms owned
H7  daily maximum per course  a subject does not pile up on one day
H8  block time                the given periods are joined as consecutive periods
H9  teacher daily/weekly maximum periods
H10 teacher consecutive-lessons limit
"""

from ortools.sat.python import cp_model

from teacher_scheduling.domain.entities import Course
from teacher_scheduling.solver.variables import TimetableVariables


def _add_weekly_period_counts(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H1: exactly the weekly periods per course."""
    for course in variables.problem.courses:
        model.add(sum(variables.course_vars(course.id)) == course.periods_per_week)


def _add_fixed_slots(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H2: fixed placements."""
    for course in variables.problem.courses:
        for slot in course.fixed_slots:
            var = variables.x.get((course.id, slot))
            if var is None:
                raise ValueError(f"[{course.id}] fixed time {slot} clashes with a teacher/class unavailable time")
            model.add(var == 1)


def _add_parallel_groups(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H5: a parallel group runs at the same time."""
    for courses in variables.problem.courses_by_parallel_group.values():
        representative, *others = courses
        for course in others:
            for slot in variables.allowed_slots[course.id]:
                model.add(variables.x[course.id, slot] == variables.x[representative.id, slot])


def _add_class_group_conflicts(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H3: no class clashes.

    Equating it with the boolean class_busy alone enforces "at most one lesson of that class at that time".
    """
    for key, lesson_vars in variables.class_lessons.items():
        model.add(sum(lesson_vars) == variables.class_busy[key])


def _add_teacher_conflicts(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H4: no teacher clashes. Same approach as H3."""
    for key, lesson_vars in variables.teacher_lessons.items():
        model.add(sum(lesson_vars) == variables.teacher_busy[key])


def _add_room_capacity(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H6: a special-room type can be used only as many times at once as rooms exist.

    Which course uses which room is assigned after solving (`rooms.assign_rooms`).
    Rooms of the same type are interchangeable, so a count constraint suffices.
    """
    problem = variables.problem
    demands: dict[tuple[str, object], list[cp_model.IntVar]] = {}
    for course in problem.courses:
        room_type = problem.required_room_type(course)
        if room_type is None:
            continue
        for slot in variables.allowed_slots[course.id]:
            demands.setdefault((room_type, slot), []).append(variables.x[course.id, slot])

    for (room_type, _slot), lesson_vars in demands.items():
        capacity = len(problem.rooms_by_type.get(room_type, ()))
        model.add(sum(lesson_vars) <= capacity)


def _add_daily_course_limits(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H7: how many periods of the same course fit on one day."""
    for course in variables.problem.courses:
        limit = course.effective_max_periods_per_day
        for day in variables.problem.calendar.days:
            day_vars = variables.course_vars_on_day(course.id, day)
            if len(day_vars) > limit:
                model.add(sum(day_vars) <= limit)


def _add_block_periods(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H8: block time. Once a week, block_periods consecutive periods."""
    for course in variables.problem.courses:
        if not course.has_block:
            continue
        starts = _block_start_vars(model, variables, course)
        if not starts:
            # Without any consecutive cells for the block there is nothing to search: an input error.
            raise ValueError(
                f"[{course.id}] no consecutive periods for a block of {course.block_periods}"
                " (check the forbidden/unavailable times and the calendar)"
            )
        model.add_exactly_one(starts)


def _block_start_vars(model: cp_model.CpModel, variables: TimetableVariables, course: Course) -> list[cp_model.IntVar]:
    """A boolean per candidate start period of the block; when on, it switches on every consecutive cell."""
    calendar = variables.problem.calendar
    allowed = set(variables.allowed_slots[course.id])
    size = course.block_periods
    starts = []
    for day in calendar.days:
        day_slots = calendar.slots_of(day)
        for index in range(len(day_slots) - size + 1):
            window = day_slots[index : index + size]
            if not all(slot in allowed for slot in window):
                continue
            start = model.new_bool_var(f"block[{course.id},{window[0]}]")
            for slot in window:
                model.add_implication(start, variables.x[course.id, slot])
            starts.append(start)
    return starts


def _add_teacher_workload(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H9: teacher daily/weekly maximum periods."""
    problem = variables.problem
    for teacher in problem.teachers:
        weekly_vars = [var for (teacher_id, _slot), var in variables.teacher_busy.items() if teacher_id == teacher.id]
        if not weekly_vars:
            continue
        if teacher.max_periods_per_week is not None:
            model.add(sum(weekly_vars) <= teacher.max_periods_per_week)
        for day in problem.calendar.days:
            day_vars = list(variables.teacher_busy_on_day(teacher.id, day).values())
            if len(day_vars) > teacher.max_periods_per_day:
                model.add(sum(day_vars) <= teacher.max_periods_per_day)


def _add_teacher_consecutive_limits(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H10: consecutive-lessons limit. At most limit lessons in every window of length (limit + 1)."""
    problem = variables.problem
    for teacher in problem.teachers:
        window_size = teacher.max_consecutive_periods + 1
        for day in problem.calendar.days:
            busy_by_period = variables.teacher_busy_on_day(teacher.id, day)
            periods = sorted(busy_by_period)
            if len(periods) < window_size:
                continue
            for start in range(1, problem.calendar.periods_per_day[day] - window_size + 2):
                window = [busy_by_period[p] for p in range(start, start + window_size) if p in busy_by_period]
                if len(window) == window_size:
                    model.add(sum(window) <= teacher.max_consecutive_periods)
