"""fixed_slots -> pinned conversion (H2) and pins kept after solve (canonical domain)."""

import pytest
import st_timetabling as st
from st_timetabling import canon, convert_problem


def test_fixed_slot_becomes_pinned_lesson(problem):
    conv = convert_problem(problem)
    lessons = conv.table.lessons
    pe1 = next(l for l in lessons if l.id == "C1-PE#1")
    assert pe1.pinned
    assert pe1.slot == conv.slot_index[(2, 3)]
    # The other PE lessons are free (they start unassigned).
    pe2 = next(l for l in lessons if l.id == "C1-PE#2")
    assert not pe2.pinned
    assert pe2.slot == -1


def test_pinned_lesson_survives_solve(problem):
    conv = convert_problem(problem)
    expected = conv.slot_index[(2, 3)]
    pinned_index = next(i for i, l in enumerate(conv.table.lessons) if l.id == "C1-PE#1")

    cfg = canon.SolverConfig()
    cfg.spent_limit_seconds = 10.0
    cfg.random_seed = 7
    cfg.best_score_limit = (0, -1e18)  # stop at the first feasible solution
    canon.Solver(cfg).solve(conv.table)

    assert conv.table.assignment()[pinned_index] == expected

    # The cell shows up in the canonical output too (gym matched -> room_id filled).
    result = st.convert_solution(conv)
    pe = [l for l in result["lessons"] if l["course_id"] == "C1-PE"]
    assert {"day": 2, "period": 3} in [l["slot"] for l in pe]
    fixed = next(l for l in pe if l["slot"] == {"day": 2, "period": 3})
    assert fixed["room_id"] == "R_GYM"


def _solved_table(problem, pin):
    """The lessons at reversed slots, so moving a pin would improve the score."""
    conv = convert_problem(problem)
    cfg = canon.SolverConfig()
    cfg.spent_limit_seconds = 0.1
    cfg.random_seed = 1
    canon.Solver(cfg).solve(conv.table)
    values = conv.table.assignment()[::-1]
    for index, lesson in enumerate(conv.table.lessons):
        lesson.slot = values[index]
        lesson.pinned = pin(index)
    cfg.spent_limit_seconds = 0.2
    cfg.random_seed = 2
    canon.Solver(cfg).solve(conv.table)
    return values, conv.table.assignment()


def test_a_fully_pinned_timetable_is_not_moved(problem):
    values, solved = _solved_table(problem, lambda index: True)
    assert solved == values


def test_every_pin_holds_with_one_free_lesson(problem):
    values, solved = _solved_table(problem, lambda index: index != 0)
    assert solved[1:] == values[1:]


def test_a_pinned_lesson_without_a_slot_is_refused(problem):
    conv = convert_problem(problem)
    conv.table.lessons[0].pinned = True
    conv.table.lessons[0].slot = -1
    cfg = canon.SolverConfig()
    cfg.spent_limit_seconds = 0.1
    with pytest.raises(ValueError, match="is pinned but has no value"):
        canon.Solver(cfg).solve(conv.table)
