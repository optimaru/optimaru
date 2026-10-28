import pytest

import meeting_scheduling as domain
from meeting_scheduling.constraints import get_constraints
from meeting_scheduling.models import (
    MeetingAssignmentModel,
    MeetingModel,
    MeetingScheduleModel,
    PersonModel,
    RoomModel,
    TimeGrainModel,
)
from meeting_scheduling_cpsat import solve
from meeting_scheduling import calculate_score


def test_room_conflict_is_avoided() -> None:
    problem = MeetingScheduleModel(
        people=[],
        time_grains=[
            TimeGrainModel(
                id="g0", grain_index=0, day_of_year=1, starting_minute_of_day=540
            ),
            TimeGrainModel(
                id="g1", grain_index=1, day_of_year=1, starting_minute_of_day=570
            ),
        ],
        rooms=[RoomModel(id="r", capacity=10)],
        meetings=[MeetingModel(id="m0"), MeetingModel(id="m1")],
        meeting_assignments=[
            MeetingAssignmentModel(id="a0", meeting=0),
            MeetingAssignmentModel(id="a1", meeting=1),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert {assignment.grain_room for assignment in solved.meeting_assignments} == {
        0,
        1,
    }
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> MeetingScheduleModel:
    return MeetingScheduleModel(
        people=[PersonModel(id=str(index)) for index in range(3)],
        time_grains=[
            TimeGrainModel(
                id=f"g{index}",
                grain_index=index,
                day_of_year=100 if index < 4 else 101,
                starting_minute_of_day=540 + 15 * (index % 4),
            )
            for index in range(8)
        ],
        rooms=[RoomModel(id="small", capacity=1), RoomModel(id="large", capacity=5)],
        meetings=[
            MeetingModel(
                id="m0",
                duration_in_grains=4,
                required_attendees=[0, 0],
                preferred_attendees=[0, 1],
            ),
            MeetingModel(
                id="m1",
                duration_in_grains=4,
                required_attendees=[0],
                preferred_attendees=[1, 1],
            ),
            MeetingModel(
                id="m2",
                duration_in_grains=2,
                required_attendees=[0],
                preferred_attendees=[1],
            ),
        ],
        meeting_assignments=[
            MeetingAssignmentModel(id="a0", meeting=0),
            MeetingAssignmentModel(id="a1", meeting=1),
            MeetingAssignmentModel(id="a2", meeting=2),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_meetings(constraint) -> None:
    problem = _parity_problem()
    fixed = [4, 4, 13]
    candidate = problem.model_copy(deep=True)
    for assignment, grain_room in zip(
        candidate.meeting_assignments, fixed, strict=True
    ):
        assignment.grain_room = grain_room
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_grain_rooms=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)
