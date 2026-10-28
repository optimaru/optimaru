"""하드 제약 H1~H10 을 소형 인스턴스로 하나씩 확인한다."""

from collections import Counter

import pytest
from helpers import course, make_problem, quick_solve, slot, teacher

from teacher_scheduling.domain.entities import ClassGroup, Room, Subject
from teacher_scheduling.domain.solution import SolveStatus
from teacher_scheduling.validate import validate_timetable


def _solved_and_valid(problem, timetable):
    assert timetable.status.has_solution, timetable.status
    assert validate_timetable(problem, timetable) == []


# ---------------------------------------------------------------- H1 주당 시수


def test_weekly_period_count():
    problem = make_problem([course(periods_per_week=3, max_periods_per_day=3)], [teacher()], days=1, periods=5)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len(timetable.lessons) == 3


# ---------------------------------------------------------------- H2 고정 시간


def test_fixed_slot_is_scheduled():
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 3)}))],
        [teacher()],
        days=1,
        periods=4,
    )
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert slot(0, 3) in {lesson.slot for lesson in timetable.lessons}


def test_fixed_slot_conflicting_with_unavailable_raises():
    problem = make_problem(
        [course(periods_per_week=1, fixed_slots=frozenset({slot(0, 1)}))],
        [teacher(unavailable=frozenset({slot(0, 1)}))],
        days=1,
        periods=3,
    )
    with pytest.raises(ValueError, match="고정 시간"):
        quick_solve(problem)


# ---------------------------------------------------------------- 금지/불가 시간


def test_forbidden_and_unavailable_slots_are_avoided():
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, forbidden_slots=frozenset({slot(0, 2)}))],
        [teacher(unavailable=frozenset({slot(0, 4)}))],
        class_groups=[ClassGroup(id="C1", name="C1", grade=1, unavailable=frozenset({slot(0, 5)}))],
        days=1,
        periods=5,
    )
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert {lesson.slot.period for lesson in timetable.lessons} == {1, 3}


# ---------------------------------------------------------------- H3/H4 충돌 금지


def test_class_conflicts_forbidden():
    """한 학급의 두 강좌 4시간이 정확히 4교시에 겹침 없이 들어간다."""
    courses = [
        course("K1", subject_id="S1", teacher_ids=("T1",), periods_per_week=2, max_periods_per_day=2),
        course("K2", subject_id="S2", teacher_ids=("T2",), periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len({lesson.slot for lesson in timetable.lessons}) == 4


def test_teacher_conflicts_forbidden():
    """한 교사가 두 학급을 4시간 가르치면 슬롯이 전부 달라야 한다."""
    courses = [
        course("K1", class_group_id="C1", periods_per_week=2, max_periods_per_day=2),
        course("K2", class_group_id="C2", periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher()], days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len({lesson.slot for lesson in timetable.lessons}) == 4


# ---------------------------------------------------------------- H5 분반 동시 진행


def test_parallel_group_runs_simultaneously():
    courses = [
        course("A", teacher_ids=("T1",), periods_per_week=2, max_periods_per_day=2, parallel_group="G"),
        course("B", teacher_ids=("T2",), periods_per_week=2, max_periods_per_day=2, parallel_group="G"),
        course("K3", subject_id="S3", teacher_ids=("T3",), periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2"), teacher("T3")], days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    slots_a = {lesson.slot for lesson in timetable.lessons if lesson.course_id == "A"}
    slots_b = {lesson.slot for lesson in timetable.lessons if lesson.course_id == "B"}
    slots_rest = {lesson.slot for lesson in timetable.lessons if lesson.course_id == "K3"}
    assert slots_a == slots_b
    assert slots_a.isdisjoint(slots_rest)  # 분반 그룹도 학급 한 칸을 차지한다


# ---------------------------------------------------------------- H6 특별실 수용량


def test_room_capacity_respected():
    """과학실 1개를 두 학급이 나눠 쓰면 같은 시간에 겹칠 수 없다."""
    subjects = [Subject(id="SCI", name="과학", required_room_type="과학실")]
    rooms = (Room(id="R1", name="과학실1", room_type="과학실"),)
    courses = [
        course(
            "K1", class_group_id="C1", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T1",)
        ),
        course(
            "K2", class_group_id="C2", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T2",)
        ),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], subjects=subjects, rooms=rooms, days=1, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    assert len({lesson.slot for lesson in timetable.lessons}) == 4
    assert all(lesson.room_id == "R1" for lesson in timetable.lessons)


def test_two_rooms_shared_in_same_slot():
    """방이 2개면 같은 시간에 두 학급이 동시에 쓸 수 있고, 방은 겹치지 않는다."""
    subjects = [Subject(id="SCI", name="과학", required_room_type="과학실")]
    rooms = (
        Room(id="R1", name="과학실1", room_type="과학실"),
        Room(id="R2", name="과학실2", room_type="과학실"),
    )
    courses = [
        course(
            "K1", class_group_id="C1", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T1",)
        ),
        course(
            "K2", class_group_id="C2", subject_id="SCI", periods_per_week=2, max_periods_per_day=2, teacher_ids=("T2",)
        ),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], subjects=subjects, rooms=rooms, days=1, periods=2)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)  # 방 중복/타입 오류는 validator 가 잡는다
    used = Counter((lesson.slot, lesson.room_id) for lesson in timetable.lessons)
    assert all(count == 1 for count in used.values())


# ---------------------------------------------------------------- H7 강좌 일일 상한


def test_daily_course_limit():
    problem = make_problem([course(periods_per_week=4, max_periods_per_day=2)], [teacher()], days=2, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    per_day = Counter(lesson.slot.day for lesson in timetable.lessons)
    assert sorted(per_day.values()) == [2, 2]


# ---------------------------------------------------------------- H8 블록타임


def test_block_periods_are_consecutive():
    problem = make_problem([course(periods_per_week=3, block_periods=3)], [teacher()], days=2, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)  # 연강 연속성은 validator 의 H8 검사가 확인한다
    per_day: dict = {}
    for lesson in timetable.lessons:
        per_day.setdefault(lesson.slot.day, []).append(lesson.slot.period)
    (periods,) = per_day.values()  # 3연강이므로 하루에 몰린다
    periods.sort()
    assert periods == list(range(periods[0], periods[0] + 3))


def test_block_without_room_raises_value_error():
    """연강을 놓을 연속 칸이 아예 없으면 명확한 입력 오류를 낸다."""
    problem = make_problem(
        [course(periods_per_week=2, block_periods=2, forbidden_slots=frozenset({slot(0, 2)}))],
        [teacher()],
        days=1,
        periods=3,
    )
    with pytest.raises(ValueError, match="연강"):
        quick_solve(problem)


# ---------------------------------------------------------------- H9 교사 일일/주당 상한


def test_teacher_weekly_max():
    def build(limit):
        courses = [
            course("K1", class_group_id="C1", periods_per_week=2, max_periods_per_day=2),
            course("K2", class_group_id="C2", periods_per_week=2, max_periods_per_day=2),
        ]
        return make_problem(courses, [teacher(max_periods_per_week=limit)], days=2, periods=4)

    infeasible = quick_solve(build(3))
    assert infeasible.status == SolveStatus.INFEASIBLE
    assert infeasible.lessons == ()

    problem = build(4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)


def test_teacher_daily_max():
    courses = [
        course("K1", class_group_id="C1", periods_per_week=2, max_periods_per_day=2),
        course("K2", class_group_id="C2", periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher(max_periods_per_day=2)], days=2, periods=4)
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)
    per_day = Counter(lesson.slot.day for lesson in timetable.lessons)
    assert sorted(per_day.values()) == [2, 2]


# ---------------------------------------------------------------- H10 연속 수업 제한


def test_teacher_consecutive_limit():
    problem = make_problem(
        [course(periods_per_week=3, max_periods_per_day=3)],
        [teacher(max_consecutive_periods=2)],
        days=1,
        periods=5,
    )
    timetable = quick_solve(problem)
    _solved_and_valid(problem, timetable)  # 연속 상한 위반은 validator 의 H10 검사가 확인한다
    periods = sorted(lesson.slot.period for lesson in timetable.lessons)
    assert periods[2] - periods[0] != 2  # 세 시간이 연달아 붙지 않았다


# ---------------------------------------------------------------- INFEASIBLE


def test_overfull_class_is_infeasible():
    """학급 가용 칸(3)보다 많은 시수(4)는 배정할 수 없다."""
    courses = [
        course("K1", subject_id="S1", teacher_ids=("T1",), periods_per_week=2, max_periods_per_day=2),
        course("K2", subject_id="S2", teacher_ids=("T2",), periods_per_week=2, max_periods_per_day=2),
    ]
    problem = make_problem(courses, [teacher("T1"), teacher("T2")], days=1, periods=3)
    timetable = quick_solve(problem)
    assert timetable.status == SolveStatus.INFEASIBLE
    assert timetable.lessons == ()
