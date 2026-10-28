"""convert.py: round-trip consistency of canonical -> st::canon unfolding and engine -> canonical regrouping."""

import pytest

from st_timetabling import canon, convert_problem, convert_solution


def test_course_expansion(problem):
    conv = convert_problem(problem)
    table = conv.table

    # One course = periods_per_week Lessons, ids "{course_id}#k".
    assert len(table.lessons) == 40  # 2 classes x 20 weekly periods
    ids = [lesson.id for lesson in table.lessons]
    assert "C1-KOR#1" in ids and "C1-KOR#4" in ids
    assert "C2-MUS#2" in ids
    per_group: dict[str, int] = {}
    for group in conv.groups:
        per_group[group.key] = len(group.lesson_indices)
    for course_id, course in conv.courses_by_id.items():
        assert per_group[course_id] == course["periods_per_week"]

    # Lessons of one course share course_key; class occupancy is 1 (dense id).
    kor = [l for l in table.lessons if l.id.startswith("C1-KOR#")]
    assert len({l.course_key for l in kor}) == 1
    assert all(l.class_group_id == 0 for l in kor)  # C1 is the first class

    # Team teaching: teachers holds the co-teachers too (no [0] restriction any more).
    mus = next(l for l in table.lessons if l.id == "C1-MUS#1")
    assert len(mus.teachers) == 2
    assert conv.notes == []  # the canonical conversion has no 'unsupported' notes


def test_special_room_demand_and_capacity(problem):
    conv = convert_problem(problem)
    table = conv.table

    # Only the needed types get a dense index: science lab / gym (no demand for the music room).
    assert {"science lab", "gym"} <= set(conv.room_ids_by_type)
    assert table.room_capacity_by_type == [1, 1]

    sci = next(l for l in table.lessons if l.id == "C1-SCI#1")
    kor = next(l for l in table.lessons if l.id == "C1-KOR#1")
    assert sum(sci.room_type_demand) == 1
    assert sum(kor.room_type_demand) == 0


def test_unavailable_and_forbidden_become_allowed_slot(problem):
    # Kim (T_KOR) unavailable Monday period 1 + C1-KOR forbidden time + C1 class unavailability.
    problem["courses"][0]["forbidden_slots"] = [{"day": 1, "period": 2}]
    problem["class_groups"][0]["unavailable"] = [{"day": 4, "period": 4}]
    conv = convert_problem(problem)

    kor = next(l for l in conv.table.lessons if l.id == "C1-KOR#1")
    blocked = {conv.slot_index[(0, 1)], conv.slot_index[(1, 2)], conv.slot_index[(4, 4)]}
    assert [i for i, ok in enumerate(kor.allowed_slot) if not ok] == sorted(blocked)

    # Other courses of the same class drop only the class unavailability.
    math = next(l for l in conv.table.lessons if l.id == "C1-MATH#1")
    assert [i for i, ok in enumerate(math.allowed_slot) if not ok] == [conv.slot_index[(4, 4)]]


def test_fixed_slot_pins_and_conflict_raises(problem):
    conv = convert_problem(problem)
    pe1 = next(l for l in conv.table.lessons if l.id == "C1-PE#1")
    assert pe1.pinned and pe1.slot == conv.slot_index[(2, 3)]
    pe2 = next(l for l in conv.table.lessons if l.id == "C1-PE#2")
    assert not pe2.pinned and pe2.slot == -1

    # A fixed time contradicting a forbidden/unavailable one fails at the same layer as cpsat (ValueError).
    problem["courses"][0]["fixed_slots"] = [{"day": 0, "period": 1}]  # T_KOR unavailable time
    with pytest.raises(ValueError, match="clashes"):
        convert_problem(problem)


def test_block_periods_assign_block_group(problem):
    problem["courses"][1]["block_periods"] = 2  # C1-MATH, 4 periods a week
    conv = convert_problem(problem)
    math = [l for l in conv.table.lessons if l.id.startswith("C1-MATH#")]
    # Only the last block_periods lessons get a blockGroup.
    assert [l.block_group for l in math] == [-1, -1, 0, 0]
    # The H7 cap rises to at least the block size (a block must fit in one day).
    group_index = next(i for i, g in enumerate(conv.groups) if g.key == "C1-MATH")
    assert conv.table.course_daily_limit[group_index] == 2


def test_parallel_group_merges_into_one_lesson(problem):
    # C1 technology/home-economics split: technology (special room) and home
    # economics (preferred periods) run at the same time.
    problem["subjects"] += [
        {"id": "TECH", "name": "Technology", "required_room_type": "tech room", "avoided_periods": [4]},
        {"id": "HOME", "name": "Home economics", "preferred_periods": [1, 2]},
    ]
    problem["teachers"] += [
        {"id": "T_TECH", "name": "Kwon (Technology)", "subject_ids": ["TECH"]},
        {"id": "T_HOME", "name": "Song (Home economics)", "subject_ids": ["HOME"]},
    ]
    problem["rooms"].append({"id": "R_TECH", "name": "Tech room", "room_type": "tech room"})
    for cg, subject_id, teacher_id in (("C1", "TECH", "T_TECH"), ("C1", "HOME", "T_HOME")):
        problem["courses"].append(
            {
                "id": f"{cg}-{subject_id}",
                "class_group_id": cg,
                "subject_id": subject_id,
                "teacher_ids": [teacher_id],
                "periods_per_week": 2,
                "parallel_group": "C1-split",
            }
        )
    conv = convert_problem(problem)

    # A parallel group folds into one merged lesson per weekly period: teacher
    # union, summed special-room demand, class occupancy 1 (the lesson itself is one).
    merged = [l for l in conv.table.lessons if l.id.startswith("C1-split#")]
    assert len(merged) == 2
    assert len(merged[0].teachers) == 2
    assert sum(merged[0].room_type_demand) == 1  # one tech room (home economics uses the class room)
    # S5/S6: one preference unit per member course (technology avoids / home economics prefers).
    assert merged[0].preference_units == [(1 << 3, 0), (0, (1 << 0) | (1 << 1))]

    group = next(g for g in conv.groups if g.key == "C1-split")
    assert group.course_ids == ["C1-TECH", "C1-HOME"]

    # Reverse conversion: one merged lesson unfolds into two canonical lessons, one per member course.
    assignment = conv.table.assignment()
    for k, index in enumerate(group.lesson_indices):
        assignment[index] = conv.slot_index[(3, k + 1)]
    # Fill the remaining lessons anywhere to remove unassigned noise (only the shape matters here).
    for i, slot in enumerate(assignment):
        if slot < 0:
            assignment[i] = i % conv.table.slot_count()
    conv.table.set_assignment(assignment)
    result = convert_solution(conv)
    emitted = [l for l in result["lessons"] if l["course_id"] in ("C1-TECH", "C1-HOME")]
    assert len(emitted) == 4  # 2 courses x 2 weekly periods
    slots_of = {
        course_id: sorted((l["slot"]["day"], l["slot"]["period"]) for l in emitted if l["course_id"] == course_id)
        for course_id in ("C1-TECH", "C1-HOME")
    }
    assert slots_of["C1-TECH"] == slots_of["C1-HOME"] == [(3, 1), (3, 2)]  # H5: run together
    for lesson in emitted:
        assert lesson["room_id"] == ("R_TECH" if lesson["course_id"] == "C1-TECH" else None)


def test_parallel_group_across_classes_raises(problem):
    problem["courses"][0]["parallel_group"] = "bad-split"
    c2 = next(c for c in problem["courses"] if c["class_group_id"] == "C2")
    c2["parallel_group"] = "bad-split"
    with pytest.raises(ValueError, match="spans different classes"):
        convert_problem(problem)


def test_missing_special_room_raises(problem):
    problem["rooms"] = [r for r in problem["rooms"] if r["room_type"] != "science lab"]
    with pytest.raises(ValueError, match="science lab"):
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
    """Builds an engine assignment by hand to check canonical regrouping / the special-room greedy / penalties."""
    conv = convert_problem(problem)
    table = conv.table

    # C1 takes slots 0..19 in order, C2 shifted by 4: no class or teacher clash.
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
                slot += 1  # leave the pinned C1-PE#1 cell free
            next_slot[group.class_group_id] = slot + 1
            offset = 4 if group.class_group_id == "C2" else 0
            assignment[index] = (slot + offset) % 20
    table.set_assignment(assignment)

    result = convert_solution(conv, wall_time_seconds=1.5)
    assert result["wall_time_seconds"] == 1.5
    assert len(result["lessons"]) == 40

    # Per-course regrouping matches periods_per_week and the fields restore the source.
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

    # Team-taught courses get their full teacher_ids back.
    assert per_course["C1-MUS"][0]["teacher_ids"] == ["T_MUS", "T_ASSIST"]

    # room_id convention: null for regular lessons, special-room lessons get a room from the greedy pass.
    for lesson in per_course["C1-KOR"]:
        assert lesson["room_id"] is None
    assert all(l["room_id"] == "R_SCI" for l in per_course["C1-SCI"] + per_course["C2-SCI"])
    assert all(l["room_id"] == "R_GYM" for l in per_course["C1-PE"] + per_course["C2-PE"])

    # penalties are the raw S1–S6 counts, objective_value the weighted sum (cpsat units).
    breakdown = canon.compute_breakdown(table)
    assert result["penalties"] == {name: int(count) for name, count in breakdown.penalties().items()}
    assert result["objective_value"] == breakdown.soft_penalty(table.weights)
