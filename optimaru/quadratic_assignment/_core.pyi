"""Type stubs for `optimaru.quadratic_assignment._core` — the quadratic-assignment C++
solver.

The problem object holds the solution: each facility's `location` (an index
into `problem.locations`, -1 == unassigned) is the single assignment-shape decision
variable. The solver keeps the facilities a permutation of the locations
(swap moves only).

Score (hard/soft, lexicographic), at the default weights: hard =
-(pinned locations not holding their facility + facilities at a forbidden
location + demand above location capacities), soft = -(direct assignment cost
+ flow-distance cost). The weights are the C++ components' defaults
(constraint_info()); the problem carries none.
"""

from collections.abc import Callable, Iterable
from typing import Any, ClassVar, Literal, NamedTuple

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True iff hard >= 0."""

class Facility:
    """A facility placed at exactly one location."""

    def __init__(self, id: str, demand: int | None = None, location: int = -1) -> None: ...
    id: str
    demand: int | None
    """facility_demands[id]; None = not listed (scored as demand 0)."""
    location: int
    """The decision variable: index into problem.locations; -1 == unassigned."""
    def is_assigned(self) -> bool: ...

class Location:
    """Problem fact: a site that holds one facility."""

    def __init__(self, id: str, capacity: int | None = None) -> None: ...
    id: str
    capacity: int | None
    """location_capacities[id]; None = no capacity limit."""

class FixedAssignment:
    """One fixed_assignments entry, by index: `location` must hold `facility`."""

    def __init__(self, location: int, facility: int) -> None: ...
    location: int
    facility: int

class ForbiddenAssignment:
    """One forbidden_assignments row, by index: `facility` must not stand at
    `location`. Repeated rows are kept; scoring counts a pair once."""

    def __init__(self, facility: int, location: int) -> None: ...
    facility: int
    location: int

class QuadraticAssignmentProblem:
    """Problem and solution in one object. NOTE: element access returns copies (use
    set_assignment for bulk location writes); assigning a fact drops the
    prepared caches, so the next score or solve re-validates the problem."""

    def __init__(
        self,
        facilities: list[Facility],
        locations: list[Location],
        flows: list[list[int]],
        distances: list[list[int]],
        assignment_costs: list[list[int]] | None = None,
        fixed_assignments: list[FixedAssignment] = ...,
        forbidden_assignments: list[ForbiddenAssignment] = ...,
        warm_start: bool = False,
    ) -> None: ...
    facilities: list[Facility]
    locations: list[Location]
    flows: list[list[int]]
    """n x n, facility by facility."""
    distances: list[list[int]]
    """n x n, location by location."""
    assignment_costs: list[list[int]] | None
    """n x n, facility by location; None = no direct assignment cost."""
    fixed_assignments: list[FixedAssignment]
    forbidden_assignments: list[ForbiddenAssignment]
    warm_start: bool
    """Start the next solve from the current assignment, which must be a full
    bijection (else ValueError); the solve resets it to False. Without it the
    solve starts from the identity (facility i at location i)."""
    @property
    def score(self) -> Score | None:
        """The score last computed on this problem (a copy): None until
        calculate_score() or a solve has scored it. Later writes to the
        assignment or the facts do not reset it."""
    def size(self) -> int: ...
    def facility_count(self) -> int: ...
    def location_count(self) -> int: ...
    def is_bijection(self) -> bool:
        """Every facility at its own location and every location used once."""
    def flow_distance_cost(self) -> int:
        """sum over assigned facilities a, b of `flows[a][b] * distances[loc a][loc b]`."""
    def assignment_cost(self) -> int:
        """sum over assigned facilities f of `assignment_costs[f][loc f]` (0 without a matrix)."""
    def total_cost(self) -> int:
        """The model's total_cost: flow-distance plus direct assignment cost, raw."""
    def assignment(self) -> list[int]:
        """Per location, the index of the facility standing there (-1 = empty)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the decision variable, per location (-1 = empty). A
        facility named twice or an index out of range raises ValueError;
        facilities not named become unassigned."""

def calculate_score(problem: QuadraticAssignmentProblem) -> Score:
    """Full recompute of the score at the default constraints (also
    stores problem.score)."""

class AcceptorType:
    HILL_CLIMBING: ClassVar[AcceptorType]
    LATE_ACCEPTANCE: ClassVar[AcceptorType]
    ENTITY_TABU: ClassVar[AcceptorType]
    SIMULATED_ANNEALING: ClassVar[AcceptorType]
    GREAT_DELUGE: ClassVar[AcceptorType]
    STEP_COUNTING_HILL_CLIMBING: ClassVar[AcceptorType]
    RECORD_TO_RECORD_TRAVEL: ClassVar[AcceptorType]
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget (30 s by default). 0 returns the start unchanged."""
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    """0 = no step limit."""
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    """Independent starts (seed + i each); ignored by solve_with_constraints."""
    acceptor_type: AcceptorType
    """LATE_ACCEPTANCE by default."""
    late_acceptance_size: int
    """0 = auto: clamp(8 * facility count, 400, 3200)."""
    entity_tabu_size: int
    """ENTITY_TABU: for how many steps a moved facility stays tabu (7 by default; >= 0).
    ENTITY_TABU accepts only non-worsening swaps (sideways ones blocked on tabu facilities),
    so it climbs like HILL_CLIMBING."""
    simulated_annealing_starting_temperature: tuple[int, float]
    """SIMULATED_ANNEALING's (hard, soft) starting temperature, cooling to 0 over the solve.
    (0, 0), the default, = auto: hard 0, soft = simulated_annealing_temperature_fraction x
    |start soft| (at least 1)."""
    simulated_annealing_temperature_fraction: float
    """The auto temperature's fraction of the start's |soft| (0.01 by default; > 0)."""
    great_deluge_water_level_step: tuple[int, float]
    """GREAT_DELUGE: how much the (hard, soft) water level, which starts at the start's score,
    rises per step. (0, 0), the default, leaves it there: only moves that keep the score at or
    above the start's are taken."""
    step_counting_hill_climbing_size: int
    """STEP_COUNTING_HILL_CLIMBING: the bar is reset to the current score every this many
    steps (400 by default; > 0)."""
    record_to_record_start_gap: float
    record_to_record_end_gap: float
    """RECORD_TO_RECORD_TRAVEL: the acceptable gap below the best score, as a fraction of the
    start's |soft|, shrinking from start (0.02) to end (0.0) over the solve."""
    full_assert: bool
    best_score_limit: tuple[int, float]
    """(hard, soft): stop early once bestScore >= this. (0, -inf) stops at
    the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Identity (or warm-start) start + local search over swap moves (late
    acceptance by default). Solves IN PLACE: sets facility.location,
    problem.score and warm_start = False; the result is always a bijection."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: QuadraticAssignmentProblem) -> None: ...
    def solve_with_constraints(
        self, problem: QuadraticAssignmentProblem, constraints: Iterable[Any] | None = ...
    ) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.quadratic_assignment.constraints). The given list replaces the
        default set; None keeps the default set. Single start. A name may be picked
        several times: each pick is its own instance with its own knobs (its level
        included), and the instances add up. There is no medium level:
        level="medium" raises ValueError."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    pinned_assignments_cost, forbidden_assignments_cost, location_capacity_cost,
    assignment_cost, flow_distance_cost. The same names
    optimaru.quadratic_assignment.constraints uses."""

def constraint_costs(
    problem: QuadraticAssignmentProblem, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str, optionally "units": [int, ...] and "signed": True}},
    "level" being the level in effect (the pick's, or the default) and "hard" the same as
    level == "hard". "signed" marks an objective whose data holds a negative entry (its
    total can be negative, so it cannot be hard). The three side constraints list one
    raw cost per location (location order) and are scoped: "units" holds the locations
    each evaluates (ascending: the fixed, the forbidden-row and the capacitated
    locations), the only ones a literal offset charges; assignment_cost and
    flow_distance_cost are global (one total each). None = the default constraints; a
    pick list (as for solve) scores exactly those picks, each entry carrying its pick's
    knobs, the k-th pick of a name keyed "<name>#<k>" from the second on.
    optimaru.quadratic_assignment.constraints.score() turns it into a score."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the three hard side
    constraints, then the two soft objectives, all at weight 1, flippable and
    scored by default (default_enabled)."""
