"""컴시간풍 데모 학교.

- 5일제: 월~목 7교시, 금 6교시 -> 학급당 주 34칸
- 3개 학년 x 2반 (1-1 ~ 3-2)
- 과목 11개(창체 포함), 교사 15명(불가시간/담임/주당 상한 일부)
- 특별실: 과학실 2, 음악실 1, 미술실 1, 체육관 1
- 미술은 주 1회 2연강(block_periods=2)
- 1-1 기술가정은 기술/가정 분반(parallel_group)
- 2-1 영어 1시간은 두 교사 팀티칭
- 창체(학급회의)는 월요일 1교시 고정, 담임이 진행

학급마다 주당 시수 합(33 + 창체 1 = 34)이 가용 슬롯 수와 정확히 일치해서
빈칸 없이 꽉 채워지는 시간표가 나온다.
"""

from teacher_scheduling.domain.entities import ClassGroup, Course, Room, Subject, Teacher, TimeSlot
from teacher_scheduling.domain.enums import RoomType, Weekday
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem

# 학급별 주당 시수표. 합계 33 (창체 1시간은 별도) = 34 - 1.
_CURRICULUM: dict[str, int] = {
    "KOR": 5,
    "MATH": 5,
    "ENG": 5,
    "SCI": 4,
    "SOC": 4,
    "PE": 3,
    "MUS": 2,
    "ART": 2,
    "TECH": 2,
    "ETH": 1,
}

# 과목 -> {학급: 담당 교사}. C11 기술가정은 분반이라 여기 없다.
_SUBJECT_TEACHERS: dict[str, dict[str, str]] = {
    "KOR": {"C11": "T01", "C12": "T01", "C21": "T01", "C22": "T02", "C31": "T02", "C32": "T02"},
    "MATH": {"C11": "T03", "C12": "T03", "C21": "T03", "C22": "T04", "C31": "T04", "C32": "T04"},
    "ENG": {"C11": "T05", "C21": "T05", "C31": "T05", "C12": "T06", "C22": "T06", "C32": "T06"},
    "SCI": {"C21": "T07", "C22": "T07", "C31": "T07", "C11": "T08", "C12": "T08", "C32": "T08"},
    "SOC": {"C11": "T09", "C12": "T09", "C31": "T09", "C21": "T10", "C22": "T10", "C32": "T10"},
    "ETH": {"C11": "T09", "C12": "T09", "C31": "T09", "C21": "T10", "C22": "T10", "C32": "T10"},
    "PE": {c: "T11" for c in ("C11", "C12", "C21", "C22", "C31", "C32")},
    "MUS": {c: "T12" for c in ("C11", "C12", "C21", "C22", "C31", "C32")},
    "ART": {c: "T13" for c in ("C11", "C12", "C21", "C22", "C31", "C32")},
    "TECH": {"C12": "T14", "C21": "T14", "C31": "T14", "C22": "T15", "C32": "T15"},
}

# 담임 배정. 창체(월1 학급회의)를 진행한다.
_HOMEROOMS: dict[str, str] = {
    "C11": "T01",
    "C12": "T03",
    "C21": "T05",
    "C22": "T07",
    "C31": "T09",
    "C32": "T11",
}


def build_sample_problem() -> TimetableProblem:
    return TimetableProblem(
        calendar=SchoolCalendar(
            periods_per_day={
                Weekday.MON: 7,
                Weekday.TUE: 7,
                Weekday.WED: 7,
                Weekday.THU: 7,
                Weekday.FRI: 6,
            }
        ),
        subjects=_subjects(),
        teachers=_teachers(),
        class_groups=_class_groups(),
        rooms=_rooms(),
        courses=_courses(),
    )


def _subjects() -> tuple[Subject, ...]:
    return (
        Subject(id="KOR", name="국어"),
        Subject(id="MATH", name="수학", preferred_periods=frozenset({1, 2, 3, 4})),
        Subject(id="ENG", name="영어"),
        Subject(id="SCI", name="과학", required_room_type=RoomType.SCIENCE_LAB),
        Subject(id="SOC", name="사회"),
        Subject(id="PE", name="체육", required_room_type=RoomType.GYM, avoided_periods=frozenset({1})),
        Subject(id="MUS", name="음악", required_room_type=RoomType.MUSIC_ROOM),
        Subject(id="ART", name="미술", required_room_type=RoomType.ART_ROOM),
        Subject(id="TECH", name="기술가정"),
        Subject(id="ETH", name="도덕"),
        Subject(id="HR", name="창체"),
    )


def _teachers() -> tuple[Teacher, ...]:
    return (
        Teacher(
            id="T01",
            name="김국어",
            subject_ids=frozenset({"KOR", "HR"}),
            homeroom_class_id="C11",
            max_periods_per_week=18,
        ),
        Teacher(
            id="T02",
            name="이국어",
            subject_ids=frozenset({"KOR"}),
            # 월/화 7교시는 교육청 연수로 자리를 비운다.
            unavailable=frozenset({TimeSlot(day=Weekday.MON, period=7), TimeSlot(day=Weekday.TUE, period=7)}),
        ),
        Teacher(id="T03", name="박수학", subject_ids=frozenset({"MATH", "HR"}), homeroom_class_id="C12"),
        Teacher(
            id="T04",
            name="최수학",
            subject_ids=frozenset({"MATH"}),
            unavailable=frozenset({TimeSlot(day=Weekday.WED, period=6), TimeSlot(day=Weekday.WED, period=7)}),
            preferred_free_days=frozenset({Weekday.FRI}),
        ),
        Teacher(id="T05", name="정영어", subject_ids=frozenset({"ENG", "HR"}), homeroom_class_id="C21"),
        Teacher(id="T06", name="한영어", subject_ids=frozenset({"ENG"})),
        Teacher(id="T07", name="조과학", subject_ids=frozenset({"SCI", "HR"}), homeroom_class_id="C22"),
        Teacher(id="T08", name="윤과학", subject_ids=frozenset({"SCI"})),
        Teacher(id="T09", name="장사회", subject_ids=frozenset({"SOC", "ETH", "HR"}), homeroom_class_id="C31"),
        Teacher(id="T10", name="임사회", subject_ids=frozenset({"SOC", "ETH"})),
        Teacher(
            id="T11",
            name="강체육",
            subject_ids=frozenset({"PE", "HR"}),
            homeroom_class_id="C32",
            max_periods_per_week=20,
        ),
        Teacher(
            id="T12",
            name="서음악",
            subject_ids=frozenset({"MUS"}),
            # 금요일은 겸임교 출강이라 하루 종일 불가.
            unavailable=frozenset(TimeSlot(day=Weekday.FRI, period=p) for p in range(1, 7)),
        ),
        Teacher(id="T13", name="신미술", subject_ids=frozenset({"ART"}), preferred_free_days=frozenset({Weekday.FRI})),
        Teacher(id="T14", name="오기술", subject_ids=frozenset({"TECH"})),
        Teacher(id="T15", name="문가정", subject_ids=frozenset({"TECH"})),
    )


def _class_groups() -> tuple[ClassGroup, ...]:
    return tuple(
        ClassGroup(id=f"C{grade}{section}", name=f"{grade}-{section}", grade=grade)
        for grade in (1, 2, 3)
        for section in (1, 2)
    )


def _rooms() -> tuple[Room, ...]:
    return (
        Room(id="R_SCI1", name="과학실1", room_type=RoomType.SCIENCE_LAB),
        Room(id="R_SCI2", name="과학실2", room_type=RoomType.SCIENCE_LAB),
        Room(id="R_MUS", name="음악실", room_type=RoomType.MUSIC_ROOM),
        Room(id="R_ART", name="미술실", room_type=RoomType.ART_ROOM),
        Room(id="R_GYM", name="체육관", room_type=RoomType.GYM),
    )


def _courses() -> tuple[Course, ...]:
    courses: list[Course] = []
    for subject_id, hours in _CURRICULUM.items():
        for class_id, teacher_id in _SUBJECT_TEACHERS[subject_id].items():
            periods = hours
            if subject_id == "ENG" and class_id == "C21":
                periods = 4  # 나머지 1시간은 아래 팀티칭 강좌
            courses.append(
                Course(
                    id=f"{class_id}-{subject_id}",
                    class_group_id=class_id,
                    subject_id=subject_id,
                    teacher_ids=(teacher_id,),
                    periods_per_week=periods,
                    block_periods=2 if subject_id == "ART" else 0,  # 미술은 주 1회 2연강
                )
            )

    # 팀티칭: 2-1 영어 회화 1시간은 두 영어 교사가 함께 들어간다.
    courses.append(
        Course(id="C21-ENG-TT", class_group_id="C21", subject_id="ENG", teacher_ids=("T05", "T06"), periods_per_week=1)
    )

    # 분반: 1-1 기술가정은 기술/가정 두 반으로 나눠 같은 시간에 진행한다.
    for suffix, teacher_id in (("A", "T14"), ("B", "T15")):
        courses.append(
            Course(
                id=f"C11-TECH-{suffix}",
                class_group_id="C11",
                subject_id="TECH",
                teacher_ids=(teacher_id,),
                periods_per_week=2,
                parallel_group="C11-TECH",
            )
        )

    # 창체(학급회의): 담임이 월요일 1교시 고정으로 진행한다.
    for class_id, teacher_id in _HOMEROOMS.items():
        courses.append(
            Course(
                id=f"{class_id}-HR",
                class_group_id=class_id,
                subject_id="HR",
                teacher_ids=(teacher_id,),
                periods_per_week=1,
                fixed_slots=frozenset({TimeSlot(day=Weekday.MON, period=1)}),
            )
        )

    return tuple(courses)


__all__ = ["build_sample_problem"]
