"""Type stubs for `pdp_routing._core` — the pickup-and-delivery C++ solver.

Times are integer seconds from a common day start. The plan is the planning
solution: each vehicle's ordered `node_indices` is the planning list variable;
a stop in no route is unassigned and lands in `unassigned_node_indices`.

Score (HardMediumSoft, lexicographic): hard = capacity peak + time-window
lateness + the coupling rules (a request is routed whole, on one vehicle,
pickup before dropoff); medium = -(service minutes of unrouted requests);
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
    """Row/column into the driving-time matrix; set by PdpRoutePlan."""

class Node:
    """One stop: the pickup half or the dropoff half of a request."""
    def __init__(
        self,
        id: str,
        location_index: int,
        min_start_time: int,
        max_end_time: int,
        service_duration: int,
        delivery_index: int,
        pickup: bool,
    ) -> None: ...
    id: str
    location_index: int
    min_start_time: int
    max_end_time: int
    service_duration: int
    delivery_index: int
    """Index into PdpRoutePlan.deliveries — the request this stop belongs to."""
    pickup: bool
    """True for the pickup half, False for the dropoff half."""
    vehicle_index: int
    """Shadow: index of the serving vehicle; -1 == unassigned."""
    arrival_time: int
    """Shadow: arrival seconds; -1 == unassigned."""

class Delivery:
    """One transport request: a pickup, a dropoff, and the load between them."""
    def __init__(
        self,
        id: str,
        pickup_node_index: int,
        dropoff_node_index: int,
        demand: int,
    ) -> None: ...
    id: str
    pickup_node_index: int
    dropoff_node_index: int
    demand: int
    """Added to the vehicle's load at the pickup, removed at the dropoff."""

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
    """Maximum load carried at any one moment, not a route total."""
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
    node_indices: list[int]
    """The planning list variable: the ordered route (indices into nodes)."""

class PdpRoutePlan:
    """Planning solution. Constructing it assigns location indices and
    accepts an explicit matrix or precomputes the haversine fallback."""
    def __init__(
        self,
        name: str,
        locations: list[Location],
        vehicles: list[Vehicle],
        nodes: list[Node],
        deliveries: list[Delivery],
        driving_seconds: list[list[int]] | None = None,
    ) -> None: ...
    name: str
    locations: list[Location]
    vehicles: list[Vehicle]
    nodes: list[Node]
    deliveries: list[Delivery]
    unassigned_node_indices: list[int]
    """The unassigned pool. A request is dropped when BOTH its stops are here;
    exactly one here is a half-assigned request, which is hard-infeasible."""
    @property
    def score(self) -> Score: ...
    def driving_time(self, from_location: int, to_location: int) -> int:
        """Precomputed driving seconds between two location indices."""
    def routes(self) -> dict[str, list[str]]:
        """Vehicle id -> ordered stop ids (convenience view)."""
    def unassigned_delivery_ids(self) -> list[str]:
        """Ids of the requests left entirely unrouted."""
    def rebuild_unassigned_pool(self) -> None:
        """Recompute unassigned_node_indices from the current routes."""

def calculate_score(plan: PdpRoutePlan) -> Score:
    """Recompute shadow variables and the score (also stores plan.score)."""

def build_random_instance(
    request_count: int, vehicle_count: int, seed: int = 0
) -> PdpRoutePlan:
    """A randomly generated PDPTW instance, for demos and benchmarks."""

class SolverConfig:
    def __init__(self) -> None: ...

    # --- termination ---
    spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    unimproved_spent_limit_seconds: float
    best_score_limit: tuple[int, int, int]
    """(hard, medium, soft): stop early once bestScore >= this."""

    # --- engine ---
    random_seed: int
    parallel_solver_count: int
    """Independent parallel starts; the best final score wins."""
    first_fit_decreasing: bool
    """Construction order: tightest window / largest demand first."""
    pair_construction_heuristic: bool
    """Cheapest feasible insertion of complete pickup/dropoff pairs."""

    # --- acceptance ---
    acceptor_type: AcceptorType
    late_acceptance_size: int
    """0 = auto: clamp(4 * len(nodes), 200, 1600)."""
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

    # --- pair-aware operators (the PDP-specific extensions) ---
    pair_moves: bool
    pair_exchange_moves: bool
    pair_exchange_min_nodes: int
    """Minimum node count for pair exchange; 0 forces it at every size."""
    """Relocate a whole request, and repair requests whose pair is broken."""
    pair_repair_probability: float
    pair_assignment_probability: float
    """Share of pair moves that drop a request into the pool or pull one back
    out — how the medium level is reachable at all."""
    pair_ruin: bool
    """Ruin whole requests, never half of one."""

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
    route_elimination_weight: float
    route_elimination_max_size: int
    ruin_recreate_selection_type: OperatorSelectionType
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    """Four rewards, indexed NEW_BEST / BETTER / ACCEPTED / REJECTED."""
    ruin_recreate_selection_segment_length: int
    ruin_recreate_selection_alpha: float
    sisr_ruin: bool
    route_ruin: bool
    sisr_max_string_removals: int
    sisr_max_string_size: int
    regret_k: int
    """Repair strategy: >= 2 uses regret-k insertion, 0/1 plain greedy."""
    blink_rate: float

    # --- nearby selection ---
    driving_time_nearby: bool
    driving_time_nearby_size: int
    driving_time_nearby_probability: float

    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Construction heuristic + local search + adaptive ruin & recreate, with
    pair-aware move and ruin operators. Solves IN PLACE: fills
    vehicle.node_indices, the unassigned pool, the shadows and plan.score."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: PdpRoutePlan) -> None: ...
    def solve_with_constraints(
        self, plan: PdpRoutePlan, constraints: Iterable[Any] | None = ...
    ) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (pdp_routing.constraints 참고). 준 목록이 기본 세트를 대체하고,
        None 이면 기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    pdp_routing.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `pdp_routing.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
