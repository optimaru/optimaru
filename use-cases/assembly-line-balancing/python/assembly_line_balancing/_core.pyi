"""Type stubs for `assembly_line_balancing._core` — the assembly-line-balancing
C++ solver.

The problem is the planning solution: each task's `station` (a station index,
-1 == unassigned) is the single assignment-shape planning variable. Stations are not
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
        """True iff hard >= 0."""

class Task:
    """Planning entity: one task with its processing time and station domain."""

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
    """The planning variable: a station index; -1 == unassigned."""
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
    """Planning solution. NOTE: `tasks` element access returns copies; use
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
        """Bulk write of the planning variable (every station >= -1)."""

def calculate_score(problem: AssemblyLineBalancingProblem) -> Score:
    """Full recompute of the score at the default constraints (also stores
    problem.score). Unassigned tasks contribute nothing."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget (30 s). 0 stops before the first step: the solve returns the
    construction, or a complete warm-start seed, unchanged."""
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    """0 = no step limit."""
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = auto: clamp(8 * task count, 400, 3200)."""
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
    ruinRecreate.asMoveWeight); one such move costs ~ruinCount * station count
    score evaluations. 0 = off."""
    ruin_recreate_as_move_max_ruin: int
    change_move_weight: float
    swap_move_weight: float
    value_recommender_probability: float
    """Probability that a change move draws the task's new station from its current
    precedence / domain window instead of uniformly over all stations (0.5)."""
    descent_weight: float
    """Assignment-shape best-improvement descent strategy weight. 0 = off."""
    custom_move_probability: float
    """Gate on how often a step may draw CloseStationMove; 1 = unrestricted."""
    close_station_moves: bool
    """Enable the station-closing macro move: it empties one used station in a
    single step, the only way past the stations-used step function."""
    best_score_limit: tuple[int, float]
    """(hard, soft): stop early once bestScore >= this. (0, -inf) stops at
    the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None

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
        (see assembly_line_balancing.constraints). The given list replaces the default set;
        None keeps the default set. Single start. A name may be picked several times: each
        pick is its own instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (catalog order, snake_case):
    station_capacity_cost, task_precedence_cost, max_stations_cost,
    task_station_bounds_cost, fixed_task_stations_cost, forbidden_task_stations_cost,
    same_station_groups_cost, different_station_pairs_cost, used_stations_cost,
    workload_imbalance_cost. The same names assembly_line_balancing.constraints uses."""

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
    assembly_line_balancing.constraints.score() turns it into a score."""

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
