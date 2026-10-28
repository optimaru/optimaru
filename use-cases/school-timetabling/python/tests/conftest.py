"""공용 픽스처: API_SPEC.md 형태의 canonical 데모 문제.

작은 데모 학교 하나 — 5일 x 4교시(칸 20개), 학급 2개, 과목 6개(특별실 2종),
교사 7명(팀티칭 1건), 학급마다 주 20시수로 그리드가 정확히 꽉 찬다.
C1-PE 는 수요일 3교시 고정(fixed_slots), 김국어(T_KOR)는 월요일 1교시 불가.
"""

import pytest


def demo_problem() -> dict:
    """canonical TimetableProblem JSON (dict 형태; key 는 JSON 처럼 문자열)."""
    subjects = [
        {"id": "KOR", "name": "국어", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "MATH", "name": "수학", "required_room_type": None, "preferred_periods": [1, 2], "avoided_periods": []},
        {"id": "ENG", "name": "영어", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "SCI", "name": "과학", "required_room_type": "과학실", "preferred_periods": [], "avoided_periods": []},
        {"id": "PE", "name": "체육", "required_room_type": "체육관", "preferred_periods": [], "avoided_periods": [1]},
        {"id": "MUS", "name": "음악", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
    ]
    teachers = [
        {"id": "T_KOR", "name": "김국어", "subject_ids": ["KOR"], "unavailable": [{"day": 0, "period": 1}]},
        {"id": "T_MATH", "name": "박수학", "subject_ids": ["MATH"], "unavailable": []},
        {"id": "T_ENG", "name": "정영어", "subject_ids": ["ENG"], "unavailable": []},
        {"id": "T_SCI", "name": "조과학", "subject_ids": ["SCI"], "unavailable": []},
        {"id": "T_PE", "name": "임체육", "subject_ids": ["PE"], "unavailable": []},
        {"id": "T_MUS", "name": "한음악", "subject_ids": ["MUS"], "unavailable": []},
        {"id": "T_ASSIST", "name": "보조음악", "subject_ids": ["MUS"], "unavailable": []},
    ]
    # 주 20시수 = 5일 x 4교시: 두 학급 모두 그리드가 꽉 찬다.
    hours = {"KOR": 4, "MATH": 4, "ENG": 4, "SCI": 3, "PE": 3, "MUS": 2}
    teacher_of = {"KOR": "T_KOR", "MATH": "T_MATH", "ENG": "T_ENG", "SCI": "T_SCI", "PE": "T_PE", "MUS": "T_MUS"}
    courses = []
    for cg in ("C1", "C2"):
        for subject_id, count in hours.items():
            teacher_ids = [teacher_of[subject_id]]
            if subject_id == "MUS":
                teacher_ids.append("T_ASSIST")  # 팀티칭
            courses.append(
                {
                    "id": f"{cg}-{subject_id}",
                    "class_group_id": cg,
                    "subject_id": subject_id,
                    "teacher_ids": teacher_ids,
                    "periods_per_week": count,
                    "block_periods": 0,
                    "max_periods_per_day": 1,
                    "fixed_slots": ([{"day": 2, "period": 3}] if (cg, subject_id) == ("C1", "PE") else []),
                    "forbidden_slots": [],
                    "parallel_group": None,
                }
            )
    return {
        "calendar": {"periods_per_day": {"0": 4, "1": 4, "2": 4, "3": 4, "4": 4}},
        "subjects": subjects,
        "teachers": teachers,
        "class_groups": [
            {"id": "C1", "name": "1학년 1반", "grade": 1, "unavailable": []},
            {"id": "C2", "name": "1학년 2반", "grade": 1, "unavailable": []},
        ],
        "rooms": [
            {"id": "R_SCI", "name": "과학실 1", "room_type": "과학실"},
            {"id": "R_GYM", "name": "체육관", "room_type": "체육관"},
            {"id": "R_MUSIC", "name": "음악실", "room_type": "음악실"},  # 안 쓰임
        ],
        "courses": courses,
    }


@pytest.fixture
def problem() -> dict:
    return demo_problem()
