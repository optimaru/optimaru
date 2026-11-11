"""Type stubs for `optimaru.pickup_delivery._core` — the pickup-and-delivery C++ solver.

Times are integer seconds from a common day start. Each vehicle's ordered
`node_indices` (its route) is the decision;
a stop in no route is unassigned and lands in `unassigned_node_indices`.

Score (hard / medium / soft, lexicographic), at the default weights: hard =
capacity peak + time-window lateness + the coupling rules (a request is
routed whole, on one vehicle, pickup before dropoff); medium = -(service
minutes of unrouted requests), folded into the engine's soft as
medium * 10^9; soft = -(total driving seconds). The weights are the C++
components' defaults (constraint_info()).
"""

import enum
from collections.abc import Iterable
from typing import Any, Callable, Literal, NamedTuple

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
        """True when hard >= 0: no hard constraint is violated."""

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
    """Derived: index of the serving vehicle; -1 == unassigned."""
    arrival_time: int
    """Derived: arrival seconds; -1 == unassigned."""

class Delivery:
    """One transport request: a pickup, a dropoff, and the load between them."""
    def __init__(
        self,
        id: str,
        pickup_node_index: int,
        dropoff_node_index: int,
        demand: int,
        order_time: int = 0,
        required_vehicle_index: int = -1,
        group_index: int = -1,
    ) -> None: ...
    id: str
    pickup_node_index: int
    dropoff_node_index: int
    demand: int
    """Added to the vehicle's load at the pickup, removed at the dropoff."""
    order_time: int
    """When the request was placed, in plan seconds; negative if it was
    already waiting when the plan starts."""
    required_vehicle_index: int
    """Index into PdpRoutePlan.vehicles of the vehicle this request must ride
    on; -1 == any."""
    group_index: int
    """The group this request travels in; -1 == none. The requests that share a
    group ride one vehicle together, or all wait (request_group_cost)."""

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
    pinned_node_indices: list[int]
    """`node_indices` is the decision: the ordered route (indices into nodes)."""

class PdpRoutePlan:
    """The problem and its routes. Constructing it assigns location indices and
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
    """Recompute the derived fields and the score (also stores plan.score).

    The score is the one solve(plan) stores: the seven core rules, or the default
    constraints when the plan has a group of two or more requests.
    """

def build_random_instance(request_count: int, vehicle_count: int, seed: int = 0) -> PdpRoutePlan:
    """A randomly generated PDPTW instance, for demos and benchmarks."""

class SolverConfig:
    def __init__(self) -> None: ...

    # --- termination ---
    spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    unimproved_spent_limit_seconds: float
    best_score_limit: tuple[int, int, int]
    """(hard, medium, soft): stop early once bestScore >= this. -(2**63) on
    medium or soft means no floor on the soft side (only hard is compared), so
    (0, -(2**63), -(2**63)) stops at the first feasible solution."""

    # --- engine ---
    random_seed: int
    parallel_solver_count: int
    """Independent parallel starts; the best final score wins."""
    full_assert: bool
    """Re-verify every incremental score against a full recompute after every
    move (a debugging aid; much slower)."""
    first_fit_decreasing: bool
    """Construction order: tightest window / largest demand first."""
    pair_construction_heuristic: bool
    """Cheapest feasible insertion of complete pickup/dropoff pairs."""

    # --- acceptance ---
    acceptor_type: AcceptorType
    late_acceptance_size: int
    """LATE_ACCEPTANCE: 0 = auto: clamp(4 * len(nodes), 200, 1600) up to 60 nodes, 5 above."""
    entity_tabu_size: int
    """ENTITY_TABU: for how many steps a moved entity stays tabu (7 by default; >= 0)."""
    simulated_annealing_starting_temperature: tuple[int, float]
    """SIMULATED_ANNEALING's (hard, soft) starting temperature, cooling to 0 over the solve.
    (0, 0), the default, = auto: hard 0, soft = simulated_annealing_temperature_fraction x
    |start soft| (at least 1); the soft level alone, without the medium level of the
    unassigned requests."""
    simulated_annealing_temperature_fraction: float
    """The auto temperature's fraction of the start's soft level, without the medium level of
    the unassigned requests (0.01 by default; > 0)."""
    great_deluge_water_level_step: tuple[int, float]
    """GREAT_DELUGE: how much the (hard, soft) water level, which starts at the start's score,
    rises per step. (0, 0), the default, leaves it there: only moves that keep the score at or
    above the start's are taken. A soft amount you give here, like an explicit starting
    temperature, is on the scale the search compares in, where one unit of the medium level
    (a penalty minute of an unassigned request) counts 10^9: only the automatic temperature
    leaves the medium level out."""
    step_counting_hill_climbing_size: int
    """STEP_COUNTING_HILL_CLIMBING: the bar is reset to the current score every this many
    steps (400 by default; > 0)."""
    record_to_record_start_gap: float
    """RECORD_TO_RECORD_TRAVEL: the gap below the best score that is still accepted at the
    start of the solve, as a fraction of the start's |soft| (0.02 by default; >= 0). The
    medium level is included here, at 10^9 per unit: a start that leaves work unassigned
    widens the gap."""
    record_to_record_end_gap: float
    """RECORD_TO_RECORD_TRAVEL: the same gap at the end of the solve (0.0 by default; >= 0);
    the gap shrinks from the start gap to this one."""

    # --- move mix (relative weights) ---
    relocate_move_weight: float
    list_swap_move_weight: float
    two_opt_move_weight: float
    or_opt_move_weight: float
    tail_swap_move_weight: float
    inter_exchange_move_weight: float
    inter_exchange_max_segment: int

    # --- pair operators ---
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

    # --- request-level moves ---
    request_ruin_recreate: bool
    """Take a few requests out of their routes and reinsert each, pickup and
    dropoff together, at its cheapest feasible slot pair (in random order or by
    `regret_k`, with `blink_rate`). Requests waiting unassigned join the
    reinsertion."""
    request_ruin_max_requests: int
    """The most requests one application takes out (1 = a pair relocate with the
    best slot). Never more than two fifths of the plan's requests; a request of a
    group brings its whole group, which can take the count past that."""
    bundle_moves: bool
    """Move requests whose pickups follow each other on a route to another route
    together, or swap two such bundles between nearby routes."""
    bundle_max_requests: int
    """The most requests in one bundle, a group counting as one."""

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
    """Called with (step, score) on each new best, in the levels of the final plan.score."""

class Solver:
    """Construction heuristic + local search + adaptive ruin & recreate, with
    request-level move and ruin operators. Solves IN PLACE: fills
    vehicle.node_indices, the unassigned pool, the derived fields and plan.score."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: PdpRoutePlan) -> None:
        """Solve on the seven core rules, or on the default constraints when two
        requests or more share a group (request_group_cost is not a core rule; that
        solve is a single start). The core rules do not include required_vehicle_cost,
        pinned_stops_cost or the vehicle limits either: for a plan that uses any of
        those, call solve_with_constraints."""
    def solve_with_constraints(self, plan: PdpRoutePlan, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.pickup_delivery.constraints). The given list replaces the default set;
        None keeps the default set. A name may be picked several times: each pick is
        its own instance with its own knobs, and the instances add up. plan.score and
        on_new_best report the three levels unfolded, so a soft total of 10^9 or more
        stays soft."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    the sixteen per-vehicle constraints (vehicle_capacity_cost ... required_vehicle_cost),
    then the five request-level globals (paired_request_cost, same_vehicle_cost,
    pickup_before_dropoff_cost, request_group_cost, unassigned_requests_cost).
    The same names optimaru.pickup_delivery.constraints uses."""

def constraint_costs(plan: PdpRoutePlan, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/offset/level
    rounding. A unit is a vehicle (plus the unassigned pool as the last unit); a global
    constraint reports one cost. `optimaru.pickup_delivery.constraints.score()` takes these and
    produces a score:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str, "units": [...],
    "medium_fold": {"kind": "into_soft", "unit": 1e9}}}. None = the default
    constraints; a pick list (as for solve_with_constraints) scores exactly those
    picks, each entry carrying its pick's knobs, the k-th pick of a name keyed
    "<name>#<k>" from the second on. "units" (the per-vehicle constraints) lists the
    vehicle indices the constraint evaluates, so the literal offset (clip=False)
    charges every vehicle, an idle one included, and not the pool;
    required_vehicle_cost and squared_order_age_cost price the pool too and carry no
    "units" (every unit counts). The medium fold belongs to the domain, so EVERY entry
    carries it."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the default level and
    weight of each constraint (unassigned_requests_cost is medium at weight 1 in
    medium units; pinned_stops_cost hard at 1e12; the coupling rules,
    required_vehicle_cost and request_group_cost hard at 1000). All flippable, none
    a reward;
    default_enabled is False for the opt-in weight-0 objectives
    (enclosing_circle_radius_cost, route_duration_cost, latency_cost,
    squared_order_age_cost)."""
