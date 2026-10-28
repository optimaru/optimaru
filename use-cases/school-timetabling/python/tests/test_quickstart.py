"""quickstart 모드 근사 변환 검증 (기존 convert.py 커버리지를 이식).

canonical 풀-패리티 경로는 test_convert/test_parity 가 다루고, 여기서는
quickstart 도메인(6제약 + slotRoom 합성 인코딩)의 전개/특별실 repair/pinned
동작이 그대로 유지되는지 본다.
"""

import pytest

import st_timetabling as st
from st_timetabling import quickstart


def test_course_expansion(problem):
    conv = quickstart.convert_problem(problem)
    table = conv.table

    # 강좌 하나 = periods_per_week 개의 Lesson, id 는 "{course_id}#k".
    assert len(table.lessons) == 40  # 학급 2개 x 주 20시수
    ids = [lesson.id for lesson in table.lessons]
    assert "C1-KOR#1" in ids and "C1-KOR#4" in ids
    assert "C2-MUS#2" in ids
    per_course: dict[str, int] = {}
    for course_id in conv.lesson_courses:
        per_course[course_id] = per_course.get(course_id, 0) + 1
    for course_id, course in conv.courses_by_id.items():
        assert per_course[course_id] == course["periods_per_week"]

    # quickstart 한계: 팀티칭은 teacher_ids[0] 만 엔진 교사가 된다.
    mus = next(l for l in table.lessons if l.id == "C1-MUS#1")
    assert mus.teacher == "T_MUS"
    assert mus.student_group == "C1"
    assert any("팀티칭" in note for note in conv.notes)


def test_room_mapping(problem):
    conv = quickstart.convert_problem(problem)

    # 학급 전용 가짜 교실 2개 + 실제로 쓰는 특별실 2개; 음악실은 버려진다.
    assert conv.room_ids[:2] == [None, None]
    assert set(conv.room_ids[2:]) == {"R_SCI", "R_GYM"}
    assert any("R_MUSIC" in note for note in conv.notes)

    # 교사 배정 불가 시간: (day 0, period 1) 은 timeslot index 0.
    assert conv.table.teacher_unavailable["T_KOR"] == {0}


def test_missing_special_room_raises(problem):
    problem["rooms"] = [r for r in problem["rooms"] if r["room_type"] != "과학실"]
    with pytest.raises(ValueError, match="과학실"):
        quickstart.convert_problem(problem)


def test_fixed_slot_becomes_pinned_and_survives_solve(problem):
    conv = quickstart.convert_problem(problem)
    ts = conv.slot_index[(2, 3)]
    gym = conv.room_ids.index("R_GYM")
    expected = ts * len(conv.room_ids) + gym

    lessons = conv.table.lessons
    pe1 = next(l for l in lessons if l.id == "C1-PE#1")
    assert pe1.pinned and pe1.slot_room == expected
    pe2 = next(l for l in lessons if l.id == "C1-PE#2")
    assert not pe2.pinned and pe2.slot_room == -1

    pinned_index = next(i for i, l in enumerate(lessons) if l.id == "C1-PE#1")
    cfg = st.SolverConfig()
    cfg.spent_limit_seconds = 10.0
    cfg.random_seed = 7
    cfg.best_score_limit = (0, -1e18)  # 첫 feasible 에서 종료
    st.Solver(cfg).solve(conv.table)
    assert conv.table.assignment()[pinned_index] == expected

    # canonical 출력에도 그 칸이 그대로 나타난다 (체육관 매칭 -> room_id 채움).
    result = quickstart.convert_solution(conv)
    pe = [l for l in result["lessons"] if l["course_id"] == "C1-PE"]
    fixed = next(l for l in pe if l["slot"] == {"day": 2, "period": 3})
    assert fixed["room_id"] == "R_GYM"


def test_round_trip_with_room_repair(problem):
    """엔진 배정을 손으로 만들어 canonical 재집계와 특별실 repair 를 본다."""
    conv = quickstart.convert_problem(problem)
    table = conv.table
    room_count = table.room_count()
    homeroom = {"C1": 0, "C2": 1}
    sci_room = conv.room_ids.index("R_SCI")

    # C1 은 자기 수업 20개를 slot 0..19 에 순서대로, C2 는 4칸 밀어서 배정한다.
    # 과학 수업은 과학실로 — 단 C2-SCI#1 은 일부러 C1-SCI#1 과 같은 시간의
    # 가짜 교실에 두어, repair 가 빈 과학실을 못 찾는 실패 경로
    # (room_id=null + notes)를 확인한다. C2-SCI#2/#3 은 가짜 교실에 두지만
    # 그 시간의 과학실이 비어 있으므로 repair 로 R_SCI 를 받아야 한다.
    assignment = []
    next_slot = {"C1": 0, "C2": 0}
    for index, lesson in enumerate(table.lessons):
        course = conv.courses_by_id[conv.lesson_courses[index]]
        group = course["class_group_id"]
        slot = next_slot[group]
        next_slot[group] += 1
        if group == "C2":
            slot = (slot + 4) % 20
        room = homeroom[group]
        if course["subject_id"] == "SCI":
            if lesson.id == "C2-SCI#1":
                slot = 12  # C1-SCI#1 이 과학실을 쓰는 시간
            elif group == "C1":
                room = sci_room
        assignment.append(slot * room_count + room)
    table.set_assignment(assignment)

    result = quickstart.convert_solution(conv, wall_time_seconds=1.5)
    assert result["wall_time_seconds"] == 1.5
    assert len(result["lessons"]) == 40

    # 강좌별 재집계가 periods_per_week 와 일치하고, 필드가 원본을 복원한다.
    per_course: dict[str, list[dict]] = {}
    for lesson in result["lessons"]:
        per_course.setdefault(lesson["course_id"], []).append(lesson)
    for course_id, course in conv.courses_by_id.items():
        emitted = per_course[course_id]
        assert len(emitted) == course["periods_per_week"]
        for lesson in emitted:
            assert lesson["class_group_id"] == course["class_group_id"]
            assert lesson["subject_id"] == course["subject_id"]
            assert lesson["teacher_ids"] == list(course["teacher_ids"])

    # 팀티칭 강좌는 전체 teacher_ids 가 복원된다.
    assert per_course["C1-MUS"][0]["teacher_ids"] == ["T_MUS", "T_ASSIST"]

    # slot 디코딩: C1 수업들은 slot 0..19 = (day 0..4, period 1..4) 순서.
    c1_slots = [(l["slot"]["day"], l["slot"]["period"]) for l in result["lessons"] if l["class_group_id"] == "C1"]
    assert c1_slots == [(d, p) for d in range(5) for p in range(1, 5)]

    # room_id 규약: 일반 수업은 null, 과학 수업은 과학실 (유지 또는 repair).
    for lesson in per_course["C1-KOR"]:
        assert lesson["room_id"] is None
    assert all(l["room_id"] == "R_SCI" for l in per_course["C1-SCI"])
    c2_rooms = sorted((l["room_id"] is None, l["room_id"] or "") for l in per_course["C2-SCI"])
    # C2-SCI#2/#3 은 빈 과학실로 repair, #1 은 과학실이 점유돼 배정 실패.
    assert c2_rooms == [(False, "R_SCI"), (False, "R_SCI"), (True, "")]
    assert any("C2-SCI" in note and "배정 실패" in note for note in result["notes"])
