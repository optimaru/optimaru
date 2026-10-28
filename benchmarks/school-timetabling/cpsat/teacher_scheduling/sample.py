"""A Comcigan-style demo school.

- 5-day week: Mon–Thu 7 periods, Fri 6 periods -> 34 cells per class
- 3 grades x 2 classes (1-1 … 3-2)
- 11 subjects (homeroom included), 15 teachers (some unavailability /
  homeroom duty / weekly caps)
- Special rooms: 2 science labs, 1 music room, 1 art room, 1 gym
- Art once a week as a 2-period block (block_periods=2)
- 1-1's technology/home economics is a technology / home-economics split (parallel_group)
- One hour of 2-1's English is team-taught by two teachers
- Homeroom (class meeting) is fixed to Monday period 1, run by the homeroom teacher

Each class's weekly periods (33 + homeroom 1 = 34) equal the available slots
exactly, so the timetable fills up with no empty cell.
"""

from teacher_scheduling.domain.entities import ClassGroup, Course, Room, Subject, Teacher, TimeSlot
from teacher_scheduling.domain.enums import RoomType, Weekday
from teacher_scheduling.domain.problem import SchoolCalendar, TimetableProblem

# Weekly periods per class. Total 33 (plus the separate homeroom hour) = 34 - 1.
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

# subject -> {class: teacher}. C11's technology/home economics is a split and not listed here.
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

# Homeroom teachers. They run the homeroom hour (Monday period 1 class meeting).
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
        Subject(id="KOR", name="Korean"),
        Subject(id="MATH", name="Math", preferred_periods=frozenset({1, 2, 3, 4})),
        Subject(id="ENG", name="English"),
        Subject(id="SCI", name="Science", required_room_type=RoomType.SCIENCE_LAB),
        Subject(id="SOC", name="Social studies"),
        Subject(id="PE", name="PE", required_room_type=RoomType.GYM, avoided_periods=frozenset({1})),
        Subject(id="MUS", name="Music", required_room_type=RoomType.MUSIC_ROOM),
        Subject(id="ART", name="Art", required_room_type=RoomType.ART_ROOM),
        Subject(id="TECH", name="Technology and Home Economics"),
        Subject(id="ETH", name="Ethics"),
        Subject(id="HR", name="Homeroom"),
    )


def _teachers() -> tuple[Teacher, ...]:
    return (
        Teacher(
            id="T01",
            name="Kim (Korean)",
            subject_ids=frozenset({"KOR", "HR"}),
            homeroom_class_id="C11",
            max_periods_per_week=18,
        ),
        Teacher(
            id="T02",
            name="Lee (Korean)",
            subject_ids=frozenset({"KOR"}),
            # Away for district training in period 7 on Mon/Tue.
            unavailable=frozenset({TimeSlot(day=Weekday.MON, period=7), TimeSlot(day=Weekday.TUE, period=7)}),
        ),
        Teacher(id="T03", name="Park (Math)", subject_ids=frozenset({"MATH", "HR"}), homeroom_class_id="C12"),
        Teacher(
            id="T04",
            name="Choi (Math)",
            subject_ids=frozenset({"MATH"}),
            unavailable=frozenset({TimeSlot(day=Weekday.WED, period=6), TimeSlot(day=Weekday.WED, period=7)}),
            preferred_free_days=frozenset({Weekday.FRI}),
        ),
        Teacher(id="T05", name="Jung (English)", subject_ids=frozenset({"ENG", "HR"}), homeroom_class_id="C21"),
        Teacher(id="T06", name="Han (English)", subject_ids=frozenset({"ENG"})),
        Teacher(id="T07", name="Cho (Science)", subject_ids=frozenset({"SCI", "HR"}), homeroom_class_id="C22"),
        Teacher(id="T08", name="Yoon (Science)", subject_ids=frozenset({"SCI"})),
        Teacher(id="T09", name="Jang (Social)", subject_ids=frozenset({"SOC", "ETH", "HR"}), homeroom_class_id="C31"),
        Teacher(id="T10", name="Lim (Social)", subject_ids=frozenset({"SOC", "ETH"})),
        Teacher(
            id="T11",
            name="Kang (PE)",
            subject_ids=frozenset({"PE", "HR"}),
            homeroom_class_id="C32",
            max_periods_per_week=20,
        ),
        Teacher(
            id="T12",
            name="Seo (Music)",
            subject_ids=frozenset({"MUS"}),
            # Teaches at another school on Fridays, so unavailable all day.
            unavailable=frozenset(TimeSlot(day=Weekday.FRI, period=p) for p in range(1, 7)),
        ),
        Teacher(id="T13", name="Shin (Art)", subject_ids=frozenset({"ART"}), preferred_free_days=frozenset({Weekday.FRI})),
        Teacher(id="T14", name="Oh (Technology)", subject_ids=frozenset({"TECH"})),
        Teacher(id="T15", name="Moon (Home economics)", subject_ids=frozenset({"TECH"})),
    )


def _class_groups() -> tuple[ClassGroup, ...]:
    return tuple(
        ClassGroup(id=f"C{grade}{section}", name=f"{grade}-{section}", grade=grade)
        for grade in (1, 2, 3)
        for section in (1, 2)
    )


def _rooms() -> tuple[Room, ...]:
    return (
        Room(id="R_SCI1", name="Science lab 1", room_type=RoomType.SCIENCE_LAB),
        Room(id="R_SCI2", name="Science lab 2", room_type=RoomType.SCIENCE_LAB),
        Room(id="R_MUS", name="Music room", room_type=RoomType.MUSIC_ROOM),
        Room(id="R_ART", name="Art room", room_type=RoomType.ART_ROOM),
        Room(id="R_GYM", name="Gym", room_type=RoomType.GYM),
    )


def _courses() -> tuple[Course, ...]:
    courses: list[Course] = []
    for subject_id, hours in _CURRICULUM.items():
        for class_id, teacher_id in _SUBJECT_TEACHERS[subject_id].items():
            periods = hours
            if subject_id == "ENG" and class_id == "C21":
                periods = 4  # the remaining hour is the team-taught course below
            courses.append(
                Course(
                    id=f"{class_id}-{subject_id}",
                    class_group_id=class_id,
                    subject_id=subject_id,
                    teacher_ids=(teacher_id,),
                    periods_per_week=periods,
                    block_periods=2 if subject_id == "ART" else 0,  # art: one 2-period block a week
                )
            )

    # Team teaching: one hour of 2-1's English conversation has both English teachers.
    courses.append(
        Course(id="C21-ENG-TT", class_group_id="C21", subject_id="ENG", teacher_ids=("T05", "T06"), periods_per_week=1)
    )

    # Parallel sections: 1-1's technology/home economics splits into two groups at the same time.
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

    # Homeroom (class meeting): run by the homeroom teacher, fixed to Monday period 1.
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
