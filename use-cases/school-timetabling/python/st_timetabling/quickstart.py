"""quickstart 모드 전용 변환: canonical JSON <-> quickstart 도메인(`_core.Timetable`).

`solve(mode="quickstart")` 가 쓰는 근사 변환이다 — 풀-패리티가 필요하면
canonical 변환(`st_timetabling.convert`, st::canon 도메인)을 쓴다. 여기 남은
"미지원" 한계들은 quickstart 도메인(6제약 + slotRoom 합성 인코딩)의 표현력
한계이며, canonical 모드에는 해당하지 않는다.

canonical 문제 JSON 을 quickstart 형태(Lesson 단위)로 펼친다:
강좌(course) 하나는 `periods_per_week` 개의 Lesson 이 되고(id 는 "{course_id}#1" 식),
`teacher_ids[0]` 이 담당 교사가 된다. `fixed_slots` 는 해당 시간 + 적정 교실로
pinned 처리한다.

교실 매핑 전략
--------------
엔진은 entity 당 BASIC 변수 1개(slotRoom 합성 인코딩)만 알고, "이 수업은 이
교실 타입만 가능"이라는 hard 제약이 없다. 그래서:

- 학급마다 가짜 "학급 전용 교실"을 하나씩 만든다. 일반 수업(특별실 불필요)은
  개념상 늘 자기 교실에서 진행되므로, 결과 변환 시 room_id 는 항상 null 이다
  (API_SPEC 의 규약과 동일). 엔진이 일반 수업을 다른 교실에 두더라도 canonical
  관점에서는 무해하다 — 학급 충돌은 studentGroupConflict 가 이미 막는다.
- 특별실이 필요한 수업은 실제 특별실(해당 room_type)들이 후보다. 엔진의
  roomConflict 가 특별실 중복 사용을 hard 로 막아 주지만, 수업이 특별실
  '밖'(가짜 교실 등)에 배정되는 것은 hard 로 막지 못한다. 그래서 결과 변환 시
  timeslot 마다 같은 시간의 빈 특별실로 재배정(repair)한다 — 일반 수업은
  canonical 관점에서 특별실을 점유하지 않으므로 항상 안전하다. 같은 시간에
  같은 타입의 수업이 특별실 수보다 많으면(엔진이 못 막는 유일한 경우) 그
  수업은 room_id=null + notes 로 보고된다.
- 어떤 강좌도 쓰지 않는 타입의 특별실은 탐색 공간만 키우므로 버린다(notes 기록).

미지원 제약(변환 불가 — notes 에 명시)
--------------------------------------
block_periods(연강), parallel_group(분반), forbidden_slots, 학급 unavailable,
교사별 시수 상한(max_periods_per_week/day, max_consecutive_periods),
preferred_free_days, 과목 선호/기피 교시, 팀티칭 부교사의 충돌 검사.
soft 목적식도 canonical weights 대신 엔진 내장 제약(교사 이동 최소화 ·
교사 연강 선호 · 학급 과목 분산)으로 대체된다.
"""

from dataclasses import dataclass, field

from . import _core

__all__ = ["ConvertedProblem", "convert_problem", "convert_solution"]

# 가짜 학급 전용 교실의 room_type (실제 특별실 타입과 겹치지 않는 내부용 표식).
CLASS_ROOM_TYPE = "__학급전용__"


@dataclass
class ConvertedProblem:
    """convert_problem 의 결과: 엔진 solution + 역변환에 필요한 메타데이터."""

    table: "_core.Timetable"
    timeslots: list[tuple[int, int]]  # timeslot index -> (day, period)
    slot_index: dict[tuple[int, int], int]  # (day, period) -> timeslot index
    room_ids: list[str | None]  # room index -> canonical room id (None = 가짜 교실)
    room_types: list[str]  # room index -> room_type
    lesson_courses: list[str]  # lesson index -> course id
    courses_by_id: dict[str, dict]
    subjects_by_id: dict[str, dict]
    notes: list[str] = field(default_factory=list)


def _slot_key(slot: dict) -> tuple[int, int]:
    return (int(slot["day"]), int(slot["period"]))


def convert_problem(problem: dict) -> ConvertedProblem:
    """canonical TimetableProblem dict 를 엔진 Timetable 로 변환한다.

    참조 오류(없는 과목/학급/특별실)는 ValueError 로 올린다 — canonical pydantic
    검증(HTTP 422)과 같은 계층의 실패다.
    """
    notes: list[str] = []

    # --- timeslots: 요일별 교시 수 -> (day, period) 그리드 ---
    periods_per_day = {int(day): int(count) for day, count in problem["calendar"]["periods_per_day"].items()}
    timeslots = [(day, period) for day in sorted(periods_per_day) for period in range(1, periods_per_day[day] + 1)]
    slot_index = {slot: i for i, slot in enumerate(timeslots)}

    subjects_by_id = {s["id"]: s for s in problem["subjects"]}
    class_groups = list(problem["class_groups"])
    class_ids = {cg["id"] for cg in class_groups}
    courses = list(problem["courses"])
    courses_by_id = {c["id"]: c for c in courses}

    def required_room_type(course: dict) -> str | None:
        subject = subjects_by_id.get(course["subject_id"])
        if subject is None:
            raise ValueError(f"[{course['id']}] 존재하지 않는 과목: {course['subject_id']}")
        return subject.get("required_room_type")

    # --- rooms: 학급 전용 가짜 교실 + 실제로 필요한 특별실 ---
    rooms: list[_core.Room] = []
    room_ids: list[str | None] = []
    room_types: list[str] = []
    homeroom_index: dict[str, int] = {}
    for cg in class_groups:
        homeroom_index[cg["id"]] = len(rooms)
        rooms.append(_core.Room(cg["id"], CLASS_ROOM_TYPE))
        room_ids.append(None)
        room_types.append(CLASS_ROOM_TYPE)

    needed_types = {rt for c in courses if (rt := required_room_type(c)) is not None}
    rooms_of_type: dict[str, list[int]] = {rt: [] for rt in needed_types}
    dropped: list[str] = []
    for room in problem.get("rooms", []):
        if room["room_type"] in needed_types:
            rooms_of_type[room["room_type"]].append(len(rooms))
            rooms.append(_core.Room(room["id"], room["room_type"]))
            room_ids.append(room["id"])
            room_types.append(room["room_type"])
        else:
            dropped.append(room["id"])
    for room_type in sorted(needed_types):
        if not rooms_of_type[room_type]:
            raise ValueError(f"필요한 특별실이 없습니다: {room_type}")
    if dropped:
        notes.append(f"사용하는 강좌가 없어 제외한 교실: {', '.join(dropped)}")
    if needed_types:
        notes.append(
            "required_room_type 은 엔진에서 hard 제약이 아니다: 특별실 배정은 "
            "결과 변환 시 같은 시간의 빈 특별실로 재배정되며, 특별실이 모자라면 "
            "room_id=null + notes 로 보고된다"
        )

    # --- lessons: 강좌 -> periods_per_week 개의 Lesson ---
    lessons: list[_core.Lesson] = []
    lesson_courses: list[str] = []
    pinned_used: set[tuple[int, int]] = set()  # (timeslot index, room index)
    team_teaching: list[str] = []
    blocked: list[str] = []
    parallel: set[str] = set()
    forbidden: list[str] = []

    for course in courses:
        course_id = course["id"]
        group_id = course["class_group_id"]
        if group_id not in class_ids:
            raise ValueError(f"[{course_id}] 존재하지 않는 학급: {group_id}")
        room_type = required_room_type(course)
        teacher_ids = list(course["teacher_ids"])
        if len(teacher_ids) > 1:
            team_teaching.append(course_id)
        if course.get("block_periods", 0) >= 2:
            blocked.append(course_id)
        if course.get("parallel_group") is not None:
            parallel.add(course["parallel_group"])
        if course.get("forbidden_slots"):
            forbidden.append(course_id)

        # fixed_slots 는 앞쪽 Lesson 부터 (day, period) 순으로 pinned 배정.
        fixed = sorted(_slot_key(s) for s in course.get("fixed_slots", []))
        for k in range(1, int(course["periods_per_week"]) + 1):
            lesson = _core.Lesson(f"{course_id}#{k}", course["subject_id"], teacher_ids[0], group_id)
            if k <= len(fixed):
                slot = fixed[k - 1]
                if slot not in slot_index:
                    raise ValueError(f"[{course_id}] 캘린더에 없는 fixed_slot: {slot}")
                ts = slot_index[slot]
                if room_type is None:
                    room = homeroom_index[group_id]
                else:
                    candidates = rooms_of_type[room_type]
                    room = next(
                        (r for r in candidates if (ts, r) not in pinned_used),
                        candidates[0],
                    )
                if (ts, room) in pinned_used:
                    notes.append(
                        f"{course_id}: fixed_slot {slot} 에 배정할 빈 교실이 없어 "
                        "고정 수업끼리 겹친다 (입력 자체가 과밀)"
                    )
                pinned_used.add((ts, room))
                lesson.slot_room = ts * len(rooms) + room
                lesson.pinned = True
            lessons.append(lesson)
            lesson_courses.append(course_id)

    # --- 교사 배정 불가 시간 (hard 4 확장) ---
    teacher_unavailable: dict[str, set[int]] = {}
    caps: list[str] = []
    free_days: list[str] = []
    for teacher in problem.get("teachers", []):
        slots = {slot_index[key] for s in teacher.get("unavailable", []) if (key := _slot_key(s)) in slot_index}
        if slots:
            teacher_unavailable[teacher["id"]] = slots
        if teacher.get("max_periods_per_week") is not None:
            caps.append(teacher["id"])
        if teacher.get("preferred_free_days"):
            free_days.append(teacher["id"])

    # --- 변환이 표현하지 못한 제약을 notes 로 남긴다 ---
    if team_teaching:
        notes.append(
            f"팀티칭 강좌는 teacher_ids[0] 만 담당 교사로 반영된다 (부교사 충돌 미검사): {', '.join(team_teaching)}"
        )
    if blocked:
        notes.append(f"block_periods(연강) 미지원 — 무시됨: {', '.join(blocked)}")
    if parallel:
        notes.append(
            f"parallel_group(분반) 미지원 — 강좌별 독립 배정되며 학급 칸도 따로 차지한다: {', '.join(sorted(parallel))}"
        )
    if forbidden:
        notes.append(f"forbidden_slots 미지원 — 무시됨: {', '.join(forbidden)}")
    unavailable_groups = [cg["id"] for cg in class_groups if cg.get("unavailable")]
    if unavailable_groups:
        notes.append(f"학급 unavailable 미지원 — 무시됨: {', '.join(unavailable_groups)}")
    if caps:
        notes.append(
            f"교사별 시수 상한(max_periods_per_week/day, max_consecutive_periods) 미지원 — 무시됨: {', '.join(caps)}"
        )
    if free_days:
        notes.append(f"preferred_free_days 미지원 — 무시됨: {', '.join(free_days)}")
    if any(s.get("preferred_periods") or s.get("avoided_periods") for s in problem["subjects"]):
        notes.append("과목 선호/기피 교시 미지원 — 무시됨")
    notes.append(
        "engine 백엔드의 soft 목적식은 canonical weights 대신 내장 제약"
        "(교사 이동 최소화 · 교사 연강 선호 · 학급 과목 분산)을 쓴다"
    )

    table = _core.Timetable(
        [_core.Timeslot(day, period) for day, period in timeslots],
        rooms,
        lessons,
        teacher_unavailable,
    )
    return ConvertedProblem(
        table=table,
        timeslots=timeslots,
        slot_index=slot_index,
        room_ids=room_ids,
        room_types=room_types,
        lesson_courses=lesson_courses,
        courses_by_id=courses_by_id,
        subjects_by_id=subjects_by_id,
        notes=notes,
    )


def convert_solution(converted: ConvertedProblem, *, wall_time_seconds: float = 0.0) -> dict:
    """엔진 결과(solve 로 mutate 된 `converted.table`)를 canonical Timetable
    JSON(dict) 으로 되돌린다. Lesson 들은 강좌별로 재집계되고 teacher_ids 는
    원본 강좌의 전체 튜플이 복원된다.

    특별실 repair: 엔진은 특별실 밖 배정을 hard 로 막지 못하므로, timeslot
    마다 필요한 타입의 특별실을 다시 매칭한다 — 맞는 타입에 이미 있던 수업은
    그 교실을 지키고, 나머지는 빈 특별실을 받는다. 일반 수업은 room_id=null
    (자기 교실)이라 특별실을 점유하지 않는다. 같은 시간에 특별실이 모자라면
    room_id=null + notes 로 보고된다."""
    table = converted.table
    score = table.score
    room_count = len(converted.room_ids)
    notes = list(converted.notes)
    assignment = table.assignment()

    def required_of(index: int) -> str | None:
        course = converted.courses_by_id[converted.lesson_courses[index]]
        return converted.subjects_by_id[course["subject_id"]].get("required_room_type")

    rooms_of_type: dict[str, list[int]] = {}
    for room_index, room_type in enumerate(converted.room_types):
        if room_type != CLASS_ROOM_TYPE:
            rooms_of_type.setdefault(room_type, []).append(room_index)

    # (timeslot, 필요 타입) 별로 특별실 수업을 모아 교실을 매칭한다.
    special: dict[tuple[int, str], list[int]] = {}
    for index, slot_room in enumerate(assignment):
        if slot_room < 0:
            continue
        required = required_of(index)
        if required is not None:
            special.setdefault((slot_room // room_count, required), []).append(index)

    repaired_room: dict[int, int | None] = {}  # lesson index -> room index
    for (_, required), members in special.items():
        taken: set[int] = set()
        pending: list[int] = []
        for index in members:  # 1차: 이미 맞는 타입의 교실에 있으면 유지
            room_index = assignment[index] % room_count
            if converted.room_types[room_index] == required and room_index not in taken:
                repaired_room[index] = room_index
                taken.add(room_index)
            else:
                pending.append(index)
        free = [r for r in rooms_of_type[required] if r not in taken]
        for index in pending:  # 2차: 빈 특별실로 재배정; 모자라면 실패 보고
            repaired_room[index] = free.pop(0) if free else None

    lessons_out: list[dict] = []
    for index, slot_room in enumerate(assignment):
        course = converted.courses_by_id[converted.lesson_courses[index]]
        if slot_room < 0:
            notes.append(f"{course['id']}: 미배정 수업이 남았다 (탐색 실패)")
            continue
        day, period = converted.timeslots[slot_room // room_count]

        # room_id 는 특별실이 필요한 수업에만 채워진다. 일반 수업은 항상 null
        # (자기 교실) — API_SPEC 규약.
        room_id = None
        required = required_of(index)
        if required is not None:
            room_index = repaired_room[index]
            if room_index is not None:
                room_id = converted.room_ids[room_index]
            else:
                notes.append(
                    f"{course['id']}: 특별실({required}) 배정 실패 "
                    f"(day {day} period {period}) — 같은 시간에 빈 {required} 없음"
                )
        lessons_out.append(
            {
                "course_id": course["id"],
                "class_group_id": course["class_group_id"],
                "subject_id": course["subject_id"],
                "teacher_ids": list(course["teacher_ids"]),
                "slot": {"day": day, "period": period},
                "room_id": room_id,
            }
        )

    # local search 는 최적성/비가능성을 증명하지 못한다: hard 0 이면 FEASIBLE,
    # 아니면 UNKNOWN (배정은 그대로 내려 주고 notes 로 설명).
    feasible = score.hard == 0
    if not feasible:
        notes.append(f"hard 위반이 남았다: {score}")
    return {
        "status": "FEASIBLE" if feasible else "UNKNOWN",
        "lessons": lessons_out,
        # 엔진 soft 점수의 부호 반전(페널티 합 근사). canonical objective 와
        # 단위가 다르다 — notes 의 soft 목적식 항목 참고.
        "objective_value": -score.soft,
        "best_objective_bound": None,
        "wall_time_seconds": wall_time_seconds,
        "penalties": {},
        "notes": notes,
    }
