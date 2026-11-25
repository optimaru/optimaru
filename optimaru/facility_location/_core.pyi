"""Type stubs for `optimaru.facility_location._core` — the facility-location C++
solver.

The problem object holds the solution: each consumer's `facility` (an index
into `problem.facilities`, -1 == unassigned) is the single assignment-shape decision
variable.

Score (hard/soft, lexicographic), at the default weights: hard =
-(over-capacity demand summed over used facilities), soft = -(2 * used
facilities' setup cost + 6 * consumer->facility meters). The weights are the
C++ components' defaults (constraint_info()); the problem carries none.
"""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True when hard >= 0: no hard constraint is violated."""

class Location:
    def __init__(self, latitude: float, longitude: float) -> None: ...
    latitude: float
    longitude: float
    def distance_to(self, other: Location) -> int:
        """Meters: ceil(Euclidean degree distance * 111 km)."""

class Facility:
    """Problem fact: a candidate facility with a setup cost and capacity."""

    def __init__(self, id: str, location: Location, setup_cost: int, capacity: int) -> None: ...
    id: str
    location: Location
    setup_cost: int
    capacity: int

class Consumer:
    """Demand served by exactly one facility."""

    def __init__(self, id: str, location: Location, demand: int) -> None: ...
    id: str
    location: Location
    demand: int
    facility: int
    """The decision variable: index into problem.facilities; -1 == unassigned."""
    def is_assigned(self) -> bool: ...

class FacilityLocationProblem:
    """Problem and solution in one object. NOTE: `facilities` / `consumers` element access
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
    @property
    def score(self) -> Score: ...
    def facility_count(self) -> int: ...
    def consumer_count(self) -> int: ...
    def total_cost(self) -> int:
        """Setup cost of the used facilities."""
    def potential_cost(self) -> int:
        """Setup cost of every facility."""
    def total_distance_meters(self) -> int:
        """Total consumer->facility distance in meters."""
    def used_facility_count(self) -> int: ...
    def assignment(self) -> list[int]:
        """The current facility index per consumer (-1 == unassigned)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the decision variable."""

def calculate_score(problem: FacilityLocationProblem) -> Score:
    """Full recompute of the score (also stores problem.score)."""

class SolverConfig:
    """The solver settings, by stage of the solve: ``stop``, ``alns_trigger``, ``strategies``, ``custom_moves``, ``moves``, ``selection``, ``acceptance``, ``alns``, ``run``.

    Read and write a setting as ``config.<group>.<name>``, or give whole groups when making it:
    ``SolverConfig(stop={"time_limit_seconds": 10})``. See `optimaru._config`."""

    def __init__(
        self,
        *,
        stop: dict[str, object] | None = None,
        alns_trigger: dict[str, object] | None = None,
        strategies: dict[str, object] | None = None,
        custom_moves: dict[str, object] | None = None,
        moves: dict[str, object] | None = None,
        selection: dict[str, object] | None = None,
        acceptance: dict[str, object] | None = None,
        alns: dict[str, object] | None = None,
        run: dict[str, object] | None = None,
    ) -> None: ...
    GROUPS: tuple[str, ...]
    def to_dict(self) -> dict[str, dict[str, object]]:
        """Every setting by group and name: the shape of a request's ``solver_parameters``."""
        ...

    class Stop:
        """``config.stop``: When the search stops: the first limit reached."""

        time_limit_seconds: float
        unimproved_time_limit_seconds: float
        step_limit: int
        unimproved_step_limit: int
        target_score: tuple[int, float]
        """(hard, soft): stop early once bestScore >= this. (0, -inf) stops at
        the first feasible solution."""

    stop: Stop
    """When the search stops: the first limit reached."""

    class AlnsTrigger:
        """``config.alns_trigger``: When a stalled search is kicked by ruin & recreate."""

        enabled: bool
        unimproved_steps: int

    alns_trigger: AlnsTrigger
    """When a stalled search is kicked by ruin & recreate."""

    class Strategies:
        """``config.strategies``: A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

        ruin_recreate_weight: float
        """Destroy-and-repair as an ordinary acceptor-judged move (the engine's
        strategies.ruin_recreate_weight); one such move costs ~ruinCount * facility
        count score evaluations. 0 = off."""
        ruin_recreate_max_ruin: int
        descent_weight: float
        """Assignment-shape best-improvement descent strategy: probability a step runs a
        full nearby-pair descent (~2 * facility count evaluations per sweep)
        instead of one sampled move. 0 = off."""

    strategies: Strategies
    """A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        share: float
        """Gate on how often a step may draw one of the macro moves (CloseFacilityMove,
        SwapFacilityMove) instead of a change or a swap; 1 = unrestricted. A solve that starts from
        nothing draws them on a tenth of this once late acceptance has started over for the first
        time."""
        close_facility: bool
        """Enable the facility-closing macro move: it empties one used facility in
        a single step, the only way past the setup-cost step function."""
        close_facility_keep: int
        """How far down a consumer's distance-ordered facility list the two macro
        moves look for a new home (SwapFacilityMove for the consumers of the
        facility it closes). 0 switches both moves off."""
        close_facility_migrate_probability: float
        """Probability that an emptied facility's consumers migrate together to one
        alternative site instead of being redistributed over the open ones."""
        swap_facility: bool
        """Enable SwapFacilityMove: open a closed facility and close the one that serves its
        surroundings in a single step; the closed one's consumers go to their nearest facility with
        room, and the facilities with room take the consumers nearer to them. Without it the search
        keeps the facilities it first settles on."""

    custom_moves: CustomMoves
    """A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

    class Moves:
        """``config.moves``: A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

        change_weight: float
        swap_weight: float

    moves: Moves
    """A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

    class Selection:
        """``config.selection``: What the move mix moves: nearby candidates and this use case's hints."""

        nearby_probability: float
        """Bias swap partners (and the LNS related ruin) toward geographically
        close consumers with this probability. 0 = off (uniform-random)."""
        nearby_size: int
        value_recommender_probability: float
        """Probability that a change move draws the consumer's new facility from
        its value_recommender_keep nearest facilities instead of uniformly."""
        value_recommender_keep: int

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(8 * consumer count, 400, 3200). The first stage's length: the search restarts with
        a doubled history when it stalls and the next stage fits in the budget. A solve that starts
        from a given solution keeps this one length."""

    acceptance: Acceptance
    """Whether the candidate becomes the current solution."""

    class Alns:
        """``config.alns``: The ruin & recreate kick itself."""

        min_ruin_fraction: float
        max_ruin_fraction: float
        max_ruin_count: int
        operator_decay: float
        rewards: list[float]
        polish_steps: int

    alns: Alns
    """The ruin & recreate kick itself."""

    class Run:
        """``config.run``: Seeds, parallel starts and debugging aids."""

        random_seed: int
        parallel_solver_count: int
        full_assert: bool
        on_new_best: Callable[[int, Score], None] | None

    run: Run
    """Seeds, parallel starts and debugging aids."""

class Solver:
    """Construction heuristic + Late Acceptance local search (+ adaptive
    ruin-and-recreate). Solves IN PLACE: fills consumer.facility and
    problem.score."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: FacilityLocationProblem) -> None: ...
    def solve_with_constraints(self, problem: FacilityLocationProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.facility_location.constraints). The given list replaces the default set; None
        keeps the default set. A name may be picked several times: each pick is its own
        instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    facility_capacity_cost, facility_setup_cost, distance_from_facility_cost.
    The same names optimaru.facility_location.constraints uses."""

def constraint_costs(
    problem: FacilityLocationProblem, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw per-facility cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}. None = the default constraints;
    a pick list (as for solve) scores exactly those picks, each entry carrying its
    pick's knobs, the k-th pick of a name keyed "<name>#<k>" from the second on.
    optimaru.facility_location.constraints.score() turns it into a score. (A domain with a
    medium level adds "medium_fold" to every entry, a scoped constraint its evaluated
    "units".)"""

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
    facility_capacity_cost (hard, 1), facility_setup_cost (soft, 2),
    distance_from_facility_cost (soft, 6); all flippable, none a reward, all
    scored by default (default_enabled)."""
