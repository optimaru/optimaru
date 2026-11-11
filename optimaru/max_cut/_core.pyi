"""Type stubs for `optimaru.max_cut._core` — the max-cut C++ solver.

The problem object holds the solution: each vertex's side (0 or 1; -1 =
unassigned, in no side) is the single assignment-shape decision variable, read and
written with `assignment()` / `set_assignment()`. A vertex with a fixed side
is pinned: the search never moves it.

Score (hard/soft, lexicographic), at the default weights: hard =
-(fixed vertices off their side + split same-side pairs + different-side pairs
left together + side-size shortfall/excess), soft = +(cut weight), a reward
(partition_imbalance_cost is an opt-in objective, weight 0). The weights are
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

class Edge:
    """Problem fact: an undirected weighted edge between two vertex indices. A
    self-loop is never cut; parallel edges add up."""

    def __init__(self, source: int, target: int, weight: int = 1) -> None: ...
    source: int
    target: int
    weight: int

class VertexPair:
    """Problem fact: two vertex indices that must share a side (same-side pairs)
    or sit on different sides (different-side pairs)."""

    def __init__(self, first: int, second: int) -> None: ...
    first: int
    second: int

class MaxCutProblem:
    """Problem and solution in one object. NOTE: list attributes return copies; assign whole lists
    and use set_assignment for the partition. Every entry point re-validates the
    facts (ValueError on an invalid one)."""

    def __init__(
        self,
        vertex_ids: list[str],
        edges: list[Edge] = ...,
        fixed_side: list[int] | None = None,
        same_partition_pairs: list[VertexPair] = ...,
        different_partition_pairs: list[VertexPair] = ...,
        min_partition_size: int | None = None,
        max_partition_size: int | None = None,
        side: list[int] | None = None,
        warm_start: bool = False,
    ) -> None:
        """`fixed_side` / `side`: one entry per vertex, each -1, 0 or 1 (None = all -1)."""
    vertex_ids: list[str]
    edges: list[Edge]
    fixed_side: list[int]
    """Per vertex: -1 = free, 0 / 1 = pinned to that side."""
    same_partition_pairs: list[VertexPair]
    different_partition_pairs: list[VertexPair]
    min_partition_size: int | None
    """None = 0."""
    max_partition_size: int | None
    """None = the vertex count. min == max (both given) = exact size: the search only swaps."""
    warm_start: bool
    """Start the next solve from the current partition (unassigned vertices take
    the default start); the solve resets it to False."""
    @property
    def score(self) -> Score: ...
    @property
    def scored(self) -> bool:
        """True once calculate_score or a solve has scored the partition."""
    def vertex_count(self) -> int: ...
    def cut_weight(self) -> int:
        """Weight of the edges with both endpoints assigned and on different sides."""
    def total_edge_weight(self) -> int: ...
    def side_size(self, side: int) -> int:
        """Vertices on `side` (0 or 1)."""
    def is_exact_size(self) -> bool:
        """min_partition_size == max_partition_size, both given."""
    def is_complete(self) -> bool:
        """Every vertex on side 0 or 1."""
    def assignment(self) -> list[int]:
        """The decision variable per vertex (0 / 1 / -1)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the decision variable (one entry per vertex, each -1, 0 or 1)."""

def calculate_score(problem: MaxCutProblem) -> Score:
    """Full recompute of the score at the default constraints (also stores
    problem.score and sets problem.scored). Raises ValueError on invalid facts. An
    unassigned vertex (-1) is in no side: it adds nothing to the cut, the sizes or
    the pair costs, except that a same-side pair whose FIRST vertex is assigned and
    whose second is not counts as split. (max_cut.calculate_score refuses a partial
    partition instead.)"""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget (30 s by default). 0 returns the start partition unchanged."""
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    """0 = no step limit."""
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    """Independent starts (seed + i each); the best wins. Ignored by
    solve_with_constraints."""
    late_acceptance_size: int
    """0 = auto: clamp(8 * vertex count, 400, 3200). The first stage's length: the search restarts with
    a doubled history when it stalls and the next stage fits in the budget. A solve that starts
    from a given solution keeps this one length."""
    full_assert: bool
    ruin_recreate: bool
    """Adaptive ruin-and-recreate kicks on stagnation (off in exact-size mode)."""
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    ruin_recreate_as_move_weight: float
    """Destroy-and-repair as an ordinary acceptor-judged move (the engine's
    ruin_recreate.as_move_weight); 0 (default) = off; off in exact-size mode."""
    ruin_recreate_as_move_max_ruin: int
    change_move_weight: float
    """A change move flips one vertex (0.8 by default; 0 in exact-size mode)."""
    swap_move_weight: float
    """A swap move exchanges a side-0 and a side-1 vertex (0.2 by default; 1 in
    exact-size mode)."""
    descent_weight: float
    """Assignment-shape best-improvement descent strategy weight. 0 = off (off in exact-size mode)."""
    best_score_limit: tuple[int, float]
    """(hard, soft): stop early once bestScore >= this. (0, -inf) stops at
    the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None
    """Called as on_new_best(step, score) on every new best (by the first parallel start
    only). An exception it raises stops the solve and is raised by solve()."""

class Solver:
    """Start partition + Late Acceptance local search (+ adaptive
    ruin-and-recreate). Solves IN PLACE: sets the partition, problem.score,
    scored = True and warm_start = False."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: MaxCutProblem) -> None: ...
    def solve_with_constraints(self, problem: MaxCutProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.max_cut.constraints). The given list replaces the default set; None
        keeps the default set. Single start. A name may be picked several times: each
        pick is its own instance with its own knobs (its level included), and the
        instances add up. There is no medium level: level="medium" raises ValueError."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    fixed_partition_cost, same_partition_pairs_cost, different_partition_pairs_cost,
    partition_size_bounds_cost, partition_imbalance_cost, cut_weight_cost. The same
    names optimaru.max_cut.constraints uses."""

def constraint_costs(problem: MaxCutProblem, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}, "level" being the level in
    effect (the pick's, or the default) and "hard" the same as level == "hard". The
    four unit constraints report [side 0, side 1]; partition_imbalance_cost and
    cut_weight_cost one total each ("global": True). None = the default constraints;
    a pick list (as for solve) scores exactly those picks, each entry carrying its
    pick's knobs, the k-th pick of a name keyed "<name>#<k>" from the second on.
    optimaru.max_cut.constraints.score() turns it into a score. Unassigned vertices follow
    the calculate_score rule."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the four hard
    constraints at weight 1 (flippable), partition_imbalance_cost (soft, weight 0:
    opt-in, default_enabled False), then cut_weight_cost (soft, weight 1, a
    reward: level="hard" is refused and its level cannot change)."""
