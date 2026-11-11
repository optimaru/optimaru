"""Type stubs for `optimaru.vehicle_routing._core` — the vehicle-routing C++ solver.

Times are integer seconds from a common day start. Each vehicle's ordered
`visit_indices` (its route) is the decision; a visit in no route is
unassigned.

Score (hard / medium / soft, lexicographic), at the default weights: hard =
capacity excess + time-window lateness (0 = feasible), medium = -(penalty
minutes of unassigned visits), folded into the engine's soft as
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
    @property
    def index(self) -> int:
        """Row/column into the driving-time matrix; set by VehicleRoutePlan."""

class Visit:
    """One customer stop (demand, time window, service)."""
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
        group_index: int = -1,
        after_visit_indices: list[int] = ...,
    ) -> None: ...
    id: str
    name: str
    location_index: int
    demand: int
    min_start_time: int
    max_end_time: int
    service_duration: int
    unassigned_penalty_minutes: int
    group_index: int
    """The group this visit travels in; -1 == none. The visits that share a group
    ride one vehicle together, or all wait (visit_group_cost)."""
    after_visit_indices: list[int]
    """Indices into VehicleRoutePlan.visits of the visits of its group this one is
    served after on the route (visit_order_cost). Read the whole list and assign a
    whole list: appending to the value read changes a copy."""
    def unassigned_penalty(self) -> int: ...
    @property
    def vehicle_index(self) -> int:
        """Derived: index of the serving vehicle; -1 == unassigned."""
    @property
    def arrival_time(self) -> int:
        """Derived: arrival seconds; -1 == unassigned."""

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
    """The decision: the ordered route (indices into visits)."""

class VehicleRoutePlan:
    """The problem and its routes. Constructing it assigns location indices and
    accepts an explicit matrix or precomputes the haversine fallback."""
    def __init__(
        self,
        name: str,
        locations: list[Location],
        vehicles: list[Vehicle],
        visits: list[Visit],
        driving_seconds: list[list[int]] | None = None,
    ) -> None: ...
    @property
    def name(self) -> str: ...
    @property
    def locations(self) -> list[Location]: ...
    vehicles: list[Vehicle]
    @property
    def visits(self) -> list[Visit]: ...
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
    """Recompute the derived fields and the score (also stores plan.score).

    The score is the one solve(plan) stores: the four core rules, or the default
    constraints when the plan has a group of two or more visits.
    """

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
    """Construction heuristic order: tightest time window and largest demand
    first (True), or plain input order (False)."""

    # --- acceptance ---
    acceptor_type: AcceptorType
    late_acceptance_size: int
    """LATE_ACCEPTANCE: 0 = auto: clamp(60000 // len(visits), 100, 400) — shorter for
    larger instances, keeping the acceptance horizon roughly constant in wall-time."""
    entity_tabu_size: int
    """ENTITY_TABU: for how many steps a moved entity stays tabu (7 by default; >= 0)."""
    simulated_annealing_starting_temperature: tuple[int, float]
    """SIMULATED_ANNEALING's (hard, soft) starting temperature, cooling to 0 over the solve.
    (0, 0), the default, = auto: hard 0, soft = simulated_annealing_temperature_fraction x
    |start soft| (at least 1); the soft level alone, without the medium level of the
    unassigned visits."""
    simulated_annealing_temperature_fraction: float
    """The auto temperature's fraction of the start's soft level, without the medium level of
    the unassigned visits (0.01 by default; > 0)."""
    great_deluge_water_level_step: tuple[int, float]
    """GREAT_DELUGE: how much the (hard, soft) water level, which starts at the start's score,
    rises per step. (0, 0), the default, leaves it there: only moves that keep the score at or
    above the start's are taken. A soft amount you give here, like an explicit starting
    temperature, is on the scale the search compares in, where one unit of the medium level
    (a penalty minute of an unassigned visit) counts 10^9: only the automatic temperature
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
    group_ruin_recreate: bool
    """The group operator: takes a visit group out, alone or with a few more
    visits, and puts each unit back whole at its cheapest feasible place. Only
    registered for a plan with groups."""
    group_ruin_max_visits: int
    """The most visits the group operator takes out beside its first group (0 or
    more)."""
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
    """Progress callback (steps, best score). The score has the same three
    levels as the final plan.score: with solve_with_constraints both come
    from the chosen constraints' hard / medium / soft totals, so a soft total
    of 10^9 or more stays on soft instead of spilling into medium."""

class Solver:
    """Construction heuristic + local search + adaptive ruin & recreate
    (ALNS). Solves IN PLACE: fills vehicle.visit_indices, the unassigned
    pool, the derived fields and plan.score."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: VehicleRoutePlan) -> None:
        """Solve on the four core rules, which do not include the vehicle limits: for a
        plan that sets one, call solve_with_constraints. A plan with a group of two
        visits or more (Visit.group_index) is the exception: it is scored with the default
        constraints, visit_group_cost and visit_order_cost among them, every parallel
        start on a set of its own."""
    def solve_with_constraints(self, plan: VehicleRoutePlan, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.vehicle_routing.constraints). The given list replaces the default set;
        None keeps the default set. A name may be picked several times: each pick is
        its own instance with its own knobs, and the instances add up. With
        parallel_solver_count above 1 every start is scored by its own copy of the picks."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    the fourteen per-vehicle constraints (vehicle_capacity_cost ... vehicle_fixed_cost,
    visit_order_cost), then the two globals (visit_group_cost, unassigned_visits_cost).
    The same names optimaru.vehicle_routing.constraints uses."""

def constraint_costs(plan: VehicleRoutePlan, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/offset/level
    rounding. A unit is a vehicle (plus the unassigned pool as the last cell, always 0);
    a global constraint reports one cost. `optimaru.vehicle_routing.constraints.score()` takes these
    and produces a score:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str, "units": [...],
    "medium_fold": {"kind": "into_soft", "unit": 1e9}}}. None = the default
    constraints; a pick list (as for solve_with_constraints) scores exactly those
    picks, each entry carrying its pick's knobs, the k-th pick of a name keyed
    "<name>#<k>" from the second on. "units" (the per-vehicle constraints only) lists
    the vehicle indices the constraint evaluates, so the literal offset (clip=False)
    charges every vehicle, an idle one included, and not the pool. The medium fold
    belongs to the domain, so EVERY entry carries it."""

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
    weight of each constraint (unassigned_visits_cost is medium at weight 1 in
    medium units; the route policies hard at 1; visit_group_cost and
    visit_order_cost hard at 1000; travel_time_cost,
    min_capacity_utilization_cost and vehicle_fixed_cost soft at 1). All
    flippable, none a reward; default_enabled is False for the opt-in weight-0
    objectives (enclosing_circle_radius_cost, route_duration_cost, latency_cost)."""
