"""Approximate conversion of quickstart mode (the coverage carried over from the old convert.py).

test_convert/test_parity cover the canonical full-parity path; here we check
that the quickstart domain (6 constraints + slotRoom composite encoding) keeps
its unfolding, special-room repair and pinned behaviour.
"""

import pytest

import st_timetabling as st
from st_timetabling import quickstart


def test_course_expansion(problem):
    conv = quickstart.convert_problem(problem)
    table = conv.table

    # One course = periods_per_week Lessons, ids "{course_id}#k".
    assert len(table.lessons) == 40  # 2 classes x 20 weekly periods
    ids = [lesson.id for lesson in table.lessons]
    assert "C1-KOR#1" in ids and "C1-KOR#4" in ids
    assert "C2-MUS#2" in ids
    per_course: dict[str, int] = {}
    for course_id in conv.lesson_courses:
        per_course[course_id] = per_course.get(course_id, 0) + 1
    for course_id, course in conv.courses_by_id.items():
        assert per_course[course_id] == course["periods_per_week"]

    # Quickstart limit: with team teaching only teacher_ids[0] becomes the engine teacher.
    mus = next(l for l in table.lessons if l.id == "C1-MUS#1")
    assert mus.teacher == "T_MUS"
    assert mus.student_group == "C1"
    assert any("team-taught" in note for note in conv.notes)


def test_room_mapping(problem):
    conv = quickstart.convert_problem(problem)

    # Two placeholder home rooms + the two special rooms actually used; the music room is dropped.
    assert conv.room_ids[:2] == [None, None]
    assert set(conv.room_ids[2:]) == {"R_SCI", "R_GYM"}
    assert any("R_MUSIC" in note for note in conv.notes)

    # Teacher unavailability: (day 0, period 1) is timeslot index 0.
    assert conv.table.teacher_unavailable["T_KOR"] == {0}


def test_missing_special_room_raises(problem):
    problem["rooms"] = [r for r in problem["rooms"] if r["room_type"] != "science lab"]
    with pytest.raises(ValueError, match="science lab"):
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
    cfg.best_score_limit = (0, -1e18)  # stop at the first feasible solution
    st.Solver(cfg).solve(conv.table)
    assert conv.table.assignment()[pinned_index] == expected

    # The cell shows up in the canonical output too (gym matched -> room_id filled).
    result = quickstart.convert_solution(conv)
    pe = [l for l in result["lessons"] if l["course_id"] == "C1-PE"]
    fixed = next(l for l in pe if l["slot"] == {"day": 2, "period": 3})
    assert fixed["room_id"] == "R_GYM"


def test_round_trip_with_room_repair(problem):
    """Builds an engine assignment by hand to check canonical regrouping and the special-room repair."""
    conv = quickstart.convert_problem(problem)
    table = conv.table
    room_count = table.room_count()
    homeroom = {"C1": 0, "C2": 1}
    sci_room = conv.room_ids.index("R_SCI")

    # C1 places its 20 lessons in slots 0..19 in order, C2 shifted by 4 cells.
    # Science lessons go to the science lab — except C2-SCI#1, put on purpose in a
    # placeholder room at the same time as C1-SCI#1, to exercise the failure path
    # where the repair finds no free science lab (room_id=null + notes).
    # C2-SCI#2/#3 also sit in placeholder rooms, but the science lab is free at
    # those times, so the repair must give them R_SCI.
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
                slot = 12  # the time C1-SCI#1 uses the science lab
            elif group == "C1":
                room = sci_room
        assignment.append(slot * room_count + room)
    table.set_assignment(assignment)

    result = quickstart.convert_solution(conv, wall_time_seconds=1.5)
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

    # Slot decoding: C1's lessons are slots 0..19 = (day 0..4, period 1..4) in order.
    c1_slots = [(l["slot"]["day"], l["slot"]["period"]) for l in result["lessons"] if l["class_group_id"] == "C1"]
    assert c1_slots == [(d, p) for d in range(5) for p in range(1, 5)]

    # room_id convention: null for regular lessons, science lessons in the science lab (kept or repaired).
    for lesson in per_course["C1-KOR"]:
        assert lesson["room_id"] is None
    assert all(l["room_id"] == "R_SCI" for l in per_course["C1-SCI"])
    c2_rooms = sorted((l["room_id"] is None, l["room_id"] or "") for l in per_course["C2-SCI"])
    # C2-SCI#2/#3 are repaired into the free science lab; #1 fails because the lab is taken.
    assert c2_rooms == [(False, "R_SCI"), (False, "R_SCI"), (True, "")]
    assert any("C2-SCI" in note and "no special room" in note for note in result["notes"])
