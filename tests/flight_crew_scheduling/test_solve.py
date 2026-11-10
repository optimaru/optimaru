"""Scaled-down demo solve: reaches feasible + high-level solve() + pinned seats kept."""

import time
from datetime import date, datetime, timedelta

import pytest

pytest.importorskip("pydantic")

import optimaru.flight_crew_scheduling as fc
from optimaru.flight_crew_scheduling.models import (
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


def _boom(_step, _score):
    raise RuntimeError("boom")


def test_a_raising_on_new_best_raises_from_a_multi_start_solve():
    """Two parallel starts: the callback's exception reaches Python instead of aborting the process,
    and the other start stops at once instead of running out its 10 s budget."""
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        fc.solve(
            _mini_problem(),
            time_limit_seconds=10.0,
            parallel_solver_count=2,
            on_new_best=_boom,
        )
    assert time.monotonic() - started < 8.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    from optimaru.flight_crew_scheduling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            fc.solve(_mini_problem(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_has_no_soft_floor(monkeypatch):
    """stop_at_first_feasible hands the engine best_score_limit (0, -inf): only the hard level
    is compared, so no soft score, however low, misses the stop. A finite limit such as
    (0, -1e18) would never be reached by a soft score below -1e18. A soft score that low cannot
    be built here (its soft terms are 0 at the first feasible schedule of the test instance), so
    the limit itself is checked; the stop is exercised end to end
    in the other packages' tests."""
    import optimaru.flight_crew_scheduling as package

    seen = []

    class Captured(Exception):
        pass

    class Capture:
        def __init__(self, cfg):
            seen.append(cfg.best_score_limit)
            raise Captured

    monkeypatch.setattr(package, "Solver", Capture)
    with pytest.raises(Captured):
        package.solve(_mini_problem(), stop_at_first_feasible=True)
    assert seen == [(0, float("-inf"))]
