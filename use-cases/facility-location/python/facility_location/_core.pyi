"""Type stubs for `facility_location._core` — the facility-location C++
solver.

The problem is the planning solution: each consumer's `facility` (an index
into `problem.facilities`, -1 == unassigned) is the single BASIC planning
variable.

Score (HardSoftScore, lexicographic): hard = -(over-capacity demand summed
over used facilities), soft = -(setup_cost_weight * used facilities' setup
cost + distance_from_facility_weight * consumer->facility meters).
"""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True iff hard >= 0."""

class Location:
    def __init__(self, latitude: float, longitude: float) -> None: ...
    latitude: float
    longitude: float
    def distance_to(self, other: Location) -> int:
        """Meters: ceil(Euclidean degree distance * 111 km) (Java
        Location.getDistanceTo)."""

class Facility:
    """Problem fact: a candidate facility with a setup cost and capacity."""

    def __init__(self, id: str, location: Location, setup_cost: int, capacity: int) -> None: ...
    id: str
    location: Location
    setup_cost: int
    capacity: int

class Consumer:
    """Planning entity: demand served by exactly one facility."""

    def __init__(self, id: str, location: Location, demand: int) -> None: ...
    id: str
    location: Location
    demand: int
    facility: int
    """The planning variable: index into problem.facilities; -1 == unassigned."""
    def is_assigned(self) -> bool: ...

class FacilityLocationProblem:
    """Planning solution. NOTE: `facilities` / `consumers` element access
    returns copies; use set_assignment for bulk facility writes."""

    def __init__(
        self,
        facilities: list[Facility],
        consumers: list[Consumer],
        south_west_corner: Location = ...,
        north_east_corner: Location = ...,
    ) -> None: ...
    facilities: list[Facility]
    consumers: list[Consumer]
    south_west_corner: Location
    north_east_corner: Location
    setup_cost_weight: int
    """Soft weight of the setup-cost constraint (Java provider: ofSoft(2))."""
    distance_from_facility_weight: int
    """Soft weight of the distance constraint. Default 6 = the quickstart
    solution's ConstraintWeightOverrides value (provider default 5)."""
    @property
    def score(self) -> Score: ...
    def facility_count(self) -> int: ...
    def consumer_count(self) -> int: ...
    def total_cost(self) -> int:
        """Setup cost of the used facilities (Java getTotalCost)."""
    def potential_cost(self) -> int:
        """Setup cost of every facility (Java getPotentialCost)."""
    def total_distance_meters(self) -> int:
        """Total consumer->facility distance in meters."""
    def used_facility_count(self) -> int: ...
    def assignment(self) -> list[int]:
        """The current facility index per consumer (-1 == unassigned)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the planning variable."""

def calculate_score(problem: FacilityLocationProblem) -> Score:
    """Full recompute of the HardSoftScore (also stores problem.score)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = auto: clamp(8 * consumer count, 400, 3200)."""
    full_assert: bool
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    ruin_recreate_as_move_weight: float
    """Destroy-and-repair as an ordinary acceptor-judged move (the engine's
    ruinRecreate.asMoveWeight); one such move costs ~ruinCount * facility
    count score evaluations. 0 = off."""
    ruin_recreate_as_move_max_ruin: int
    change_move_weight: float
    swap_move_weight: float
    nearby_probability: float
    """Bias swap partners (and the LNS related ruin) toward geographically
    close consumers with this probability. 0 = off (uniform-random)."""
    nearby_size: int
    value_recommender_probability: float
    """Probability that a change move draws the consumer's new facility from
    its value_recommender_keep nearest facilities instead of uniformly."""
    value_recommender_keep: int
    descent_weight: float
    """BASIC best-improvement descent strategy: probability a step runs a
    full nearby-pair descent (~2 * facility count evaluations per sweep)
    instead of one sampled move. 0 = off."""
    custom_move_probability: float
    """Gate on how often a step may draw CloseFacilityMove; 1 = unrestricted."""
    close_facility_moves: bool
    """Enable the facility-closing macro move: it empties one used facility in
    a single step, the only way past the setup-cost step function."""
    close_facility_keep: int
    """How far down a consumer's distance-ordered facility list the closing
    move looks for a new home."""
    close_facility_migrate_probability: float
    """Probability that an emptied facility's consumers migrate together to one
    alternative site instead of being redistributed over the open ones."""
    best_score_limit: tuple[int, float]
    """(hard, soft): stop early once bestScore >= this. (0, -1e18) stops at
    the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Construction heuristic + Late Acceptance local search (+ adaptive
    ruin-and-recreate). Solves IN PLACE: fills consumer.facility and
    problem.score."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: FacilityLocationProblem) -> None: ...
    def solve_with_constraints(self, problem: FacilityLocationProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see facility_location.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names facility_location.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Return the raw per-constraint cost breakdown."""
