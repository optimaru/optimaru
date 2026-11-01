from datetime import date, datetime, timezone
from itertools import product

import pytest

import flight_crew as domain
from flight_crew.constraints import (
    EmployeeUnavailabilityCost,
    FirstAssignmentNotDepartingFromHomeCost,
    FlightConflictCost,
    LastAssignmentNotArrivingAtHomeCost,
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
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }


def test_default_solve_scores_like_the_engine() -> None:
    """The default selection: CP-SAT's reported score is the engine's score of the solution."""
    problem = _parity_problem()
    solved = solve(problem, time_limit_seconds=5)
    engine = domain.calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    "demoted",
    [
        RequiredSkillCost(level="soft"),
        FlightConflictCost(level="soft"),
        TransferBetweenTwoFlightsCost(level="soft"),
    ],
    ids=lambda item: item.name,
)
def test_demoting_a_hard_constraint_scores_like_the_engine(demoted) -> None:
    """level="soft" lands on soft in both solvers: CP-SAT's reported score is the
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


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _three_employees() -> FlightCrewScheduleModel:
    """The parity problem plus an employee e2 (home C, both skills): 27 assignments."""
    problem = _parity_problem()
    problem.employees.append(
        EmployeeModel(id="e2", home_airport="C", skills=["pilot", "cabin"])
    )
    return problem


SELECTIONS = {
    "repeat": [
        RequiredSkillCost(),
        RequiredSkillCost(weight=3, offset=1),
        FlightConflictCost(),
    ],
    "repeat-literal": [
        EmployeeUnavailabilityCost(offset=1, clip=False),
        EmployeeUnavailabilityCost(weight=2, offset=-1),
        TransferBetweenTwoFlightsCost(),
        TransferBetweenTwoFlightsCost(weight=5, offset=1, clip=False),
    ],
    "hard-offsets": [
        FlightConflictCost(offset=1, clip=False),
        RequiredSkillCost(weight=2, offset=2, clip=False),
        FirstAssignmentNotDepartingFromHomeCost(offset=-1),
        LastAssignmentNotArrivingAtHomeCost(weight=3, offset=1, clip=False),
    ],
    "identical-twice": [
        RequiredSkillCost(),
        RequiredSkillCost(),
        LastAssignmentNotArrivingAtHomeCost(),
    ],
}


def _engine_of(problem, fixed, selection):
    candidate = problem.model_copy(deep=True)
    for assignment, employee in zip(candidate.flight_assignments, fixed, strict=True):
        assignment.employee = candidate.employees[employee].id
    core = candidate.to_core()
    return tuple(score(domain.constraint_costs(core, selection), selection))


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_assignment(selection) -> None:
    """Pin each of the 27 assignments: the CP-SAT objective's own levels are the engine's
    score under the same repeated / shifted picks, exactly; the reported score re-prices
    the default costs (each name once) to the same value."""
    problem = _three_employees()
    for fixed in product(range(3), repeat=3):
        proxy: dict[str, int] = {}
        solved = solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_employees=list(fixed),
            _proxy_score=proxy,
        )
        engine = _engine_of(problem, fixed, selection)
        assert (proxy["hard"], proxy["soft"]) == engine, fixed
        assert (solved.score_hard, solved.score_soft) == engine, fixed


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _three_employees()
    best = max(
        _engine_of(problem, fixed, selection) for fixed in product(range(3), repeat=3)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == best


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [RequiredSkillCost(), RequiredSkillCost(weight=3, offset=1, clip=False)]
    dicts = [
        {"name": "required_skill_cost"},
        {"name": "required_skill_cost", "weight": 3, "offset": 1, "clip": False},
    ]
    problem = _three_employees()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _three_employees(),
            time_limit_seconds=1,
            constraints=[RequiredSkillCost(offset=0.5)],
        )
