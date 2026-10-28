"""Shared fixture: a canonical demo problem in API_SPEC.md's shape.

One small demo school — 5 days x 4 periods (20 cells), 2 classes, 6 subjects
(2 special-room types), 7 teachers (one team-taught course); each class has 20
weekly periods, so the grid is exactly full. C1-PE is fixed to Wednesday
period 3 (fixed_slots); Kim (T_KOR) is unavailable Monday period 1.
"""

import pytest


def demo_problem() -> dict:
    """Canonical TimetableProblem JSON (as a dict; keys are strings as in JSON)."""
    subjects = [
        {"id": "KOR", "name": "Korean", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "MATH", "name": "Math", "required_room_type": None, "preferred_periods": [1, 2], "avoided_periods": []},
        {"id": "ENG", "name": "English", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
        {"id": "SCI", "name": "Science", "required_room_type": "science lab", "preferred_periods": [], "avoided_periods": []},
        {"id": "PE", "name": "PE", "required_room_type": "gym", "preferred_periods": [], "avoided_periods": [1]},
        {"id": "MUS", "name": "Music", "required_room_type": None, "preferred_periods": [], "avoided_periods": []},
    ]
    teachers = [
        {"id": "T_KOR", "name": "Kim (Korean)", "subject_ids": ["KOR"], "unavailable": [{"day": 0, "period": 1}]},
        {"id": "T_MATH", "name": "Park (Math)", "subject_ids": ["MATH"], "unavailable": []},
        {"id": "T_ENG", "name": "Jung (English)", "subject_ids": ["ENG"], "unavailable": []},
        {"id": "T_SCI", "name": "Cho (Science)", "subject_ids": ["SCI"], "unavailable": []},
        {"id": "T_PE", "name": "Lim (PE)", "subject_ids": ["PE"], "unavailable": []},
        {"id": "T_MUS", "name": "Han (Music)", "subject_ids": ["MUS"], "unavailable": []},
        {"id": "T_ASSIST", "name": "Assistant (Music)", "subject_ids": ["MUS"], "unavailable": []},
    ]
    # 20 weekly periods = 5 days x 4 periods: both classes fill the grid.
    hours = {"KOR": 4, "MATH": 4, "ENG": 4, "SCI": 3, "PE": 3, "MUS": 2}
    teacher_of = {"KOR": "T_KOR", "MATH": "T_MATH", "ENG": "T_ENG", "SCI": "T_SCI", "PE": "T_PE", "MUS": "T_MUS"}
    courses = []
    for cg in ("C1", "C2"):
        for subject_id, count in hours.items():
            teacher_ids = [teacher_of[subject_id]]
            if subject_id == "MUS":
                teacher_ids.append("T_ASSIST")  # team teaching
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
            {"id": "C1", "name": "Grade 1 Class 1", "grade": 1, "unavailable": []},
            {"id": "C2", "name": "Grade 1 Class 2", "grade": 1, "unavailable": []},
        ],
        "rooms": [
            {"id": "R_SCI", "name": "Science lab 1", "room_type": "science lab"},
            {"id": "R_GYM", "name": "Gym", "room_type": "gym"},
            {"id": "R_MUSIC", "name": "Music room", "room_type": "music room"},  # unused
        ],
        "courses": courses,
    }


@pytest.fixture
def problem() -> dict:
    return demo_problem()
