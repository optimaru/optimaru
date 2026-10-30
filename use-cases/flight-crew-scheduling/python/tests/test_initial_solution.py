"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

from datetime import datetime, timedelta

import pytest

pytest.importorskip("pydantic")

import flight_crew as fc
from flight_crew.models import (
    AirportModel,
    EmployeeModel,
    FlightAssignmentModel,
    FlightCrewScheduleModel,
    FlightModel,
)

DAY0 = datetime(2026, 8, 10)


def _problem():
    """Four days of ICN->NRT->ICN round trips, a pilot and a flight attendant per flight."""
    employees = [
        EmployeeModel(id=f"P{i}", home_airport="ICN", skills=["Pilot"] if i < 2 else ["Flight attendant"])
        for i in range(4)
    ]
    flights = []
    for day in range(4):
        for number, (start, end, hour) in enumerate((("ICN", "NRT", 8), ("NRT", "ICN", 13))):
            departure = DAY0 + timedelta(days=day, hours=hour)
            flights.append(
                FlightModel(
                    flight_number=f"F{day}{number}",
                    departure_airport=start,
                    departure_utc_date_time=departure,
                    arrival_airport=end,
                    arrival_utc_date_time=departure + timedelta(hours=2),
                )
            )
    assignments = [
        FlightAssignmentModel(id=f"{flight.flight_number}-{skill}", flight=flight.flight_number, required_skill=skill)
        for flight in flights
        for skill in ("Pilot", "Flight attendant")
    ]
    return FlightCrewScheduleModel(
        airports=[AirportModel(code="ICN"), AirportModel(code="NRT")],
        employees=employees,
        flights=flights,
        flight_assignments=assignments,
    )


def _crew(model):
    return {assignment.id: assignment.employee for assignment in model.flight_assignments}


def _score(model):
    return (model.score_hard, model.score_soft)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return fc.solve(model, time_limit_seconds=0, random_seed=1)


def _non_greedy_seed(problem):
    """Every assignment one employee along from where construction puts it."""
    seed = _construct(problem)
    employee_ids = [employee.id for employee in problem.employees]
    for assignment in seed.flight_assignments:
        position = employee_ids.index(assignment.employee)
        assignment.employee = employee_ids[(position + 1) % len(employee_ids)]
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in fc.__all__


def test_complete_seed_skips_construction():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    result = _construct(fc.with_initial_solution(problem, seed))
    assert _crew(result) == _crew(seed)
    assert _crew(result) != _crew(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = _problem()
    solved = fc.solve(problem, time_limit_seconds=0.3, random_seed=1)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(fc.with_initial_solution(problem, solution))
    assert _crew(result) == _crew(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["flight_assignments"] = solution["flight_assignments"][:6]
    kept = {assignment["id"]: assignment["employee"] for assignment in solution["flight_assignments"]}

    seeded = fc.with_initial_solution(problem, solution)
    assert all(employee is None for assignment_id, employee in _crew(seeded).items() if assignment_id not in kept)

    result = _construct(seeded)
    assert all(_crew(result)[assignment_id] == employee for assignment_id, employee in kept.items())
    assert all(employee is not None for employee in _crew(result).values())


def test_assignments_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(_problem())
    last = solved.flight_assignments[-1]
    seeded = fc.with_initial_solution(solved, {"flight_assignments": [{"id": last.id, "employee": last.employee}]})
    assert _crew(seeded) == {a.id: last.employee if a.id == last.id else None for a in solved.flight_assignments}
    assert _score(seeded) == (None, None)


def test_unknown_assignment_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-seat"):
        fc.with_initial_solution(_problem(), {"flight_assignments": [{"id": "no-such-seat", "employee": None}]})


@pytest.mark.parametrize("employee", ["no-such-employee", 0, ["P0"], True])
def test_unknown_employee_raises_naming_the_assignment(employee):
    problem = _problem()
    assignment_id = problem.flight_assignments[0].id
    with pytest.raises(ValueError, match=assignment_id):
        fc.with_initial_solution(problem, {"flight_assignments": [{"id": assignment_id, "employee": employee}]})


def test_unknown_employee_on_a_model_raises():
    problem = _problem()
    seed = _construct(problem)
    seed.flight_assignments[2].employee = "no-such-employee"
    with pytest.raises(ValueError, match=seed.flight_assignments[2].id):
        fc.with_initial_solution(problem, seed)


def test_repeated_assignment_raises():
    problem = _problem()
    assignment_id = problem.flight_assignments[0].id
    with pytest.raises(ValueError, match=assignment_id):
        fc.with_initial_solution(problem, {"flight_assignments": [{"id": assignment_id}, {"id": assignment_id}]})


@pytest.mark.parametrize(
    "solution",
    [None, [], {}, {"flight_assignments": "x"}, {"flight_assignments": ["x"]}, {"flight_assignments": [{}]}],
)
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        fc.with_initial_solution(_problem(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(_problem())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = fc.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.flight_assignments[0] is not problem.flight_assignments[0]


def test_pinned_assignment_keeps_its_employee_whatever_the_solution_says():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    other = next(e.id for e in problem.employees if e.id != seed.flight_assignments[0].employee)
    problem.flight_assignments[0].employee = other
    problem.flight_assignments[0].pinned = True

    seeded = fc.with_initial_solution(problem, seed)
    assert seeded.flight_assignments[0].employee == other
    assert seeded.flight_assignments[0].pinned is True
    assert fc.solve(seeded, time_limit_seconds=0.2, random_seed=1).flight_assignments[0].employee == other

    garbage = {"flight_assignments": [{"id": problem.flight_assignments[0].id, "employee": "no-such-employee"}]}
    assert fc.with_initial_solution(problem, garbage).flight_assignments[0].employee == other


def test_pinned_flag_comes_from_the_problem():
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution["flight_assignments"][0]["pinned"] = True
    assert fc.with_initial_solution(problem, solution).flight_assignments[0].pinned is False


def test_unknown_pinned_employee_on_the_problem_raises():
    problem = _problem()
    problem.flight_assignments[0].employee = "no-such-employee"
    problem.flight_assignments[0].pinned = True
    with pytest.raises(ValueError, match=problem.flight_assignments[0].id):
        fc.with_initial_solution(problem, {"flight_assignments": []})


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = _problem()
    seeded = fc.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(fc.solve(seeded, time_limit_seconds=0.3, random_seed=1)) >= _score(start)
