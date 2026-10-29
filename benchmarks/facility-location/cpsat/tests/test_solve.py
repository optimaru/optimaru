from itertools import product

import pytest

import facility_location as domain
from facility_location.constraints import DistanceFromFacility, get_constraints, score
from facility_location.models import (
    ConsumerModel,
    FacilityLocationProblemModel,
    FacilityModel,
    LocationModel,
)
from facility_location_cpsat import solve
from facility_location import calculate_score


def test_prefers_capacity_feasibility_before_distance() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=1,
                capacity=1,
            ),
            FacilityModel(
                id="far",
                location=LocationModel(latitude=1, longitude=1),
                setup_cost=1,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
            ConsumerModel(
                id="b", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert {consumer.facility for consumer in solved.consumers} == {0, 1}
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def test_uses_one_facility_when_setup_cost_dominates() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="a",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=10,
            ),
            FacilityModel(
                id="b",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
            ConsumerModel(
                id="b", location=LocationModel(latitude=0, longitude=0), demand=1
            ),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert len({consumer.facility for consumer in solved.consumers}) == 1


def test_explicit_constraint_list_replaces_defaults() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=0,
            ),
            FacilityModel(
                id="far",
                location=LocationModel(latitude=1, longitude=1),
                setup_cost=1,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            )
        ],
    )

    solved = solve(
        problem, time_limit_seconds=2, constraints=[{"type": "distance_from_facility"}]
    )

    assert solved.consumers[0].facility == 0
    assert solved.score_hard == 0  # capacity was not selected


def test_accepts_the_same_domain_constraint_object_as_the_engine() -> None:
    problem = FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=100,
                capacity=0,
            ),
            FacilityModel(
                id="far",
                location=LocationModel(latitude=1, longitude=1),
                setup_cost=1,
                capacity=10,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=1
            )
        ],
    )

    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=[DistanceFromFacility(weight=10)],
    )

    assert solved.consumers[0].facility == 0
    assert solved.score_hard == 0


def _parity_problem() -> FacilityLocationProblemModel:
    return FacilityLocationProblemModel(
        facilities=[
            FacilityModel(
                id="small-near",
                location=LocationModel(latitude=0, longitude=0),
                setup_cost=9,
                capacity=3,
            ),
            FacilityModel(
                id="large-far",
                location=LocationModel(latitude=0.01, longitude=0),
                setup_cost=2,
                capacity=8,
            ),
        ],
        consumers=[
            ConsumerModel(
                id="a", location=LocationModel(latitude=0, longitude=0), demand=3
            ),
            ConsumerModel(
                id="b", location=LocationModel(latitude=0, longitude=0), demand=2
            ),
        ],
    )


def _brute_force_score(problem, constraint):
    best = None
    for assignment in product(
        range(len(problem.facilities)), repeat=len(problem.consumers)
    ):
        candidate = problem.model_copy(deep=True)
        for consumer, facility in zip(candidate.consumers, assignment, strict=True):
            consumer.facility = facility
        value = score(domain.constraint_costs(candidate.to_core()), [constraint])
        best = max(best, value) if best is not None else value
    return best


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_brute_force_independently(constraint) -> None:
    problem = _parity_problem()
    expected = _brute_force_score(problem, constraint)

    solved = solve(problem, time_limit_seconds=2, constraints=[constraint])

    assert (solved.score_hard, solved.score_soft) == pytest.approx(expected)
