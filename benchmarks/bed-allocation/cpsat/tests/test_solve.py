from datetime import date

import pytest

import bed_allocation as domain
from bed_allocation.constraints import get_constraints
from bed_allocation.models import (
    BedModel,
    BedPlanModel,
    DepartmentModel,
    RoomModel,
    StayModel,
)
from bed_allocation_cpsat import solve
from bed_allocation import calculate_score


def test_overlapping_stays_use_distinct_beds() -> None:
    problem = BedPlanModel(
        departments=[DepartmentModel(id="d", specialty_to_priority={"cardio": 1})],
        rooms=[RoomModel(id="r", department_id="d", capacity=2)],
        beds=[BedModel(id="a", room_id="r"), BedModel(id="b", room_id="r")],
        stays=[
            StayModel(
                id="s1",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="s2",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert solved.score_medium == 0
    assert {stay.bed_id for stay in solved.stays} == {"a", "b"}
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_medium, solved.score_soft) == (
        shared.hard,
        shared.medium,
        shared.soft,
    )


def test_same_bed_constraint_can_be_removed() -> None:
    problem = BedPlanModel(
        departments=[DepartmentModel(id="d", specialty_to_priority={"cardio": 1})],
        rooms=[RoomModel(id="r", department_id="d", capacity=1)],
        beds=[BedModel(id="a", room_id="r")],
        stays=[
            StayModel(
                id="s1",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="s2",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
        ],
    )

    solved = solve(
        problem, time_limit_seconds=2, constraints=["assign_every_patient_to_a_bed"]
    )

    assert [stay.bed_id for stay in solved.stays] == ["a", "a"]
    assert solved.score_hard == 0


def _parity_problem() -> BedPlanModel:
    return BedPlanModel(
        departments=[
            DepartmentModel(
                id="d",
                specialty_to_priority={"cardio": 2},
                minimum_age=18,
                maximum_age=65,
            )
        ],
        rooms=[
            RoomModel(
                id="same",
                department_id="d",
                capacity=2,
                gender_limitation="SAME_GENDER",
                equipment=["telemetry"],
            ),
            RoomModel(
                id="male",
                department_id="d",
                capacity=1,
                gender_limitation="MALE_ONLY",
            ),
            RoomModel(
                id="female",
                department_id="d",
                capacity=1,
                gender_limitation="FEMALE_ONLY",
            ),
        ],
        beds=[
            BedModel(id="s0", room_id="same"),
            BedModel(id="s1", room_id="same"),
            BedModel(id="m", room_id="male"),
            BedModel(id="f", room_id="female"),
        ],
        stays=[
            StayModel(
                id="a",
                patient_gender="FEMALE",
                patient_age=10,
                patient_preferred_maximum_room_capacity=1,
                patient_required_equipment=["telemetry", "oxygen"],
                patient_preferred_equipment=["television"],
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="b",
                patient_gender="MALE",
                patient_age=80,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="missing",
            ),
            StayModel(
                id="c",
                patient_gender="FEMALE",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="d",
                patient_gender="MALE",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
            StayModel(
                id="e",
                patient_age=30,
                arrival_date=date(2026, 1, 1),
                departure_date=date(2026, 1, 2),
                specialty="cardio",
            ),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_a_fixed_plan(constraint) -> None:
    problem = _parity_problem()
    fixed = [0, 0, 2, 3, len(problem.beds)]
    candidate = problem.model_copy(deep=True)
    for stay, bed in zip(candidate.stays, fixed, strict=True):
        stay.bed_id = None if bed == len(problem.beds) else candidate.beds[bed].id
    core = candidate.to_core()
    core.set_assignment(fixed)
    expected = -sum(domain.constraint_costs(core)[constraint.name]["costs"])
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_beds=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)
