"""Type stubs for `vrp_routing._core` — the vehicle-routing C++ solver.

Times are integer seconds from a common day start. The plan is the planning
solution: each vehicle's ordered `visit_indices` is the planning list
variable; a visit in no route is unassigned.

Score (HardMediumSoft, lexicographic): hard = capacity excess + time-window
lateness (0 = feasible), medium = -(service minutes of unassigned visits),
soft = -(total driving seconds).
"""

import enum
from collections.abc import Iterable
from typing import Any, Callable

class AcceptorType(enum.Enum):
    """Local-search acceptance criterion."""

    HILL_CLIMBING = ...
    LATE_ACCEPTANCE = ...
    ENTITY_TABU = ...
    SIMULATED_ANNEALING = ...
    GREAT_DELUGE = ...
    STEP_COUNTING_HILL_CLIMBING = ...
    RECORD_TO_RECORD_TRAVEL = ...

class OperatorSelectionType(enum.Enum):
    """Adaptive ruin-operator selection scheme."""

    ROULETTE_WHEEL = ...
    SEGMENTED_ROULETTE_WHEEL = ...
    ALPHA_UCB = ...
    RANDOM = ...

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def medium(self) -> int: ...
    @property
    def soft(self) -> int: ...
    def is_feasible(self) -> bool:
        """True iff hard == 0."""

class Location:
    def __init__(self, latitude: float, longitude: float) -> None: ...
    latitude: float
    longitude: float
    index: int
    """Row/column into the driving-time matrix; set by VehicleRoutePlan."""

class Visit:
    """Planning entity: one customer stop (demand, time window, service)."""
    def __init__(
        self,
        id: str,
        name: str,
        location_index: int,
        demand: int,
        min_start_time: int,
        max_end_time: int,
        service_duration: int,
        unassigned_penalty_minutes: int = -1,
    ) -> None: ...
    id: str
    name: str
    location_index: int
    demand: int
    min_start_time: int
    max_end_time: int
    service_duration: int
    unassigned_penalty_minutes: int
    def unassigned_penalty(self) -> int: ...
    vehicle_index: int
    """Shadow: index of the serving vehicle; -1 == unassigned."""
    arrival_time: int
    """Shadow: arrival seconds; -1 == unassigned."""

class Vehicle:
    def __init__(
        self,
        id: str,
        capacity: int,
        home_location_index: int,
        departure_time: int = 0,
        max_travel_time: int = 0,
        max_route_duration: int = 0,
        max_end_time: int = 0,
        max_stops: int = 0,
        min_load_ratio: float = 0.0,
        fixed_cost: float = 0.0,
        max_route_radius_meters: float = 0.0,
        return_to_depot: bool = False,
    ) -> None: ...
    id: str
    capacity: int
    home_location_index: int
    departure_time: int
    max_travel_time: int
    max_route_duration: int
    max_end_time: int
    max_stops: int
    min_load_ratio: float
    fixed_cost: float
    max_route_radius_meters: float
    return_to_depot: bool
    """Whether the route ends back home. False (the default) leaves the route
    open at its last stop: no closing leg is driven or timed."""
    visit_indices: list[int]
    """The planning list variable: the ordered route (indices into visits)."""

class VehicleRoutePlan:
    """Planning solution. Constructing it assigns location indices and
    accepts an explicit matrix or precomputes the haversine fallback."""
    def __init__(
        self,
        name: str,
        locations: list[Location],
        vehicles: list[Vehicle],
        visits: list[Visit],
        driving_seconds: list[list[int]] | None = None,
    ) -> None: ...
    name: str
    locations: list[Location]
    vehicles: list[Vehicle]
    visits: list[Visit]
    unassigned_visit_indices: list[int]
    """The unassigned pool: visits the solver chose not to route. Filled by
    solve(); penalized on the medium score level."""
    @property
    def score(self) -> Score: ...
    def driving_time(self, from_location: int, to_location: int) -> int:
        """Precomputed driving seconds between two location indices."""
    def routes(self) -> dict[str, list[str]]:
        """Vehicle id -> ordered visit ids (convenience view)."""
    def unassigned_visit_ids(self) -> list[str]:
        """Ids of the visits left unassigned (convenience view)."""
    def rebuild_unassigned_pool(self) -> None:
        """Recompute unassigned_visit_indices from the current routes."""

def calculate_score(plan: VehicleRoutePlan) -> Score:
    """Recompute shadow variables and the score (also stores plan.score)."""

class SolverConfig:
    def __init__(self) -> None: ...

    # --- termination ---
    spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    unimproved_spent_limit_seconds: float
    best_score_limit: tuple[int, int, int]
    """(hard, medium, soft): stop early once bestScore >= this. (0, 0, -2**62)
    stops at the first feasible, fully-assigned solution."""

    # --- engine ---
    random_seed: int
    parallel_solver_count: int
    """Independent parallel starts; the best final score wins."""
    first_fit_decreasing: bool
    """Construction heuristic order: tightest time window and largest demand
    first (True), or plain input order (False)."""

    # --- acceptance ---
    acceptor_type: AcceptorType
    late_acceptance_size: int
    """0 = auto: clamp(60000 // len(visits), 100, 400) — shorter for larger
    instances, keeping the acceptance horizon roughly constant in wall-time."""
    record_to_record_start_gap: float
    record_to_record_end_gap: float

    # --- move mix (relative weights) ---
    relocate_move_weight: float
    list_swap_move_weight: float
    two_opt_move_weight: float
    or_opt_move_weight: float
    tail_swap_move_weight: float
    inter_exchange_move_weight: float
    inter_exchange_max_segment: int

    # --- adaptive ruin & recreate (ALNS) ---
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_polish_steps: int
    ruin_recreate_as_move_weight: float
    ruin_recreate_as_move_max_ruin: int
    descent_weight: float
    descent_max_sweeps: int
    granular_descent: bool
    granular_descent_max_segment: int
    granular_descent_candidate_routes: int
    route_elimination_weight: float
    route_elimination_max_size: int
    ejection_chain_weight: float
    ejection_chain_max_steps: int
    ejection_chain_candidate_lists: int
    ejection_chain_max_evaluations: int
    iterated_local_search_weight: float
    iterated_local_search_min_ruin: int
    iterated_local_search_max_ruin: int
    iterated_local_search_max_segment: int
    iterated_local_search_max_evaluations: int
    ruin_recreate_selection_type: OperatorSelectionType
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    """Four rewards, indexed NEW_BEST / BETTER / ACCEPTED / REJECTED."""
    ruin_recreate_selection_segment_length: int
    ruin_recreate_selection_alpha: float
    sisr_ruin: bool
    """SISR substring removal (Christiaens & Vanden Berghe)."""
    route_ruin: bool
    """Whole-route removal — the only ruin that can free up a vehicle."""
    sisr_max_string_removals: int
    sisr_max_string_size: int
    regret_k: int
    """Repair strategy: >= 2 uses regret-k insertion, 0/1 plain greedy."""
    blink_rate: float
    """SISR blinking: probability of skipping a candidate placement."""

    # --- nearby selection ---
    driving_time_nearby: bool
    driving_time_nearby_size: int
    driving_time_nearby_probability: float

    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Construction heuristic + local search + adaptive ruin & recreate
    (ALNS). Solves IN PLACE: fills vehicle.visit_indices, the unassigned
    pool, the shadow variables and plan.score."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: VehicleRoutePlan) -> None: ...
    def solve_with_constraints(
        self, plan: VehicleRoutePlan, constraints: Iterable[Any] | None = ...
    ) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see vrp_routing.constraints). The given list replaces the default set;
        None keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names vrp_routing.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `vrp_routing.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
