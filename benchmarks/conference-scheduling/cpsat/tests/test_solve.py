import pytest

import conference_scheduling as domain
from conference_scheduling.constraints import get_constraints, score
from conference_scheduling.models import (
    ConferenceScheduleModel,
    RoomModel,
    SpeakerModel,
    TalkModel,
    TimeslotModel,
)
from conference_scheduling_cpsat import catalog, solve
from conference_scheduling import calculate_score


def test_smoke_and_plugin_catalog():
    problem = ConferenceScheduleModel(
        name="Small",
        talk_types=["Talk"],
        timeslots=[
            TimeslotModel(id="S", start_minute=0, end_minute=60, talk_types=[0])
        ],
        rooms=[RoomModel(id="R", capacity=10, talk_types=[0])],
        talks=[TalkModel(code="T", talk_type=0)],
    )
    result = solve(
        problem, time_limit_seconds=2, constraints=["talk_type_compatibility"]
    )
    assert result.talks[0].slot_room == 0
    assert result.score_hard == 0
    assert result.score_soft == 0
    assert len(catalog()) == 37
    shared = calculate_score(result.to_core())
    assert (result.score_hard, result.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> ConferenceScheduleModel:
    return ConferenceScheduleModel(
        name="Parity",
        talk_types=["Talk", "Workshop"],
        timeslots=[
            TimeslotModel(
                id="S0",
                start_minute=0,
                end_minute=60,
                talk_types=[0],
                tags=["morning", "projector"],
            ),
            TimeslotModel(
                id="S1",
                start_minute=30,
                end_minute=90,
                talk_types=[0, 1],
                tags=["afternoon"],
            ),
            TimeslotModel(
                id="S2",
                start_minute=1440,
                end_minute=1500,
                talk_types=[0, 1],
                tags=["morning"],
            ),
        ],
        rooms=[
            RoomModel(
                id="R0",
                capacity=10,
                talk_types=[0],
                unavailable_timeslots=[1],
                tags=["small", "projector"],
            ),
            RoomModel(id="R1", capacity=100, talk_types=[0, 1], tags=["large"]),
        ],
        speakers=[
            SpeakerModel(
                id="P0",
                unavailable_timeslots=[0],
                required_timeslot_tags=["morning"],
                preferred_timeslot_tags=["morning"],
                prohibited_timeslot_tags=["afternoon"],
                undesired_timeslot_tags=["afternoon"],
                required_room_tags=["large"],
                preferred_room_tags=["large"],
                prohibited_room_tags=["small"],
                undesired_room_tags=["small"],
            ),
            SpeakerModel(id="P1"),
        ],
        talks=[
            TalkModel(
                code="A",
                talk_type=1,
                speakers=[0],
                theme_track_tags=["T"],
                sector_tags=["S"],
                audience_types=["dev"],
                audience_level=1,
                content_tags=["C"],
                language="ko",
                required_timeslot_tags=["morning"],
                preferred_timeslot_tags=["morning"],
                prohibited_timeslot_tags=["afternoon"],
                undesired_timeslot_tags=["afternoon"],
                required_room_tags=["large"],
                preferred_room_tags=["large"],
                prohibited_room_tags=["small"],
                undesired_room_tags=["small"],
                mutually_exclusive_talks_tags=["X"],
                prerequisite_talks=[1],
                favorite_count=20,
                crowd_control_risk=2,
            ),
            TalkModel(
                code="B",
                talk_type=0,
                speakers=[0, 1],
                theme_track_tags=["T"],
                sector_tags=["S"],
                audience_types=["dev"],
                audience_level=2,
                content_tags=["C"],
                mutually_exclusive_talks_tags=["X"],
                favorite_count=15,
                crowd_control_risk=1,
            ),
            TalkModel(
                code="C",
                talk_type=0,
                speakers=[1],
                theme_track_tags=["T"],
                sector_tags=["S"],
                audience_types=["ops"],
                audience_level=3,
                content_tags=["C"],
                language="en",
            ),
            TalkModel(code="D", talk_type=0, favorite_count=30),
        ],
        minimum_consecutive_talks_pause_in_minutes=45,
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_a_fixed_schedule(constraint) -> None:
    problem = _parity_problem()
    fixed = [0, 0, 3, 5]
    candidate = problem.model_copy(deep=True)
    for talk, slot_room in zip(candidate.talks, fixed, strict=True):
        talk.slot_room = slot_room
    expected = score(
        domain.constraint_costs(candidate.to_core()),
        [type(constraint)(weight=1)],
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_slot_rooms=fixed,
        _proxy_score=proxy,
    )

    assert (proxy["hard"], proxy["soft"]) == pytest.approx(expected)
