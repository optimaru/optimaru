"""Scaled-down demo solve: reaches feasible + high-level solve() + pinned seats kept."""

from datetime import date, datetime, timedelta

import pytest

pytest.importorskip("pydantic")

import flight_crew as fc
from flight_crew.models import (
    AirportModel,
    EmployeeModel,
    FlightAssignmentModel,
    FlightCrewScheduleModel,
    FlightModel,
    SolveConfig,
)

DAY0 = datetime(2026, 8, 10)


def _mini_problem() -> FlightCrewScheduleModel:
    """Two days x (ICN->NRT, NRT->ICN) round trips, one pilot + one flight attendant per flight.

    With 2 ICN-based pilots + 2 flight attendants each taking one day's round trip,
    a perfect 0hard/0soft solution exists.
    """
    airports = [AirportModel(code="ICN"), AirportModel(code="NRT")]
    employees = [
        EmployeeModel(id="P1", name="Pilot 1", home_airport="ICN", skills=["Pilot"]),
        EmployeeModel(id="P2", name="Pilot 2", home_airport="ICN", skills=["Pilot"]),
        EmployeeModel(id="C1", name="Cabin 1", home_airport="ICN", skills=["Flight attendant"]),
        EmployeeModel(id="C2", name="Cabin 2", home_airport="ICN", skills=["Flight attendant"]),
    ]
    flights = []
    assignments = []
    for day in range(2):
        out = DAY0 + timedelta(days=day, hours=8)
        back = DAY0 + timedelta(days=day, hours=13)
        flights.append(
            FlightModel(
                flight_number=f"OUT{day}",
                departure_airport="ICN",
                departure_utc_date_time=out,
                arrival_airport="NRT",
                arrival_utc_date_time=out + timedelta(hours=2),
            )
        )
        flights.append(
            FlightModel(
                flight_number=f"BACK{day}",
                departure_airport="NRT",
                departure_utc_date_time=back,
                arrival_airport="ICN",
                arrival_utc_date_time=back + timedelta(hours=2),
            )
        )
    for i, flight in enumerate(flights):
        assignments.append(
            FlightAssignmentModel(
                id=f"A{2 * i + 1}", flight=flight.flight_number, index_in_flight=1, required_skill="Pilot"
            )
        )
        assignments.append(
            FlightAssignmentModel(
                id=f"A{2 * i + 2}",
                flight=flight.flight_number,
                index_in_flight=2,
                required_skill="Flight attendant",
            )
        )
    return FlightCrewScheduleModel(
        airports=airports, employees=employees, flights=flights, flight_assignments=assignments
    )


def test_solve_mini_feasible():
    result = fc.solve(_mini_problem(), time_limit_seconds=3.0, random_seed=42, stop_at_first_feasible=False)
    assert result.score_hard == 0
    assert result.score_soft == 0  # perfect: everyone departs from and arrives at home
    assert all(a.employee is not None for a in result.flight_assignments)
    # Skill matching: only pilots in pilot seats.
    employee_by_id = {e.id: e for e in result.employees}
    for a in result.flight_assignments:
        assert a.required_skill in employee_by_id[a.employee].skills


def test_solve_dict_input_and_config():
    problem = _mini_problem().model_dump(mode="json")
    result = fc.solve(
        problem,
        config=SolveConfig(time_limit_seconds=3.0, random_seed=7, stop_at_first_feasible=True),
    )
    assert isinstance(result, FlightCrewScheduleModel)
    assert result.score_hard == 0


def test_solve_core_in_place():
    schedule = _mini_problem().to_core()
    returned = fc.solve(schedule, time_limit_seconds=2.0, random_seed=1, stop_at_first_feasible=True)
    assert returned is schedule
    assert schedule.score.hard == 0
    assert all(e >= 0 for e in schedule.assignment())


def test_pinned_assignment_survives_solve():
    model = _mini_problem()
    # Pin C2 to OUT0's cabin seat (A2): an assignment different from the optimum (one day's round trip each).
    model.flight_assignments[1].employee = "C2"
    model.flight_assignments[1].pinned = True
    result = fc.solve(model, time_limit_seconds=2.0, random_seed=3)
    pinned = next(a for a in result.flight_assignments if a.id == "A2")
    assert pinned.employee == "C2"
    assert pinned.pinned is True
    assert result.score_hard == 0


def test_unavailability_affects_score():
    model = _mini_problem()
    # Making everyone unavailable on the first of the two days forces the first day's round trip (4 seats) onto unavailable days.
    for employee in model.employees:
        employee.unavailable_days = [date(2026, 8, 10)]
    result = fc.solve(model, time_limit_seconds=2.0, random_seed=5)
    assert result.score_hard == -40  # 4 seats x -10
