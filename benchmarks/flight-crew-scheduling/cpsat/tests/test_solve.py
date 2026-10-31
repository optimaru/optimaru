from datetime import date, datetime, timezone

import pytest

import flight_crew as domain
from flight_crew.constraints import (
    FlightConflictCost,
    RequiredSkillCost,
    TransferBetweenTwoFlightsCost,
    get_constraints,
    score,
)

from flight_crew.models import (
    EmployeeModel,
    FlightAssignmentModel,
    FlightCrewScheduleModel,
    FlightModel,
)
from flight_crew_cpsat import solve
from flight_crew import calculate_score


def test_required_skills_drive_assignment() -> None:
    problem = FlightCrewScheduleModel(
        airports=[],
        employees=[
            EmployeeModel(id="pilot", home_airport="ICN", skills=["pilot"]),
            EmployeeModel(id="crew", home_airport="ICN", skills=["cabin"]),
        ],
        flights=[
            FlightModel(
                flight_number="F1",
                departure_airport="ICN",
                departure_utc_date_time=datetime(2026, 1, 1, 9, tzinfo=timezone.utc),
                arrival_airport="ICN",
                arrival_utc_date_time=datetime(2026, 1, 1, 10, tzinfo=timezone.utc),
            )
        ],
        flight_assignments=[
            FlightAssignmentModel(id="a", flight="F1", required_skill="pilot")
        ],
    )
    # Airport facts are required by the canonical conversion.
    from flight_crew.models import AirportModel

    problem.airports = [AirportModel(code="ICN")]
    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert solved.flight_assignments[0].employee == "pilot"
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> FlightCrewScheduleModel:
    from flight_crew.models import AirportModel

    return FlightCrewScheduleModel(
        airports=[AirportModel(code=code) for code in ("A", "B", "C")],
        employees=[
            EmployeeModel(
                id="e0",
                home_airport="A",
                skills=["pilot"],
                unavailable_days=[date(2026, 1, 1)],
            ),
            EmployeeModel(id="e1", home_airport="B", skills=["cabin"]),
        ],
        flights=[
            FlightModel(
                flight_number="F0",
                departure_airport="B",
                departure_utc_date_time=datetime(2026, 1, 1, 9),
                arrival_airport="C",
                arrival_utc_date_time=datetime(2026, 1, 1, 11),
            ),
            FlightModel(
                flight_number="F1",
                departure_airport="A",
                departure_utc_date_time=datetime(2026, 1, 1, 10),
                arrival_airport="B",
                arrival_utc_date_time=datetime(2026, 1, 1, 12),
            ),
            FlightModel(
                flight_number="F2",
                departure_airport="C",
                departure_utc_date_time=datetime(2026, 1, 1, 13),
                arrival_airport="B",
                arrival_utc_date_time=datetime(2026, 1, 1, 14),
            ),
        ],
        flight_assignments=[
            FlightAssignmentModel(id="a0", flight="F0", required_skill="cabin"),
            FlightAssignmentModel(id="a1", flight="F1", required_skill="pilot"),
            FlightAssignmentModel(id="a2", flight="F2", required_skill="pilot"),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_a_fixed_itinerary(constraint) -> None:
    problem = _parity_problem()
    fixed = [0, 0, 0]
    candidate = problem.model_copy(deep=True)
    for assignment, employee in zip(candidate.flight_assignments, fixed, strict=True):
        assignment.employee = candidate.employees[employee].id
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_employees=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    from flight_crew_cpsat import catalog

    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    assert [entry["name"] for entry in entries] == [
        "required_skill_cost",
        "flight_conflict_cost",
        "transfer_between_two_flights_cost",
        "employee_unavailability_cost",
        "first_assignment_not_departing_from_home_cost",
        "last_assignment_not_arriving_at_home_cost",
    ]
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every flight-crew constraint is flippable
        assert entry["params"] == {"weight": None, "hard": None, "exponent": 1.0}


def test_default_solve_scores_like_the_engine() -> None:
    """The default selection: CP-SAT's reported score is the engine's score of the solution."""
    problem = _parity_problem()
    solved = solve(problem, time_limit_seconds=5)
    engine = domain.calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    "demoted",
    [
        RequiredSkillCost(hard=False),
        FlightConflictCost(hard=False),
        TransferBetweenTwoFlightsCost(hard=False),
    ],
    ids=lambda item: item.name,
)
def test_demoting_a_hard_constraint_scores_like_the_engine(demoted) -> None:
    """hard=False lands on soft in both solvers: CP-SAT's reported score is the
    engine's score of the same solution under the same selection."""
    problem = _parity_problem()
    selection = [
        demoted if spec.name == demoted.name else spec for spec in get_constraints()
    ]
    proxy = {}
    solved = solve(
        problem,
        time_limit_seconds=5,
        constraints=selection,
        _fixed_employees=[0, 0, 0],
        _proxy_score=proxy,
    )
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert (proxy["hard"], proxy["soft"]) == pytest.approx(tuple(engine))
