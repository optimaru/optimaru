"""Round-trip check of the pydantic mirror serialization."""

import pytest

import conference_scheduling as cs
from conference_scheduling.models import ConferenceScheduleModel, SolveConfig


def test_roundtrip_preserves_score_semantics():
    schedule = cs.demo_conference()
    # A partially assigned state must survive as well.
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


def _dump_with(field, value):
    data = ConferenceScheduleModel.from_core(cs.demo_conference()).model_dump()
    data["talks"][0][field] = value
    return data


@pytest.mark.parametrize(
    "field, value, message",
    [
        ("talk_type", -1, "talk_type -1 is out of range"),
        ("talk_type", 2, "talk_type 2 is out of range"),
        ("talk_type", 99999999, "talk_type 99999999 is out of range"),
        ("speakers", [-1], "speaker -1 is out of range"),
        ("speakers", [99999999], "speaker 99999999 is out of range"),
        ("slot_room", -2, "slot_room -2 is out of range"),
        ("slot_room", 99999999, "slot_room 99999999 is out of range"),
    ],
)
def test_an_index_out_of_range_is_refused_before_the_engine(field, value, message):
    with pytest.raises(ValueError, match=rf"talk 'S01': {message}"):
        ConferenceScheduleModel.model_validate(_dump_with(field, value))


def test_the_last_slot_room_is_accepted():
    schedule = cs.demo_conference()
    last = schedule.slot_room_count() - 1
    assert ConferenceScheduleModel.model_validate(_dump_with("slot_room", last)).to_core().talks[0].slot_room == last


def test_an_index_edited_after_validation_is_refused_by_to_core():
    model = ConferenceScheduleModel.from_core(cs.demo_conference())
    model.talks[0].speakers = [99999999]
    with pytest.raises(ValueError, match="out of range"):
        model.to_core()


def test_an_index_into_an_empty_list_says_the_list_is_empty():
    data = ConferenceScheduleModel.from_core(cs.demo_conference()).model_dump()
    data["talk_types"] = []
    with pytest.raises(ValueError, match=r"talk_type \d+ is out of range \(the list is empty\)"):
        ConferenceScheduleModel.model_validate(data)
