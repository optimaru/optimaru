"""특별실 사후 배정.

기본 CP-SAT 모델은 H6 으로 "(특별실 종류, 시간)별 수요 <= 보유 수"를 보장하고,
어느 수업이 몇 번 방을 쓰는지는 풀고 난 뒤 여기서 정한다. 같은 종류의 방은
서로 대체 가능하므로 시간별 그리디 배정이 항상 성공한다. 플러그인 조립에서
H6 를 뺀 경우에는 같은 방을 재사용해 위반 해도 직렬화할 수 있게 한다. 제약을
껐다는 이유로 후처리가 실패하면 선택형 제약 API가 실제로는 동작하지 않기 때문이다.

같은 강좌는 가능하면 매번 같은 방을 쓰도록, 이미 그 강좌가 썼던 방을
우선 배정한다(소프트한 선호일 뿐 실패 조건은 아니다).
"""

from collections import defaultdict

from teacher_scheduling.domain.entities import TimeSlot
from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson


def assign_rooms(problem: TimetableProblem, lessons: list[ScheduledLesson]) -> list[ScheduledLesson]:
    """특별실이 필요한 수업에 room_id 를 채운 새 수업 목록을 돌려준다."""
    # (특별실 종류, 시간)별로 배정 대상 수업의 인덱스를 모은다.
    demands: dict[tuple[str, TimeSlot], list[int]] = defaultdict(list)
    for index, lesson in enumerate(lessons):
        room_type = problem.required_room_type(problem.course_by_id[lesson.course_id])
        if room_type is not None:
            demands[room_type, lesson.slot].append(index)

    assigned = list(lessons)
    preferred: dict[str, str] = {}  # 강좌가 마지막으로 쓴 방. 다음에도 같은 방을 먼저 시도한다.
    for (room_type, _slot), indices in sorted(
        demands.items(), key=lambda kv: (kv[0][0], kv[0][1].day, kv[0][1].period)
    ):
        rooms = [room.id for room in problem.rooms_by_type[room_type]]
        remaining = sorted(indices, key=lambda i: assigned[i].course_id)
        free = list(rooms)
        # 1차: 전에 쓰던 방이 비어 있으면 그대로 유지.
        for index in list(remaining):
            room_id = preferred.get(assigned[index].course_id)
            if room_id in free:
                free.remove(room_id)
                remaining.remove(index)
                assigned[index] = assigned[index].model_copy(update={"room_id": room_id})
        # 2차: 남은 수업은 남은 방에 순서대로. H6 를 조립에서 뺐다면 free 가
        # 먼저 바닥날 수 있으므로 그 뒤에는 방 목록을 순환한다. 그러면 같은
        # 시간/방 충돌이 결과에 그대로 남고, 호출자가 끈 제약의 의미도 보존된다.
        choices = free + [rooms[i % len(rooms)] for i in range(max(0, len(remaining) - len(free)))] if rooms else []
        for index, room_id in zip(remaining, choices, strict=False):
            preferred[assigned[index].course_id] = room_id
            assigned[index] = assigned[index].model_copy(update={"room_id": room_id})

    return assigned
