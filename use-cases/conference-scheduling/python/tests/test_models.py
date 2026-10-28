"""pydantic 미러 직렬화 round-trip 검증."""

import conference_scheduling as cs
from conference_scheduling.models import ConferenceScheduleModel, SolveConfig


def test_roundtrip_preserves_score_semantics():
    schedule = cs.demo_conference()
    # 일부만 배정된 상태도 그대로 살아남아야 한다.
    assignment = [-1] * schedule.talk_count()
    assignment[0] = schedule.slot_room_of(0, 4)
    assignment[2] = schedule.slot_room_of(1, 0)
    schedule.set_assignment(assignment)
    original = cs.calculate_score(schedule)

    model = ConferenceScheduleModel.from_core(schedule)
    clone = ConferenceScheduleModel.model_validate_json(model.model_dump_json())
    restored = clone.to_core()

    assert restored.talk_count() == schedule.talk_count()
    assert restored.assignment() == schedule.assignment()
    assert [t.code for t in restored.talks] == [t.code for t in schedule.talks]
    assert restored.talks[4].prerequisite_talks == [1]
    assert restored.talks[10].required_room_tags == ["Recorded"]
    assert restored.speakers[11].undesired_timeslot_tags == ["After lunch"]

    roundtrip = cs.calculate_score(restored)
    assert roundtrip.hard == original.hard
    assert roundtrip.soft == original.soft


def test_score_fields_serialized():
    schedule = cs.demo_conference()
    cs.calculate_score(schedule)
    model = ConferenceScheduleModel.from_core(schedule)
    assert model.score_hard == schedule.score.hard
    assert model.score_soft == schedule.score.soft


def test_solve_config_to_core():
    cfg = SolveConfig(
        time_limit_seconds=3.5,
        random_seed=9,
        parallel_solver_count=2,
        compatible_value_probability=0.5,
        best_score_limit=(0, -123.0),
    )
    core = cfg.to_core()
    assert abs(core.spent_limit_seconds - 3.5) < 1e-9
    assert core.random_seed == 9
    assert core.parallel_solver_count == 2
    assert abs(core.compatible_value_probability - 0.5) < 1e-9
    assert core.best_score_limit == (0, -123.0)
