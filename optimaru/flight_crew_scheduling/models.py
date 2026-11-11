"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Airport/Employee/Flight/FlightAssignment/FlightCrewSchedule and
SolverConfig 1:1, but references are identifiers instead of
indices (airport code, flight flight_number, employee id), times are
`datetime` (naive == UTC) and dates are `date`. The engine-side units are minute/day
indices from the Unix epoch::

    from optimaru.flight_crew_scheduling.models import FlightCrewScheduleModel

    model = FlightCrewScheduleModel.model_validate(problem_dict)
    schedule = model.to_core()                    # pydantic -> engine
    solved = FlightCrewScheduleModel.from_core(schedule)  # engine -> pydantic
"""

from datetime import date, datetime, timedelta, timezone

from pydantic import Field, model_validator

from .._models import SolveConfigBase, SolveRequest, StrictModel, entities_by_id, solution_dict, solve_model_request
from . import _core

__all__ = [
    "AirportModel",
    "EmployeeModel",
    "FlightAssignmentModel",
    "FlightCrewScheduleModel",
    "FlightCrewScheduleRequest",
    "FlightModel",
    "SolveConfig",
    "datetime_to_minutes",
    "minutes_to_datetime",
    "solve_request",
    "with_initial_solution",
]

_EPOCH_DATE = date(1970, 1, 1)
_EPOCH_DATETIME = datetime(1970, 1, 1)


def datetime_to_minutes(value: datetime) -> int:
    """naive (=UTC) or tz-aware datetime -> minutes from the epoch."""
    if value.tzinfo is not None:
        value = value.astimezone(timezone.utc).replace(tzinfo=None)
    return int((value - _EPOCH_DATETIME) // timedelta(minutes=1))


def minutes_to_datetime(minutes: int) -> datetime:
    """minutes from the epoch -> naive (=UTC) datetime."""
    return _EPOCH_DATETIME + timedelta(minutes=minutes)


def _date_to_day(value: date) -> int:
    return (value - _EPOCH_DATE).days


def _day_to_date(day: int) -> date:
    return _EPOCH_DATE + timedelta(days=day)


class AirportModel(StrictModel):
    """An airport. code (IATA) is the identifier."""

    code: str
    name: str = ""
    latitude: float = 0.0
    longitude: float = 0.0


class EmployeeModel(StrictModel):
    """A crew member (value range). home_airport references an airport code."""

    id: str
    name: str = ""
    home_airport: str
    skills: list[str] = Field(default_factory=list)
    unavailable_days: list[date] = Field(default_factory=list)


class FlightModel(StrictModel):
    """A flight. Times are UTC (a naive datetime is read as UTC)."""

    flight_number: str
    departure_airport: str
    departure_utc_date_time: datetime
    arrival_airport: str
    arrival_utc_date_time: datetime


class FlightAssignmentModel(StrictModel):
    """One crew seat of a flight. employee is the decision
    variable (None == unassigned; references a crew member id)."""

    id: str
    flight: str
    index_in_flight: int = 0
    required_skill: str = ""
    employee: str | None = None
    pinned: bool = False


class FlightCrewScheduleModel(StrictModel):
    """Solution mirror. score is a read-only result, kept as a separate field."""

    airports: list[AirportModel]
    employees: list[EmployeeModel]
    flights: list[FlightModel]
    flight_assignments: list[FlightAssignmentModel]
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def _unique_identifiers(self):
        for label, ids in (
            ("airport code", [a.code for a in self.airports]),
            ("employee id", [e.id for e in self.employees]),
            ("flight number", [f.flight_number for f in self.flights]),
            ("assignment id", [a.id for a in self.flight_assignments]),
        ):
            if len(set(ids)) != len(ids):
                raise ValueError(f"duplicate {label}")
        return self

    def to_core(self) -> "_core.FlightCrewSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        airport_index = {a.code: i for i, a in enumerate(self.airports)}
        employee_index = {e.id: i for i, e in enumerate(self.employees)}
        flight_index = {f.flight_number: i for i, f in enumerate(self.flights)}

        def airport(code: str) -> int:
            if code not in airport_index:
                raise ValueError(f"unknown airport code: {code!r}")
            return airport_index[code]

        employees = [
            _core.Employee(
                e.id,
                e.name,
                airport(e.home_airport),
                list(e.skills),
                {_date_to_day(d) for d in e.unavailable_days},
            )
            for e in self.employees
        ]
        flights = [
            _core.Flight(
                f.flight_number,
                airport(f.departure_airport),
                datetime_to_minutes(f.departure_utc_date_time),
                airport(f.arrival_airport),
                datetime_to_minutes(f.arrival_utc_date_time),
            )
            for f in self.flights
        ]
        assignments = []
        for a in self.flight_assignments:
            if a.flight not in flight_index:
                raise ValueError(f"unknown flight number: {a.flight!r}")
            if a.employee is not None and a.employee not in employee_index:
                raise ValueError(f"unknown employee id: {a.employee!r}")
            assignments.append(
                _core.FlightAssignment(
                    a.id,
                    flight_index[a.flight],
                    a.index_in_flight,
                    a.required_skill,
                    -1 if a.employee is None else employee_index[a.employee],
                    a.pinned,
                )
            )
        return _core.FlightCrewSchedule(
            [_core.Airport(a.code, a.name, a.latitude, a.longitude) for a in self.airports],
            employees,
            flights,
            assignments,
        )

    @classmethod
    def from_core(cls, schedule: "_core.FlightCrewSchedule") -> "FlightCrewScheduleModel":
        """engine object -> pydantic (including the current assignment and score)."""
        return cls(
            airports=[
                AirportModel(code=a.code, name=a.name, latitude=a.latitude, longitude=a.longitude)
                for a in schedule.airports
            ],
            employees=[
                EmployeeModel(
                    id=e.id,
                    name=e.name,
                    home_airport=schedule.airports[e.home_airport].code,
                    skills=list(e.skills),
                    unavailable_days=sorted(_day_to_date(d) for d in e.unavailable_days),
                )
                for e in schedule.employees
            ],
            flights=[
                FlightModel(
                    flight_number=f.flight_number,
                    departure_airport=schedule.airports[f.departure_airport].code,
                    departure_utc_date_time=minutes_to_datetime(f.departure_utc),
                    arrival_airport=schedule.airports[f.arrival_airport].code,
                    arrival_utc_date_time=minutes_to_datetime(f.arrival_utc),
                )
                for f in schedule.flights
            ],
            flight_assignments=[
                FlightAssignmentModel(
                    id=a.id,
                    flight=schedule.flights[a.flight].flight_number,
                    index_in_flight=a.index_in_flight,
                    required_skill=a.required_skill,
                    employee=None if a.employee < 0 else schedule.employees[a.employee].id,
                    pinned=a.pinned,
                )
                for a in schedule.flight_assignments
            ],
            score_hard=schedule.score.hard,
            score_soft=schedule.score.soft,
        )


class SolveConfig(SolveConfigBase):
    """Serializable mirror of `_core.SolverConfig`.

    ``late_acceptance_size`` 0 is automatic: ``clamp(4 * seats, 400, 3200)``. A positive value
    is used as it is.
    """

    def to_core(self) -> "_core.SolverConfig":
        return self._apply_to(_core.SolverConfig())


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns: a `FlightCrewScheduleModel`, or a plain dict of
    that shape (the JSON the API returned). Assignments are matched by id. An
    assignment left out becomes unassigned and the construction heuristic places it,
    and only it, so a complete solution starts the search exactly from it. `problem`
    is not mutated. Pinned assignments keep their employee on `problem`, whatever
    `solution` says.

    Raises ValueError naming the assignment for an unknown or repeated id and for an
    unknown employee id (the engine does not range-check).
    """
    if not isinstance(problem, FlightCrewScheduleModel):
        raise TypeError(f"problem must be a FlightCrewScheduleModel: {type(problem)!r}")
    ids = [a.id for a in problem.flight_assignments]
    assignments = entities_by_id(solution_dict(solution), "flight_assignments", ids)
    employee_ids = {e.id for e in problem.employees}
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for assignment in seeded.flight_assignments:
        if not assignment.pinned:
            assignment.employee = assignments.get(assignment.id, {}).get("employee")
        employee = assignment.employee
        if employee is not None and (not isinstance(employee, str) or employee not in employee_ids):
            raise ValueError(f"flight assignment {assignment.id!r} has unknown employee {employee!r}")
    return seeded


class FlightCrewScheduleRequest(SolveRequest):
    """The flight crew schedule with the settings of one solve (see `SolveRequest` for those)."""

    model_input: FlightCrewScheduleModel


def solve_request(request: FlightCrewScheduleRequest, *, constraints=None) -> FlightCrewScheduleModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)
