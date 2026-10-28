"""canonical `TimetableProblem` JSON(dict) <-> st::canon 도메인 변환 (풀-패리티).

API_SPEC.md 의 canonical 문제 JSON 을 캐노니컬 C++ 도메인(`_core.canon`)으로
옮긴다. st::canon 은 CP-SAT 레퍼런스 패키지(benchmarks/school-timetabling/cpsat 의
teacher_scheduling)의 하드 H1~H10 / 소프트 S1~S6 을 validate.py 의미론
그대로 구현하므로, 이 변환을 거치면 엔진 백엔드도 canonical 제약 전부를
그대로 반영한다 — quickstart 근사 변환은 `st_timetabling.quickstart` 로 옮겼다.

변환 규칙
---------
- 점유 단위(occupancy group): `parallel_group` 이 같은 강좌들은 하나의 병합
  그룹, 나머지는 강좌 하나가 그룹 하나. 그룹은 주당 시수만큼의 엔진 Lesson
  ("{키}#k")이 되고, 교사는 멤버 강좌 teacher_ids 의 이어붙임(중복 유지 —
  같은 교사가 두 분반을 동시에 맡는 모순을 하드로 드러낸다), 특별실 수요는
  멤버 합, 학급 점유는 1이다 (cpsat 의 Course.occupancy_key 와 동일). H5
  (분반 동시 진행)는 병합 자체로 성립한다.
- S5/S6(과목 기피/선호 교시)은 Lesson.preference_units 로 멤버 강좌마다
  1단위씩 계수되어 validate.py 의 per-lesson 카운트와 정확히 일치한다.
- `fixed_slots` -> pinned Lesson (H2: 엔진이 아예 움직이지 않는다).
- `forbidden_slots` + 학급/교사 `unavailable` -> per-lesson allowed_slot
  테이블 (H5 하드).
- `block_periods` >= 2 -> 그룹의 마지막 block_periods 개 Lesson 에 blockGroup
  부여 (H8: 같은 요일 연속 교시).
- `required_room_type` -> roomTypeDemand + roomCapacityByType (H6 하드).
  구체적인 방 배정은 cpsat 처럼 사후 그리디(convert_solution)로 정한다 —
  H6 이 하드라 hard 0 해에서는 항상 성공한다.
- 교사별 시수 상한(max_periods_per_week/day, max_consecutive_periods)과
  preferred_free_days, 강좌 일일 상한(max_periods_per_day)도 전부 전달된다.

입력 오류(없는 과목/학급/교사/특별실, 모순된 고정 시간 등)는 ValueError 로
올린다 — canonical pydantic 검증(HTTP 422)과 같은 계층의 실패다. 분반 그룹이
서로 다른 학급에 걸치는 입력만은 병합 표현이 불가능해 역시 ValueError 다
(분반의 정의상 한 학급을 나누는 것이므로 정상 입력에서는 나오지 않는다).
"""

from dataclasses import dataclass, field

from ._core import canon

__all__ = [
    "ConvertedProblem",
    "OccupancyGroup",
    "convert_problem",
    "convert_solution",
    "inject_solution",
]


@dataclass
class OccupancyGroup:
    """엔진 Lesson 들로 펼쳐진 점유 단위 하나 (분반 그룹 또는 단독 강좌)."""

    key: str  # parallel_group 이름 또는 강좌 id
    course_ids: list[str]  # 멤버 강좌 (분반이 아니면 1개)
    class_group_id: str
    periods_per_week: int
    block_periods: int  # 0/1 = 연강 없음
    fixed: list[tuple[int, int]]  # 정렬된 고정 (day, period); 앞쪽 Lesson 부터 pinned
    lesson_indices: list[int]  # 엔진 lesson index ("#1" 부터 순서대로)


@dataclass
class ConvertedProblem:
    """convert_problem 의 결과: 엔진 solution + 역변환에 필요한 메타데이터."""

    table: "canon.Timetable"
    timeslots: list[tuple[int, int]]  # slot index -> (day, period)
    slot_index: dict[tuple[int, int], int]  # (day, period) -> slot index
    groups: list[OccupancyGroup]
    courses_by_id: dict[str, dict]
    subjects_by_id: dict[str, dict]
    room_ids_by_type: dict[str, list[str]]  # room_type -> canonical room id 목록
    notes: list[str] = field(default_factory=list)


def _slot_key(slot: dict) -> tuple[int, int]:
    return (int(slot["day"]), int(slot["period"]))


def _period_mask(periods) -> int:
    """1-base 교시 목록 -> 16비트 마스크 (범위 밖 교시는 캘린더상 불가능하므로 무시)."""
    mask = 0
    for period in periods or ():
        if 1 <= int(period) <= 16:
            mask |= 1 << (int(period) - 1)
    return mask


def convert_problem(problem: dict) -> ConvertedProblem:
    """canonical TimetableProblem dict 를 st::canon Timetable 로 변환한다."""
    # --- timeslots: 요일별 교시 수 -> (day, period) 그리드 ---
    periods_per_day = {int(day): int(count) for day, count in problem["calendar"]["periods_per_day"].items()}
    if max(periods_per_day.values()) > 16 or max(periods_per_day) > 7:
        # st::canon 은 교시<=16, 요일<8 비트마스크 설계다 (prepare 가 거부한다).
        raise ValueError("캘린더가 엔진 한계를 넘는다 (교시 <= 16, 요일 index < 8)")
    timeslots = [(day, period) for day in sorted(periods_per_day) for period in range(1, periods_per_day[day] + 1)]
    slot_index = {slot: i for i, slot in enumerate(timeslots)}

    subjects_by_id = {s["id"]: s for s in problem["subjects"]}
    class_groups = list(problem["class_groups"])
    class_index = {cg["id"]: i for i, cg in enumerate(class_groups)}
    teachers = list(problem.get("teachers", []))
    teacher_index = {t["id"]: i for i, t in enumerate(teachers)}
    courses = list(problem["courses"])
    courses_by_id = {c["id"]: c for c in courses}

    def required_room_type(course: dict) -> str | None:
        subject = subjects_by_id.get(course["subject_id"])
        if subject is None:
            raise ValueError(f"[{course['id']}] 존재하지 않는 과목: {course['subject_id']}")
        return subject.get("required_room_type")

    # --- 특별실 타입 -> dense index, 보유 수 (H6) ---
    needed_types = sorted({rt for c in courses if (rt := required_room_type(c)) is not None})
    room_ids_by_type: dict[str, list[str]] = {}
    for room in problem.get("rooms", []):
        room_ids_by_type.setdefault(room["room_type"], []).append(room["id"])
    for room_type in needed_types:
        if not room_ids_by_type.get(room_type):
            raise ValueError(f"필요한 특별실이 없습니다: {room_type}")
    room_type_index = {room_type: i for i, room_type in enumerate(needed_types)}

    # --- 교사 배정 불가 시간 + 시수 상한 (H4 확장 / H9 / H10 / S3) ---
    teacher_blocked: list[set[int]] = []
    max_per_day: list[int] = []
    max_per_week: list[int] = []
    max_consecutive: list[int] = []
    free_day_mask: list[int] = []
    for teacher in teachers:
        teacher_blocked.append(
            {slot_index[key] for s in teacher.get("unavailable", []) if (key := _slot_key(s)) in slot_index}
        )
        max_per_day.append(int(teacher.get("max_periods_per_day") or 6))
        weekly = teacher.get("max_periods_per_week")
        max_per_week.append(-1 if weekly is None else int(weekly))
        max_consecutive.append(int(teacher.get("max_consecutive_periods") or 4))
        free_day_mask.append(sum(1 << int(d) for d in set(teacher.get("preferred_free_days", []))))

    class_blocked: list[set[int]] = [
        {slot_index[key] for s in cg.get("unavailable", []) if (key := _slot_key(s)) in slot_index}
        for cg in class_groups
    ]

    # --- 점유 단위: parallel_group 병합, 나머지는 강좌 1개 = 그룹 1개 ---
    members_by_key: dict[str, list[dict]] = {}
    for course in courses:
        if course["class_group_id"] not in class_index:
            raise ValueError(f"[{course['id']}] 존재하지 않는 학급: {course['class_group_id']}")
        for teacher_id in course["teacher_ids"]:
            if teacher_id not in teacher_index:
                raise ValueError(f"[{course['id']}] 존재하지 않는 교사: {teacher_id}")
        key = course.get("parallel_group") or course["id"]
        members_by_key.setdefault(key, []).append(course)

    lessons: list[canon.Lesson] = []
    groups: list[OccupancyGroup] = []
    daily_limits: list[int] = []
    block_group_count = 0
    for group_key, members in members_by_key.items():
        if len({c["class_group_id"] for c in members}) > 1:
            raise ValueError(f"분반 그룹 {group_key} 의 학급이 서로 다릅니다 (분반은 한 학급을 나눈다)")
        if len({int(c["periods_per_week"]) for c in members}) > 1:
            raise ValueError(f"분반 그룹 {group_key} 의 주당 시수가 서로 다릅니다")
        if len({int(c.get("block_periods", 0)) for c in members}) > 1:
            raise ValueError(f"분반 그룹 {group_key} 의 블록 시수가 서로 다릅니다")

        class_id = members[0]["class_group_id"]
        ppw = int(members[0]["periods_per_week"])
        block = int(members[0].get("block_periods", 0))

        # 교사: 멤버 teacher_ids 이어붙임 (중복 유지 — 위 docstring 참고).
        group_teachers = [teacher_index[t] for c in members for t in c["teacher_ids"]]

        # 특별실 수요: 멤버 합 (H6).
        demand = [0] * len(needed_types)
        for course in members:
            room_type = required_room_type(course)
            if room_type is not None:
                demand[room_type_index[room_type]] += 1

        # allowed_slot: 금지/불가 시간의 합집합을 걷어낸다 (H5). cpsat 의
        # _course_allowed_slots + 분반 교집합과 동일.
        blocked: set[int] = set(class_blocked[class_index[class_id]])
        for course in members:
            blocked.update(
                slot_index[key] for s in course.get("forbidden_slots", []) if (key := _slot_key(s)) in slot_index
            )
            for teacher_id in course["teacher_ids"]:
                blocked.update(teacher_blocked[teacher_index[teacher_id]])
        allowed = [i not in blocked for i in range(len(timeslots))]

        # fixed_slots: 멤버 합집합, (day, period) 정렬 -> 앞쪽 Lesson 부터 pinned.
        fixed = sorted({_slot_key(s) for c in members for s in c.get("fixed_slots", [])})
        for slot in fixed:
            if slot not in slot_index:
                raise ValueError(f"[{group_key}] 캘린더에 없는 fixed_slot: {slot}")
            if not allowed[slot_index[slot]]:
                # cpsat _add_fixed_slots 와 같은 실패: 고정 시간이 금지/불가와 모순.
                raise ValueError(f"[{group_key}] 고정 시간 {slot} 이 금지/불가 시간과 충돌합니다")
        if len(fixed) > ppw:
            raise ValueError(f"분반 그룹 {group_key} 의 고정 시간이 주당 시수보다 많습니다")

        # H7: 멤버별 유효 일일 상한(max(개별 상한, 블록 크기))의 최소값. 병합
        # 그룹은 하루 배치 수가 멤버 모두에게 동일하므로 최소값이 곧 전체 조건.
        daily_limits.append(min(max(int(c.get("max_periods_per_day", 1)), block) for c in members))

        # S5/S6: 멤버 과목마다 (기피, 선호) 마스크 1단위.
        units = []
        for course in members:
            subject = subjects_by_id[course["subject_id"]]
            units.append((_period_mask(subject.get("avoided_periods")), _period_mask(subject.get("preferred_periods"))))

        block_group = block_group_count if block >= 2 else -1
        if block >= 2:
            block_group_count += 1

        lesson_indices = []
        for k in range(1, ppw + 1):
            lesson = canon.Lesson(
                f"{group_key}#{k}",
                class_index[class_id],
                len(groups),
                group_teachers,
                room_type_demand=list(demand),
            )
            lesson.allowed_slot = allowed
            lesson.preference_units = units
            if k <= len(fixed):
                lesson.pinned = True
                lesson.slot = slot_index[fixed[k - 1]]
            if block_group >= 0 and k > ppw - block:
                lesson.block_group = block_group
            lesson_indices.append(len(lessons))
            lessons.append(lesson)

        groups.append(
            OccupancyGroup(
                key=group_key,
                course_ids=[c["id"] for c in members],
                class_group_id=class_id,
                periods_per_week=ppw,
                block_periods=block,
                fixed=fixed,
                lesson_indices=lesson_indices,
            )
        )

    table = canon.Timetable([day for day, _ in timeslots], [period for _, period in timeslots], lessons)
    table.teacher_count = len(teachers)
    table.class_count = len(class_groups)
    table.room_capacity_by_type = [len(room_ids_by_type[rt]) for rt in needed_types]
    table.teacher_max_per_day = max_per_day
    table.teacher_max_per_week = max_per_week
    table.teacher_max_consecutive = max_consecutive
    table.teacher_preferred_free_days = free_day_mask
    table.course_daily_limit = daily_limits

    return ConvertedProblem(
        table=table,
        timeslots=timeslots,
        slot_index=slot_index,
        groups=groups,
        courses_by_id=courses_by_id,
        subjects_by_id=subjects_by_id,
        room_ids_by_type=room_ids_by_type,
    )


def inject_solution(converted: ConvertedProblem, lessons: list[dict]) -> None:
    """외부 백엔드(cpsat 등)가 만든 canonical 배정을 엔진 lesson 슬롯에 주입한다.

    ``lessons`` 는 canonical `Timetable.lessons` 형태의 dict 목록(course_id +
    slot). 그룹의 슬롯 집합은 첫 멤버 강좌에서 읽는다 (유효한 해에서는 분반
    멤버 전원이 같은 슬롯이다 — H5). pinned Lesson 은 자기 고정 슬롯을,
    blockGroup Lesson 들은 한 요일의 연속 창(입력 해가 유효하면 H8 보장상
    반드시 존재)을 받고, 나머지는 (day, period) 정렬 순으로 배정된다. 패리티
    검증(엔진 breakdown vs cpsat validate)과 warm start 양쪽에 쓴다.
    """
    slot_index = converted.slot_index
    by_course: dict[str, list[int]] = {}
    for lesson in lessons:
        key = _slot_key(lesson["slot"])
        if key not in slot_index:
            raise ValueError(f"[{lesson['course_id']}] 캘린더에 없는 슬롯: {key}")
        by_course.setdefault(lesson["course_id"], []).append(slot_index[key])

    assignment = converted.table.assignment()  # pinned 슬롯이 이미 들어 있다
    for group in converted.groups:
        pool = sorted(by_course.get(group.course_ids[0], []))
        n = len(group.lesson_indices)
        placed: dict[int, int] = {}  # 그룹 내 위치 -> slot

        # 1) pinned (#1..#f): 자기 고정 슬롯 그대로.
        for k, slot in enumerate(group.fixed):
            placed[k] = slot_index[slot]
            if placed[k] in pool:
                pool.remove(placed[k])

        # 2) blockGroup (마지막 block_periods 개): 한 요일의 연속 창.
        block = group.block_periods if group.block_periods >= 2 else 0
        block_positions = [k for k in range(n) if block and k >= n - block]
        unpinned_block = [k for k in block_positions if k not in placed]
        if unpinned_block:
            pinned_slots = {placed[k] for k in block_positions if k in placed}
            window = _find_block_window(converted, pool, pinned_slots, block)
            if window is not None:
                free_window = sorted(s for s in window if s not in pinned_slots)
                for k, slot in zip(unpinned_block, free_window):
                    placed[k] = slot
                    pool.remove(slot)
            # 창을 못 찾으면(입력 해가 이미 H8 위반) 아래 잔여 배정으로 넘긴다.

        # 3) 나머지: 정렬 순. 슬롯이 모자라면(시수 미달 해) 미배정(-1)이 된다.
        rest = [k for k in range(n) if k not in placed]
        for k, slot in zip(rest, pool):
            placed[k] = slot
        for k in range(n):
            assignment[group.lesson_indices[k]] = placed.get(k, -1)

    converted.table.set_assignment(assignment)


def _find_block_window(
    converted: ConvertedProblem, pool: list[int], pinned_slots: set[int], size: int
) -> list[int] | None:
    """pool ∪ pinned_slots 안에서 pinned_slots 를 전부 포함하는, 한 요일의
    연속 교시 창(slot index 목록, 크기 size)을 찾는다."""
    candidates = set(pool) | pinned_slots
    by_day: dict[int, dict[int, int]] = {}
    for slot in candidates:
        day, period = converted.timeslots[slot]
        by_day.setdefault(day, {})[period] = slot
    for day in sorted(by_day):
        periods = by_day[day]
        for start in range(1, max(periods) - size + 2):
            if not all(p in periods for p in range(start, start + size)):
                continue
            window = [periods[p] for p in range(start, start + size)]
            if pinned_slots <= set(window):
                return window
    return None


def convert_solution(
    converted: ConvertedProblem,
    *,
    wall_time_seconds: float = 0.0,
    unassigned_weight: float | None = None,
) -> dict:
    """엔진 배정(`converted.table` 의 slot 들)을 canonical Timetable JSON(dict)
    으로 되돌린다. 병합 그룹은 멤버 강좌별 canonical lesson 으로 다시 풀리고
    teacher_ids 는 원본 강좌의 전체 튜플이 복원된다.

    특별실 배정: cpsat 의 rooms.assign_rooms 와 같은 사후 그리디 — (타입,
    시간)별로 같은 강좌가 쓰던 방을 우선 유지하고 나머지는 빈 방 순서대로.
    H6 이 하드 제약이라 hard 0 해에서는 항상 성공하며, hard 위반이 남은 해
    (UNKNOWN)에서만 room_id=null + notes 로 보고될 수 있다.

    penalties 는 엔진 breakdown 의 S1~S6 원시 카운트(validate.py
    compute_penalties 와 동일 의미), objective_value 는 table.weights 를 적용한
    가중 벌점 합이다 — cpsat 백엔드와 같은 단위.

    ``unassigned_weight`` 는 H1 을 soft 로 내린 조립(= 부분 편성)에서만 준다:
    미배정이 **탐색 실패가 아니라 결과**가 되므로 하드 합계에서 빠지고
    (status 는 나머지 하드로만 정해진다), 못 채운 시수는 cpsat 과 같은 이름
    (`weekly_period_counts`)으로 penalties 에 실리며 objective_value 에
    `unassigned_weight` 배로 더해진다 — cpsat 의 완화 모드와 같은 단위다."""
    table = converted.table
    breakdown = canon.compute_breakdown(table)
    partial = unassigned_weight is not None
    # 부분 편성에서는 H1 이 하드가 아니다: 미배정은 모델이 고른 결과라 status
    # 를 UNKNOWN 으로 만들면 안 된다 (cpsat 완화 모드는 OPTIMAL/FEASIBLE 이다).
    hard_total = breakdown.hard_total() - (breakdown.unassigned if partial else 0)
    notes: list[str] = list(converted.notes)
    assignment = table.assignment()

    lessons_out: list[dict] = []
    special: dict[tuple[str, int], list[int]] = {}  # (room_type, slot) -> 출력 인덱스
    for group in converted.groups:
        for lesson_index in group.lesson_indices:
            slot = assignment[lesson_index]
            if slot < 0:
                if not partial:
                    notes.append(f"{group.key}: 미배정 수업이 남았다 (탐색 실패)")
                continue
            day, period = converted.timeslots[slot]
            for course_id in group.course_ids:
                course = converted.courses_by_id[course_id]
                room_type = converted.subjects_by_id[course["subject_id"]].get("required_room_type")
                if room_type is not None:
                    special.setdefault((room_type, slot), []).append(len(lessons_out))
                lessons_out.append(
                    {
                        "course_id": course_id,
                        "class_group_id": course["class_group_id"],
                        "subject_id": course["subject_id"],
                        "teacher_ids": list(course["teacher_ids"]),
                        "slot": {"day": day, "period": period},
                        "room_id": None,
                    }
                )

    # 특별실 사후 배정 (cpsat rooms.assign_rooms 의 그리디와 동일한 규칙).
    preferred: dict[str, str] = {}  # 강좌가 마지막으로 쓴 방
    for (room_type, slot), indices in sorted(special.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        remaining = sorted(indices, key=lambda i: lessons_out[i]["course_id"])
        free = list(converted.room_ids_by_type[room_type])
        for index in list(remaining):  # 1차: 전에 쓰던 방이 비어 있으면 유지
            room_id = preferred.get(lessons_out[index]["course_id"])
            if room_id in free:
                free.remove(room_id)
                remaining.remove(index)
                lessons_out[index]["room_id"] = room_id
        for index in remaining:  # 2차: 빈 방 순서대로; H6 하드 위반 해에서만 모자란다
            if free:
                room_id = free.pop(0)
                preferred[lessons_out[index]["course_id"]] = room_id
                lessons_out[index]["room_id"] = room_id
            else:
                day, period = converted.timeslots[slot]
                notes.append(
                    f"{lessons_out[index]['course_id']}: 특별실({room_type}) 배정 실패 "
                    f"(day {day} period {period}) — hard 위반이 남은 해"
                )

    # local search 는 최적성/비가능성을 증명하지 못한다: hard 0 이면 FEASIBLE,
    # 아니면 UNKNOWN (배정은 그대로 내려 주고 notes 로 설명).
    feasible = hard_total == 0
    if not feasible:
        notes.append(f"hard 위반이 남았다: {hard_total}")
    penalties = {name: int(count) for name, count in breakdown.penalties().items()}
    objective = float(breakdown.soft_penalty(table.weights))
    if unassigned_weight is not None:
        # cpsat 완화 모드와 같은 이름·같은 단위: 못 채운 시수 자체가 벌점 항이다.
        penalties["weekly_period_counts"] = int(breakdown.unassigned)
        objective += unassigned_weight * breakdown.unassigned
        if breakdown.unassigned:
            notes.append(f"미배정 {breakdown.unassigned} 시수 (H1 완화 — 나머지는 배정한 부분해다)")
    return {
        "status": "FEASIBLE" if feasible else "UNKNOWN",
        "lessons": lessons_out,
        "objective_value": objective,
        "best_objective_bound": None,
        "wall_time_seconds": wall_time_seconds,
        "penalties": penalties,
        "notes": notes,
    }
