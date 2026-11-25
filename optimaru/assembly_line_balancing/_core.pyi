"""Type stubs for `optimaru.assembly_line_balancing._core` — the assembly-line-balancing
C++ solver.

The problem object holds the solution: each task's `station` (a station index,
-1 == unassigned) is the single assignment-shape decision variable. Stations are not
bounded above by the facts; a solve freezes the value range at
max(task count, 1 + max station) after its construction (`station_count`).

Score (hard/soft, lexicographic), at the default weights: hard = -(the
eight hard raw costs, weight 1 each), soft = -(stations used); the
workload range is an opt-in objective (default weight 0). The weights are
the C++ components' defaults (constraint_info()); the problem carries none.
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

class Task:
    """One task with its processing time and station domain."""

    def __init__(
        self,
        id: str,
        duration: int,
        min_station: int = 0,
        max_station: int | None = None,
        fixed_station: int | None = None,
        forbidden_stations: list[int] = ...,
        station: int = -1,
    ) -> None: ...
    id: str
    duration: int
    min_station: int
    max_station: int | None
    fixed_station: int | None
    forbidden_stations: list[int]
    station: int
    """The decision variable: a station index; -1 == unassigned."""
    def is_assigned(self) -> bool: ...

class Precedence:
    """`before` (a task index) must not sit on a later station than `after`."""

    def __init__(self, before: int, after: int) -> None: ...
    before: int
    after: int

class TaskGroup:
    """Tasks (indices) that should share one station."""

    def __init__(self, id: str, tasks: list[int]) -> None: ...
    id: str
    tasks: list[int]

class TaskPair:
    """Two tasks (indices) that must not share a station."""

    def __init__(self, first: int, second: int) -> None: ...
    first: int
    second: int

class AssemblyLineBalancingProblem:
    """Problem and solution in one object. NOTE: `tasks` element access returns copies; use
    set_assignment for bulk station writes."""

    def __init__(
        self,
        cycle_time: int,
        tasks: list[Task],
        precedences: list[Precedence] = ...,
        max_stations: int | None = None,
        same_station_groups: list[TaskGroup] = ...,
        different_station_pairs: list[TaskPair] = ...,
        warm_start: bool = False,
    ) -> None: ...
    cycle_time: int
    tasks: list[Task]
    precedences: list[Precedence]
    max_stations: int | None
    same_station_groups: list[TaskGroup]
    different_station_pairs: list[TaskPair]
    warm_start: bool
    """Start the next solve from the current stations (tasks at -1 go through the
    construction). False: the solve ignores the input stations. Reset to False by
    every solve."""
    @property
    def score(self) -> Score: ...
    @property
    def station_count(self) -> int:
        """The value range of the last solve / scoring: max(task count, 1 + max station)."""
    def task_count(self) -> int: ...
    def station_loads(self) -> list[int]:
        """Processing time per station 0..max(station), empty middle stations included."""
    def stations_used(self) -> int:
        """Stations with a load > 0."""
    def assignment(self) -> list[int]:
        """The current station per task (-1 == unassigned)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the decision variable (every station >= -1)."""

def calculate_score(problem: AssemblyLineBalancingProblem) -> Score:
    """Full recompute of the score at the default constraints (also stores
    problem.score). Unassigned tasks contribute nothing."""

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
        """Wall-clock budget (30 s). 0 stops before the first step: the solve returns the
        construction, or a complete warm-start seed, unchanged."""
        unimproved_time_limit_seconds: float
        step_limit: int
        """0 = no step limit."""
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
        ruin_recreate.as_move_weight); one such move costs ~ruinCount * station count
        score evaluations. 0 = off."""
        ruin_recreate_max_ruin: int
        descent_weight: float
        """Assignment-shape best-improvement descent strategy weight. 0 = off."""

    strategies: Strategies
    """A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        share: float
        """Gate on how often a step may draw CloseStationMove; 1 = unrestricted."""
        close_station: bool
        """Enable the station-closing macro move: it empties one used station in a
        single step, the only way past the stations-used step function."""

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

        value_recommender_probability: float
        """Probability that a change move draws the task's new station from its current
        precedence / domain window instead of uniformly over all stations (0.5)."""

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(8 * task count, 400, 3200)."""

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
    """Domain construction + Late Acceptance local search (+ adaptive
    ruin-and-recreate, CloseStationMove). Solves IN PLACE: fills task.station and
    problem.score and resets warm_start to False."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: AssemblyLineBalancingProblem) -> None: ...
    def solve_with_constraints(
        self, problem: AssemblyLineBalancingProblem, constraints: Iterable[Any] | None = ...
    ) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.assembly_line_balancing.constraints). The given list replaces the default set;
        None keeps the default set. Single start. A name may be picked several times: each
        pick is its own instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (catalog order, snake_case):
    station_capacity_cost, task_precedence_cost, max_stations_cost,
    task_station_bounds_cost, fixed_task_stations_cost, forbidden_task_stations_cost,
    same_station_groups_cost, different_station_pairs_cost, used_stations_cost,
    workload_imbalance_cost. The same names optimaru.assembly_line_balancing.constraints uses."""

def constraint_costs(
    problem: AssemblyLineBalancingProblem, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level, in
    list_constraints() order: {key: {"costs": [...], "weight": float, "exponent": float,
    "offset": float, "clip": bool, "global": bool,
    "level": "hard" | "medium" | "soft", "reward": bool, "level_flippable": bool,
    "name": str, "label": str, "default_weight": float, "default_level": str}}. "costs" has
    one entry per station (0 .. station_count - 1) for a per-station constraint and one total
    for a global one (task_precedence_cost, same_station_groups_cost, used_stations_cost,
    workload_imbalance_cost); the scoped max_stations_cost and forbidden_task_stations_cost
    add "units", the stations they evaluate (ascending). None = the default constraints; a
    pick list (as for solve) scores exactly those picks, each entry carrying its pick's
    knobs, the k-th pick of a name keyed "<name>#<k>" from the second on (each name's
    instances together, the names in catalog order).
    optimaru.assembly_line_balancing.constraints.score() turns it into a score."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the eight hard
    constraints (hard, 1), used_stations_cost (soft, 1) and
    workload_imbalance_cost (soft, 0, default_enabled False); all flippable,
    none a reward."""
