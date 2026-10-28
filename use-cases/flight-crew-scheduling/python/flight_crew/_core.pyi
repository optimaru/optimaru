"""`flight_crew._core` 타입 스텁 — flight-crew-scheduling C++ 솔버.

컴파일된 모듈은 `use-cases/flight-crew-scheduling/cpp` 의 C++ 도메인 구현을
감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

도메인: 항공편의 승무원 좌석(FlightAssignment)마다 승무원(employee 인덱스)
을 고르는 BASIC 변수 문제. 공항/승무원/항공편 참조는 전부 리스트 인덱스다
(pydantic 미러 `flight_crew.models` 는 code/id 문자열 참조를 쓴다).

시간 단위: 기준 epoch(모델은 Unix)에서의 분(minute). 날짜는
floor(minutes / 1440) 로 얻는 day 인덱스.

점수 규약: HardSoftScore — Java quickstart 와 동일한 가중치.
  hard 1. requiredSkill               스킬 없는 배정마다 -100
  hard 2. flightConflict              같은 승무원의 겹치는 배정 쌍마다 -10
  hard 3. transferBetweenTwoFlights   끊긴 연속 배정 쌍마다 -1
  hard 4. employeeUnavailability      불가일 배정마다 -10
  soft 5. firstAssignmentNotDepartingFromHome  -1000
  soft 6. lastAssignmentNotArrivingAtHome      -1000
"""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    """HardSoft 점수. `hard` 는 위반 가중 합(0 == feasible); `soft` 는
    목적식. 사전식 비교(hard 우선). 덜 음수일수록 좋다."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """hard >= 0 이면 True."""

class Airport:
    """problem fact: 공항. 다른 객체는 이 리스트의 인덱스로 참조한다."""
    def __init__(
        self,
        code: str,
        name: str = "",
        latitude: float = 0.0,
        longitude: float = 0.0,
    ) -> None: ...
    code: str
    name: str
    latitude: float
    longitude: float

class Employee:
    """problem fact(value range): 승무원."""
    def __init__(
        self,
        id: str,
        name: str = "",
        home_airport: int = -1,
        skills: list[str] = ...,
        unavailable_days: set[int] = ...,
    ) -> None: ...
    id: str
    name: str
    home_airport: int
    """airports 리스트 인덱스."""
    skills: list[str]
    unavailable_days: set[int]
    """불가한 날의 day 인덱스 집합 (floor(minutes / 1440))."""

class Flight:
    """problem fact: 항공편. 시각은 기준 epoch 에서의 분(UTC)."""
    def __init__(
        self,
        flight_number: str,
        departure_airport: int,
        departure_utc: int,
        arrival_airport: int,
        arrival_utc: int,
    ) -> None: ...
    flight_number: str
    departure_airport: int
    departure_utc: int
    arrival_airport: int
    arrival_utc: int

class FlightAssignment:
    """planning entity: 항공편의 승무원 좌석 하나. `employee` 가 planning
    variable (-1 == 미배정); 솔버가 쓴다."""
    def __init__(
        self,
        id: str,
        flight: int,
        index_in_flight: int = 0,
        required_skill: str = "",
        employee: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: str
    flight: int
    index_in_flight: int
    required_skill: str
    employee: int
    """employees 리스트 인덱스; -1 == 미배정."""
    pinned: bool
    """True 면 employee 를 고정 (점수엔 반영되지만 절대 움직이지 않는다)."""

class FlightCrewSchedule:
    """planning solution. solve 는 flight_assignments 의 `employee` 와
    `score` 를 in place 로 바꾼다.

    주의: `flight_assignments` 등 리스트 속성 접근은 복사본을 돌려주므로,
    배정을 바꾸려면 `set_assignment` 를 써야 한다."""
    def __init__(
        self,
        airports: list[Airport],
        employees: list[Employee],
        flights: list[Flight],
        flight_assignments: list[FlightAssignment],
    ) -> None: ...
    airports: list[Airport]
    employees: list[Employee]
    flights: list[Flight]
    flight_assignments: list[FlightAssignment]
    @property
    def score(self) -> Score: ...
    @property
    def prepared(self) -> bool: ...
    def assignment(self) -> list[int]:
        """좌석별 현재 employee 인덱스 (-1 == 미배정)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """planning variable 일괄 쓰기 (길이 == 좌석 수)."""

def calculate_score(schedule: FlightCrewSchedule) -> Score:
    """전체 재계산 (schedule.score 에도 저장)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = 자동: clamp(4 * 좌석 수, 400, 3200)."""
    full_assert: bool
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    change_move_weight: float
    swap_move_weight: float
    best_score_limit: tuple[int, float]
    """이 점수 이상이면 조기 종료. (0, -1e18) 이면 첫 feasible 에서 멈춘다."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: FlightCrewSchedule) -> None:
        """in place 로 푼다: employee 배정과 score 를 채운다. 고정(pinned)
        좌석은 유지된다."""

    def solve_with_constraints(self, schedule: FlightCrewSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (flight_crew.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    flight_crew.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `flight_crew.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
