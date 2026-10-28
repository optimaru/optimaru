"""데모 solve: Java DemoDataGenerator 재현 인스턴스가 feasible 에 도달하고,
고정(pinned) 배정이 유지되는지 검증."""

import meeting_scheduling as ms
from meeting_scheduling.models import MeetingScheduleModel, demo_data


def test_demo_solve_reaches_feasible():
    model = demo_data()
    result = ms.solve(model, time_limit_seconds=10.0, random_seed=42, stop_at_first_feasible=True)
    assert isinstance(result, MeetingScheduleModel)
    assert result.score_hard == 0  # feasible
    assert all(a.grain_room is not None for a in result.meeting_assignments)
    # 배정 값이 인코딩 범위 안이어야 한다: [0, 160 * 3).
    assert all(0 <= a.grain_room < 160 * 3 for a in result.meeting_assignments)
    assert result.score_soft <= 0


def test_solve_core_in_place():
    core = ms.demo_data()
    result = ms.solve(core, time_limit_seconds=5.0, random_seed=1, stop_at_first_feasible=True)
    assert result is core
    assert core.score.is_feasible()
    assert all(v >= 0 for v in core.assignment())


def test_pinned_assignment_kept():
    core = ms.demo_data()
    pinned_value = core.grain_room_of(0, 2)  # 첫 그레인, Room 3
    assignments = core.meeting_assignments  # 복사본
    assignments[0].grain_room = pinned_value
    assignments[0].pinned = True
    core.meeting_assignments = assignments  # 리스트 통째로 되써야 반영된다
    ms.solve(core, time_limit_seconds=5.0, random_seed=7, stop_at_first_feasible=True)
    assert core.meeting_assignments[0].grain_room == pinned_value
    assert core.meeting_assignments[0].pinned is True
    assert core.score.hard == 0


def test_stop_at_first_feasible_and_callback():
    events = []
    result = ms.solve(
        demo_data(),
        time_limit_seconds=10.0,
        random_seed=3,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert result.score_hard == 0
    assert events, "on_new_best 콜백이 한 번도 불리지 않았다"
