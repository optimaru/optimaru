"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Airport/Employee/Flight/FlightAssignment/FlightCrewSchedule 과
SolverConfig 를 1:1 로 미러링하되, 참조는 인덱스 대신 Java quickstart 처럼
식별자(공항 code, 항공편 flight_number, 승무원 id)로 표현하고, 시각은
`datetime`(naive == UTC), 날짜는 `date` 로 표현한다. 엔진 쪽 단위는 Unix
epoch 기준 분(minute)/일(day) 인덱스다::

    from flight_crew.models import FlightCrewScheduleModel

    model = FlightCrewScheduleModel.model_validate(problem_dict)
    schedule = model.to_core()                    # pydantic -> 엔진
    solved = FlightCrewScheduleModel.from_core(schedule)  # 엔진 -> pydantic

`pydantic` 은 선택 의존성이다 (`pip install flight_crew[models]`).
"""

from datetime import date, datetime, timedelta, timezone

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "AirportModel",
    "EmployeeModel",
    "FlightAssignmentModel",
    "FlightCrewScheduleModel",
    "FlightModel",
    "SolveConfig",
    "datetime_to_minutes",
    "minutes_to_datetime",
]

_EPOCH_DATE = date(1970, 1, 1)
_EPOCH_DATETIME = datetime(1970, 1, 1)


def datetime_to_minutes(value: datetime) -> int:
    """naive(=UTC) 또는 tz-aware datetime -> epoch 기준 분."""
    if value.tzinfo is not None:
        value = value.astimezone(timezone.utc).replace(tzinfo=None)
    return int((value - _EPOCH_DATETIME) // timedelta(minutes=1))


def minutes_to_datetime(minutes: int) -> datetime:
    """epoch 기준 분 -> naive(=UTC) datetime."""
    return _EPOCH_DATETIME + timedelta(minutes=minutes)


def _date_to_day(value: date) -> int:
    return (value - _EPOCH_DATE).days


def _day_to_date(day: int) -> date:
    return _EPOCH_DATE + timedelta(days=day)


class AirportModel(BaseModel):
    """공항. code(IATA)가 식별자다."""

    model_config = ConfigDict(extra="forbid")

    code: str
    name: str = ""
    latitude: float = 0.0
    longitude: float = 0.0


class EmployeeModel(BaseModel):
    """승무원(value range). home_airport 는 공항 code 참조."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str = ""
    home_airport: str
    skills: list[str] = Field(default_factory=list)
    unavailable_days: list[date] = Field(default_factory=list)


class FlightModel(BaseModel):
    """항공편. 시각은 UTC (naive datetime == UTC 로 해석)."""

    model_config = ConfigDict(extra="forbid")

    flight_number: str
    departure_airport: str
    departure_utc_date_time: datetime
    arrival_airport: str
    arrival_utc_date_time: datetime


class FlightAssignmentModel(BaseModel):
    """planning entity: 항공편의 승무원 좌석 하나. employee 가 planning
    variable (None == 미배정, 승무원 id 참조)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    flight: str
    index_in_flight: int = 0
    required_skill: str = ""
    employee: str | None = None
    pinned: bool = False


class FlightCrewScheduleModel(BaseModel):
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

    model_config = ConfigDict(extra="forbid")

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
        """pydantic -> 엔진 객체 (score 필드는 엔진이 다시 계산하므로 무시)."""
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
        """엔진 객체 -> pydantic (현재 배정과 score 포함)."""
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


class SolveConfig(BaseModel):
    """`_core.SolverConfig` 의 직렬화 가능한 미러."""

    model_config = ConfigDict(extra="forbid")

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    # 0 = 자동 (clamp(4 * 좌석 수, 400, 3200)). 양수를 주면 그 값을 쓴다.
    late_acceptance_size: int = Field(default=0, ge=0)
    stop_at_first_feasible: bool = False
    best_score_limit: tuple[int, float] | None = None
    unimproved_spent_limit_seconds: float = Field(default=0.0, ge=0)
    step_count_limit: int = Field(default=0, ge=0)
    unimproved_step_count_limit: int = Field(default=0, ge=0)
    full_assert: bool = False

    ruin_recreate: bool = True
    ruin_recreate_unimproved_step_trigger: int = Field(default=5000, ge=0)
    ruin_recreate_min_ruin_fraction: float = Field(default=0.10, ge=0)
    ruin_recreate_max_ruin_fraction: float = Field(default=0.30, ge=0)
    ruin_recreate_max_ruin_count: int = Field(default=60, ge=1)
    ruin_recreate_operator_decay: float = Field(default=0.8, ge=0, le=1)
    ruin_recreate_rewards: list[float] = Field(
        default_factory=lambda: [33.0, 9.0, 3.0, 0.0],
        min_length=4,
        max_length=4,
    )
    ruin_recreate_polish_steps: int = Field(default=5000, ge=0)

    change_move_weight: float = Field(default=0.75, ge=0)
    swap_move_weight: float = Field(default=0.25, ge=0)

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.ruin_recreate_min_ruin_fraction > self.ruin_recreate_max_ruin_fraction:
            raise ValueError("ruin_recreate_min_ruin_fraction must be <= ruin_recreate_max_ruin_fraction")
        return self

    def to_core(self) -> "_core.SolverConfig":
        cfg = _core.SolverConfig()
        cfg.spent_limit_seconds = self.time_limit_seconds
        cfg.random_seed = self.random_seed
        cfg.parallel_solver_count = self.parallel_solver_count
        cfg.late_acceptance_size = self.late_acceptance_size  # 0 = 자동
        cfg.unimproved_spent_limit_seconds = self.unimproved_spent_limit_seconds
        cfg.step_count_limit = self.step_count_limit
        cfg.unimproved_step_count_limit = self.unimproved_step_count_limit
        cfg.full_assert = self.full_assert
        cfg.ruin_recreate = self.ruin_recreate
        cfg.ruin_recreate_unimproved_step_trigger = self.ruin_recreate_unimproved_step_trigger
        cfg.ruin_recreate_min_ruin_fraction = self.ruin_recreate_min_ruin_fraction
        cfg.ruin_recreate_max_ruin_fraction = self.ruin_recreate_max_ruin_fraction
        cfg.ruin_recreate_max_ruin_count = self.ruin_recreate_max_ruin_count
        cfg.ruin_recreate_operator_decay = self.ruin_recreate_operator_decay
        cfg.ruin_recreate_rewards = list(self.ruin_recreate_rewards)
        cfg.ruin_recreate_polish_steps = self.ruin_recreate_polish_steps
        cfg.change_move_weight = self.change_move_weight
        cfg.swap_move_weight = self.swap_move_weight
        if self.stop_at_first_feasible:
            cfg.best_score_limit = (0, -1e18)
        if self.best_score_limit is not None:
            cfg.best_score_limit = self.best_score_limit
        return cfg
