"""Type stubs for `flight_crew._core` — the flight-crew-scheduling C++ solver.

The compiled module wraps the C++ domain in `use-cases/flight-crew-scheduling/cpp`,
which imports the generic engine in `include/optimaru`.

Domain: a BASIC-variable problem picking a crew member (employee index) for every crew
seat of a flight (FlightAssignment). Airport/employee/flight references are all list
indices (the pydantic mirror `flight_crew.models` uses code/id string references).

Time unit: minutes from a reference epoch (Unix in the model). Dates are day indices
obtained as floor(minutes / 1440).

Score convention: HardSoftScore, at the default weights — the same weights as the Java
quickstart, which are the C++ components' defaults (constraint_info()):
  hard 1. required_skill_cost                -100 per assignment without the skill
  hard 2. flight_conflict_cost               -10 per overlapping assignment pair of one crew member
  hard 3. transfer_between_two_flights_cost  -1 per disconnected consecutive assignment pair
  hard 4. employee_unavailability_cost       -10 per assignment on an unavailable day
  soft 5. first_assignment_not_departing_from_home_cost  -1000
  soft 6. last_assignment_not_arriving_at_home_cost      -1000
"""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple

class Score:
    """HardSoft score. `hard` is the weighted violation sum (0 == feasible); `soft` is
    the objective. Lexicographic comparison (hard first). Less negative is better."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True if hard >= 0."""

class Airport:
    """Problem fact: an airport. Other objects reference it by index into this list."""
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
    """Problem fact (value range): a crew member."""
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
    """Index into the airports list."""
    skills: list[str]
    unavailable_days: set[int]
    """Set of day indices of unavailable days (floor(minutes / 1440))."""

class Flight:
    """Problem fact: a flight. Times are minutes from the reference epoch (UTC)."""
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
    """Planning entity: one crew seat of a flight. `employee` is the planning
    variable (-1 == unassigned), written by the solver."""
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
    """Index into the employees list; -1 == unassigned."""
    pinned: bool
    """If True, employee is fixed (it counts in the score but never moves)."""

class FlightCrewSchedule:
    """Planning solution. solve changes the flight_assignments' `employee` and
    `score` in place.

    Note: list attributes such as `flight_assignments` return copies, so use
    `set_assignment` to change assignments."""
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
        """Current employee index per seat (-1 == unassigned)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk-write the planning variable (length == number of seats)."""

def calculate_score(schedule: FlightCrewSchedule) -> Score:
    """Full recalculation (also stored in schedule.score)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = automatic: clamp(4 * seats, 400, 3200)."""
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
    """Stop early at or above this score. (0, -inf) stops at the first feasible."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: FlightCrewSchedule) -> None:
        """Solves in place: fills the employee assignments and score. Pinned
        seats are kept."""

    def solve_with_constraints(self, schedule: FlightCrewSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see flight_crew.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    required_skill_cost, flight_conflict_cost, transfer_between_two_flights_cost,
    employee_unavailability_cost, first_assignment_not_departing_from_home_cost,
    last_assignment_not_arriving_at_home_cost. The same names flight_crew.constraints uses."""

def constraint_costs(schedule: FlightCrewSchedule) -> dict[str, dict[str, Any]]:
    """Raw per-employee cost of every default constraint, before weight/exponent/level:
    {name: {"costs": [...], "weight": float, "exponent": float, "hard": bool,
    "global": bool, "level": "hard" | "medium" | "soft", "reward": bool,
    "level_flippable": bool, "name": str, "label": str}}. flight_crew.constraints.score()
    turns it into a score. (A domain with a medium level adds "medium_fold" to every
    entry; a name held more than once is keyed "<name>#<k>" from its second instance.)"""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order:
    required_skill_cost (hard, 100), flight_conflict_cost (hard, 10),
    transfer_between_two_flights_cost (hard, 1), employee_unavailability_cost (hard, 10),
    first_assignment_not_departing_from_home_cost (soft, 1000),
    last_assignment_not_arriving_at_home_cost (soft, 1000); all flippable, none a reward,
    all scored by default (default_enabled)."""
