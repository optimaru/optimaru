"""convert.py 검증: canonical -> st::canon 전개와 엔진 -> canonical 재집계의 왕복 정합성."""

import pytest

from st_timetabling import canon, convert_problem, convert_solution


def test_course_expansion(problem):
    conv = convert_problem(problem)
    table = conv.table

    # 강좌 하나 = periods_per_week 개의 Lesson, id 는 "{course_id}#k".
    assert len(table.lessons) == 40  # 학급 2개 x 주 20시수
    ids = [lesson.id for lesson in table.lessons]
    assert "C1-KOR#1" in ids and "C1-KOR#4" in ids
    assert "C2-MUS#2" in ids
    per_group: dict[str, int] = {}
    for group in conv.groups:
        per_group[group.key] = len(group.lesson_indices)
    for course_id, course in conv.courses_by_id.items():
        assert per_group[course_id] == course["periods_per_week"]

    # 같은 강좌의 레슨들은 course_key 를 공유하고 학급 점유는 1(dense id)이다.
    kor = [l for l in table.lessons if l.id.startswith("C1-KOR#")]
    assert len({l.course_key for l in kor}) == 1
    assert all(l.class_group_id == 0 for l in kor)  # C1 이 첫 학급

    # 팀티칭: teachers 에 부교사까지 전부 들어간다 (이제 [0] 제한이 없다).
    mus = next(l for l in table.lessons if l.id == "C1-MUS#1")
    assert len(mus.teachers) == 2
    assert conv.notes == []  # canonical 변환은 미지원 notes 가 없어야 정상


def test_special_room_demand_and_capacity(problem):
    conv = convert_problem(problem)
    table = conv.table

    # 필요한 타입만 dense index 를 받는다: 과학실/체육관 (음악실은 수요 없음).
    assert {"과학실", "체육관"} <= set(conv.room_ids_by_type)
    assert table.room_capacity_by_type == [1, 1]

    sci = next(l for l in table.lessons if l.id == "C1-SCI#1")
    kor = next(l for l in table.lessons if l.id == "C1-KOR#1")
    assert sum(sci.room_type_demand) == 1
    assert sum(kor.room_type_demand) == 0


def test_unavailable_and_forbidden_become_allowed_slot(problem):
    # 김국어(T_KOR) 월요일 1교시 불가 + C1-KOR 금지 시간 + C1 학급 불가 시간.
    problem["courses"][0]["forbidden_slots"] = [{"day": 1, "period": 2}]
    problem["class_groups"][0]["unavailable"] = [{"day": 4, "period": 4}]
    conv = convert_problem(problem)

    kor = next(l for l in conv.table.lessons if l.id == "C1-KOR#1")
    blocked = {conv.slot_index[(0, 1)], conv.slot_index[(1, 2)], conv.slot_index[(4, 4)]}
    assert [i for i, ok in enumerate(kor.allowed_slot) if not ok] == sorted(blocked)

    # 같은 학급의 다른 강좌는 학급 불가 시간만 걷어낸다.
    math = next(l for l in conv.table.lessons if l.id == "C1-MATH#1")
    assert [i for i, ok in enumerate(math.allowed_slot) if not ok] == [conv.slot_index[(4, 4)]]


def test_fixed_slot_pins_and_conflict_raises(problem):
    conv = convert_problem(problem)
    pe1 = next(l for l in conv.table.lessons if l.id == "C1-PE#1")
    assert pe1.pinned and pe1.slot == conv.slot_index[(2, 3)]
    pe2 = next(l for l in conv.table.lessons if l.id == "C1-PE#2")
    assert not pe2.pinned and pe2.slot == -1

    # 고정 시간이 금지/불가와 모순이면 cpsat 과 같은 계층(ValueError)에서 실패.
    problem["courses"][0]["fixed_slots"] = [{"day": 0, "period": 1}]  # T_KOR 불가 시간
    with pytest.raises(ValueError, match="충돌"):
        convert_problem(problem)


def test_block_periods_assign_block_group(problem):
    problem["courses"][1]["block_periods"] = 2  # C1-MATH, 주 4시간
    conv = convert_problem(problem)
    math = [l for l in conv.table.lessons if l.id.startswith("C1-MATH#")]
    # 마지막 block_periods 개 레슨만 blockGroup 을 받는다.
    assert [l.block_group for l in math] == [-1, -1, 0, 0]
    # H7 상한은 블록 크기 이상으로 올라간다 (블록이 하루에 몰릴 수 있어야 한다).
    group_index = next(i for i, g in enumerate(conv.groups) if g.key == "C1-MATH")
    assert conv.table.course_daily_limit[group_index] == 2


def test_parallel_group_merges_into_one_lesson(problem):
    # C1 기술가정 분반: 기술(특별실)과 가정(선호 교시)이 같은 시간에 진행된다.
    problem["subjects"] += [
        {"id": "TECH", "name": "기술", "required_room_type": "기술실", "avoided_periods": [4]},
        {"id": "HOME", "name": "가정", "preferred_periods": [1, 2]},
    ]
    problem["teachers"] += [
        {"id": "T_TECH", "name": "권기술", "subject_ids": ["TECH"]},
        {"id": "T_HOME", "name": "송가정", "subject_ids": ["HOME"]},
    ]
    problem["rooms"].append({"id": "R_TECH", "name": "기술실", "room_type": "기술실"})
    for cg, subject_id, teacher_id in (("C1", "TECH", "T_TECH"), ("C1", "HOME", "T_HOME")):
        problem["courses"].append(
            {
                "id": f"{cg}-{subject_id}",
                "class_group_id": cg,
                "subject_id": subject_id,
                "teacher_ids": [teacher_id],
                "periods_per_week": 2,
                "parallel_group": "C1-분반",
            }
        )
    conv = convert_problem(problem)

    # 분반 그룹은 주당 시수만큼의 병합 레슨 하나로 접힌다: 교사 합집합,
    # 특별실 수요 합, 학급 점유 1 (레슨 자체가 1개).
    merged = [l for l in conv.table.lessons if l.id.startswith("C1-분반#")]
    assert len(merged) == 2
    assert len(merged[0].teachers) == 2
    assert sum(merged[0].room_type_demand) == 1  # 기술실 1 (가정은 일반 교실)
    # S5/S6 은 멤버 강좌마다 preference unit 1개씩 (기술 기피 / 가정 선호).
    assert merged[0].preference_units == [(1 << 3, 0), (0, (1 << 0) | (1 << 1))]

    group = next(g for g in conv.groups if g.key == "C1-분반")
    assert group.course_ids == ["C1-TECH", "C1-HOME"]

    # 역변환: 병합 레슨 하나가 멤버 강좌별 canonical lesson 2개로 다시 풀린다.
    assignment = conv.table.assignment()
    for k, index in enumerate(group.lesson_indices):
        assignment[index] = conv.slot_index[(3, k + 1)]
    # 나머지 레슨도 아무 데나 채워 미배정 노이즈를 없앤다 (여기선 형태만 본다).
    for i, slot in enumerate(assignment):
        if slot < 0:
            assignment[i] = i % conv.table.slot_count()
    conv.table.set_assignment(assignment)
    result = convert_solution(conv)
    emitted = [l for l in result["lessons"] if l["course_id"] in ("C1-TECH", "C1-HOME")]
    assert len(emitted) == 4  # 강좌 2개 x 주 2시간
    slots_of = {
        course_id: sorted((l["slot"]["day"], l["slot"]["period"]) for l in emitted if l["course_id"] == course_id)
        for course_id in ("C1-TECH", "C1-HOME")
    }
    assert slots_of["C1-TECH"] == slots_of["C1-HOME"] == [(3, 1), (3, 2)]  # H5: 동시 진행
    for lesson in emitted:
        assert lesson["room_id"] == ("R_TECH" if lesson["course_id"] == "C1-TECH" else None)


def test_parallel_group_across_classes_raises(problem):
    problem["courses"][0]["parallel_group"] = "잘못된분반"
    c2 = next(c for c in problem["courses"] if c["class_group_id"] == "C2")
    c2["parallel_group"] = "잘못된분반"
    with pytest.raises(ValueError, match="학급이 서로 다릅니다"):
        convert_problem(problem)


def test_missing_special_room_raises(problem):
    problem["rooms"] = [r for r in problem["rooms"] if r["room_type"] != "과학실"]
    with pytest.raises(ValueError, match="과학실"):
        convert_problem(problem)


def test_weights_and_caps_forwarded(problem):
    problem["teachers"][0]["max_periods_per_week"] = 18
    problem["teachers"][0]["max_periods_per_day"] = 5
    problem["teachers"][0]["max_consecutive_periods"] = 3
    problem["teachers"][0]["preferred_free_days"] = [4]
    conv = convert_problem(problem)
    table = conv.table
    assert table.teacher_max_per_week[0] == 18 and table.teacher_max_per_week[1] == -1
    assert table.teacher_max_per_day[0] == 5 and table.teacher_max_per_day[1] == 6
    assert table.teacher_max_consecutive[0] == 3 and table.teacher_max_consecutive[1] == 4
    assert list(table.teacher_preferred_free_days) == [1 << 4] + [0] * 6


def test_round_trip_rooms_and_penalties(problem):
    """엔진 배정을 손으로 만들어 canonical 재집계/특별실 그리디/penalties 를 본다."""
    conv = convert_problem(problem)
    table = conv.table

    # C1 은 slot 0..19 순서대로, C2 는 4칸 밀어서: 학급/교사 충돌이 없는 배정.
    assignment = [-1] * len(table.lessons)
    next_slot = {"C1": 0, "C2": 0}
    for group in conv.groups:
        for index in group.lesson_indices:
            lesson = table.lessons[index]
            if lesson.pinned:
                assignment[index] = lesson.slot
                continue
            slot = next_slot[group.class_group_id]
            while (group.class_group_id, slot) == ("C1", conv.slot_index[(2, 3)]):
                slot += 1  # C1-PE#1 고정 칸은 비워 둔다
            next_slot[group.class_group_id] = slot + 1
            offset = 4 if group.class_group_id == "C2" else 0
            assignment[index] = (slot + offset) % 20
    table.set_assignment(assignment)

    result = convert_solution(conv, wall_time_seconds=1.5)
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

    # room_id 규약: 일반 수업은 null, 특별실 수업은 사후 그리디로 방을 받는다.
    for lesson in per_course["C1-KOR"]:
        assert lesson["room_id"] is None
    assert all(l["room_id"] == "R_SCI" for l in per_course["C1-SCI"] + per_course["C2-SCI"])
    assert all(l["room_id"] == "R_GYM" for l in per_course["C1-PE"] + per_course["C2-PE"])

    # penalties 는 S1~S6 원시 카운트, objective_value 는 가중 합 (cpsat 단위).
    breakdown = canon.compute_breakdown(table)
    assert result["penalties"] == {name: int(count) for name, count in breakdown.penalties().items()}
    assert result["objective_value"] == breakdown.soft_penalty(table.weights)
