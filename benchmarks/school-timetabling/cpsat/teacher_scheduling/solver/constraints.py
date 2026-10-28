"""하드 제약 구현. 하나라도 깨지면 시간표로 쓸 수 없는 규칙들.

어떤 제약을 켤지는 `registry` 의 제약 클래스가 정하고, 여기서는 그 클래스들이
부르는 `model.add(...)` 만 모아 둔다.


H1  주당 시수 충족          강좌마다 정해진 시수를 정확히 채운다
H2  고정 시간 배정          고정 배정된 칸은 반드시 그 시간에 들어간다
H3  학급 충돌 금지          한 학급은 한 시간에 한 수업만 (분반은 묶어서 한 칸)
H4  교사 충돌 금지          한 교사는 한 시간에 한 수업만
H5  분반 동시 진행          같은 분반 그룹은 항상 같은 시간에
H6  특별실 수용량           같은 시간에 필요한 특별실 수가 보유 수를 넘지 않는다
H7  강좌 일일 최대 시수     같은 과목이 하루에 몰리지 않는다
H8  블록타임(연강)          지정된 시수만큼 연속 교시로 붙인다
H9  교사 일일/주당 최대 시수
H10 교사 연속 수업 제한
"""

from ortools.sat.python import cp_model

from teacher_scheduling.domain.entities import Course
from teacher_scheduling.solver.variables import TimetableVariables


def _add_weekly_period_counts(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H1: 강좌별 주당 시수를 정확히 맞춘다."""
    for course in variables.problem.courses:
        model.add(sum(variables.course_vars(course.id)) == course.periods_per_week)


def _add_fixed_slots(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H2: 고정 배정."""
    for course in variables.problem.courses:
        for slot in course.fixed_slots:
            var = variables.x.get((course.id, slot))
            if var is None:
                raise ValueError(f"[{course.id}] 고정 시간 {slot} 이 교사/학급 불가 시간과 충돌합니다")
            model.add(var == 1)


def _add_parallel_groups(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H5: 분반 그룹은 같은 시간에 동시 진행."""
    for courses in variables.problem.courses_by_parallel_group.values():
        representative, *others = courses
        for course in others:
            for slot in variables.allowed_slots[course.id]:
                model.add(variables.x[course.id, slot] == variables.x[representative.id, slot])


def _add_class_group_conflicts(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H3: 학급 충돌 금지.

    불리언인 class_busy 와 같게 두는 것만으로 "그 시간 그 학급 수업은 최대 1개"가 강제된다.
    """
    for key, lesson_vars in variables.class_lessons.items():
        model.add(sum(lesson_vars) == variables.class_busy[key])


def _add_teacher_conflicts(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H4: 교사 충돌 금지. H3 와 같은 방식."""
    for key, lesson_vars in variables.teacher_lessons.items():
        model.add(sum(lesson_vars) == variables.teacher_busy[key])


def _add_room_capacity(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H6: 특별실은 같은 시간에 보유 대수만큼만 쓸 수 있다.

    어느 강좌가 몇 번 교실을 쓰는지는 풀고 나서 배정한다(`rooms.assign_rooms`).
    같은 종류의 특별실은 서로 대체 가능하므로 개수 제약만으로 충분하다.
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
    """H7: 같은 강좌를 하루에 몇 시간까지 넣을 수 있는지."""
    for course in variables.problem.courses:
        limit = course.effective_max_periods_per_day
        for day in variables.problem.calendar.days:
            day_vars = variables.course_vars_on_day(course.id, day)
            if len(day_vars) > limit:
                model.add(sum(day_vars) <= limit)


def _add_block_periods(model: cp_model.CpModel, variables: TimetableVariables) -> None:
    """H8: 블록타임. 주당 한 번, block_periods 만큼 연속 교시로 배치한다."""
    for course in variables.problem.courses:
        if not course.has_block:
            continue
        starts = _block_start_vars(model, variables, course)
        if not starts:
            # 연강을 놓을 연속 칸이 아예 없다면 탐색할 것도 없는 입력 오류다.
            raise ValueError(
                f"[{course.id}] {course.block_periods}연강을 놓을 연속 교시가 없습니다"
                " (금지/불가 시간과 캘린더를 확인하세요)"
            )
        model.add_exactly_one(starts)


def _block_start_vars(model: cp_model.CpModel, variables: TimetableVariables, course: Course) -> list[cp_model.IntVar]:
    """블록의 시작 교시 후보마다 불리언을 만들고, 켜지면 연속 칸이 전부 켜지게 묶는다."""
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
    """H9: 교사 일일/주당 최대 시수."""
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
    """H10: 연강 제한. 길이 (limit + 1) 인 모든 창에서 최대 limit 시간만 수업."""
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
