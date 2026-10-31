"""Serialization round-trip of the pydantic mirror and demo_data reproduction."""

import pytest

import meeting_scheduling as ms
from meeting_scheduling.models import MeetingScheduleModel, demo_data


def test_demo_data_matches_java_generator_shape():
    model = demo_data()
    assert len(model.people) == 20
    assert len(model.time_grains) == 160  # 4 days x 40 (08:00-17:45, 15 min)
    assert [r.capacity for r in model.rooms] == [30, 20, 16]
    assert len(model.meetings) == 24
    assert len(model.meeting_assignments) == 24
    durations = sorted(m.duration_in_grains for m in model.meetings)
    assert durations == [8] * 10 + [12] * 7 + [16] * 7
    # Grain indices are consecutive and 1-based like the Java generator.
    assert [g.grain_index for g in model.time_grains] == list(range(1, 161))
    # Required/preferred attendees are within the people index range, no duplicates within a meeting.
    for meeting in model.meetings:
        assert len(set(meeting.required_attendees)) == len(meeting.required_attendees)
        assert len(set(meeting.preferred_attendees)) == len(meeting.preferred_attendees)
        assert not set(meeting.required_attendees) & set(meeting.preferred_attendees)
        assert all(0 <= p < 20 for p in meeting.required_attendees + meeting.preferred_attendees)


def test_json_round_trip():
    model = demo_data()
    clone = MeetingScheduleModel.model_validate_json(model.model_dump_json())
    assert clone == model
    core = clone.to_core()
    assert core.time_grain_count() == 160
    assert core.grain_room_count() == 480
    back = MeetingScheduleModel.from_core(core)
    # score is (0, 0) before scoring, so compare only assignments/facts.
    assert back.meetings == model.meetings
    assert back.meeting_assignments == model.meeting_assignments
    assert back.people == model.people


def test_round_trip_preserves_solution_and_score():
    core = ms.demo_data()
    ms.solve(core, time_limit_seconds=3.0, random_seed=11, stop_at_first_feasible=True)
    model = MeetingScheduleModel.from_core(core)
    assert model.score_hard == core.score.hard
    assert model.score_soft == core.score.soft
    clone = MeetingScheduleModel.model_validate_json(model.model_dump_json())
    rescored = ms.calculate_score(clone.to_core())
    assert rescored.hard == core.score.hard
    assert rescored.soft == core.score.soft


def _dump_with_grain_room(value):
    data = demo_data().model_dump()
    data["meeting_assignments"][0]["grain_room"] = value
    return data


@pytest.mark.parametrize("value", [-1, 480, 99999999])
def test_a_grain_room_out_of_range_is_refused_before_the_engine(value):
    with pytest.raises(ValueError, match=rf"grain_room {value} is out of range 0\.\.479"):
        MeetingScheduleModel.model_validate(_dump_with_grain_room(value))


@pytest.mark.parametrize("value", [None, 0, 479])
def test_every_grain_room_in_range_is_accepted(value):
    model = MeetingScheduleModel.model_validate(_dump_with_grain_room(value))
    assert model.to_core().meeting_assignments[0].grain_room == (-1 if value is None else value)


def test_a_grain_room_edited_after_validation_is_refused_by_to_core():
    model = demo_data()
    model.meeting_assignments[0].grain_room = 99999999
    with pytest.raises(ValueError, match="out of range"):
        model.to_core()


def test_a_grain_room_with_no_rooms_says_the_list_is_empty():
    data = _dump_with_grain_room(0)
    data["rooms"] = []
    with pytest.raises(ValueError, match=r"grain_room 0 is out of range \(the list is empty\)"):
        MeetingScheduleModel.model_validate(data)
