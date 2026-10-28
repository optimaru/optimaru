"""fixed_slots -> pinned 변환(H2)과 solve 후 고정 유지 검증 (canonical 도메인)."""

import st_timetabling as st
from st_timetabling import canon, convert_problem


def test_fixed_slot_becomes_pinned_lesson(problem):
    conv = convert_problem(problem)
    lessons = conv.table.lessons
    pe1 = next(l for l in lessons if l.id == "C1-PE#1")
    assert pe1.pinned
    assert pe1.slot == conv.slot_index[(2, 3)]
    # 나머지 체육 수업은 자유 배정(미배정 시작).
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
    cfg.best_score_limit = (0, -1e18)  # 첫 feasible 에서 종료
    canon.Solver(cfg).solve(conv.table)

    assert conv.table.assignment()[pinned_index] == expected

    # canonical 출력에도 그 칸이 그대로 나타난다 (체육관 매칭 -> room_id 채움).
    result = st.convert_solution(conv)
    pe = [l for l in result["lessons"] if l["course_id"] == "C1-PE"]
    assert {"day": 2, "period": 3} in [l["slot"] for l in pe]
    fixed = next(l for l in pe if l["slot"] == {"day": 2, "period": 3})
    assert fixed["room_id"] == "R_GYM"
