"""Type stubs for `bin_packing._core` — the bin-packing C++ solver.

The problem is the planning solution: each item's `bin` (a 0-based bin index
into the n identical bins, -1 == unassigned) is the single BASIC planning
variable.

Score (HardSoftScore, lexicographic), at the default weights (1 each): hard =
-(load above capacity + items beyond the first of their colour in a bin +
conflict pairs sharing a bin), soft = -(bins in use). The weights are the C++
components' defaults (constraint_info()); the problem carries none.
"""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple

class Score:
    def __init__(self, hard: int = 0, soft: float = 0.0) -> None: ...
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True iff hard >= 0."""
    def __eq__(self, other: object) -> bool: ...
    def __ne__(self, other: object) -> bool: ...
    def __lt__(self, other: Score) -> bool: ...
    def __le__(self, other: Score) -> bool: ...
    def __gt__(self, other: Score) -> bool: ...
    def __ge__(self, other: Score) -> bool: ...
    def __hash__(self) -> int: ...

class Item:
    """Planning entity: one item packed into exactly one bin."""

    def __init__(self, id: str, weight: int, bin: int = -1, color: str | None = None) -> None: ...
    id: str
    weight: int
    bin: int
    """The planning variable: a 0-based bin index (0..item count - 1); -1 == unassigned."""
    color: str | None
    def is_assigned(self) -> bool: ...

class BinPackingProblem:
    """Planning solution. NOTE: `items` element access returns copies; use
    set_assignment for bin writes. Replacing `items` or `conflicts` is fine."""

    def __init__(
        self,
        bin_capacity: int,
        items: list[Item],
        conflicts: list[tuple[str, str]] = ...,
        warm_start: bool = False,
    ) -> None: ...
    bin_capacity: int
    items: list[Item]
    conflicts: list[tuple[str, str]]
    warm_start: bool
    """True = the next solve starts from the items' bins (a seed); every solve clears it."""
    @property
    def score(self) -> Score: ...
    def item_count(self) -> int: ...
    def bins_used(self) -> int:
        """Distinct bins holding at least one item."""
    def bin_loads(self) -> list[int]:
        """[] when nothing is assigned, else the load of every bin 0..max bin."""
    def assignment(self) -> list[int]:
        """The current bin per item (-1 == unassigned)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the planning variable (range-checked when scored or solved)."""

def calculate_score(problem: BinPackingProblem, constraints: Iterable[Any] | None = ...) -> Score:
    """Full recompute of the HardSoftScore (also stores problem.score). None scores the
    default set; a list scores exactly those picks (see bin_packing.constraints), a
    name picked several times once per pick.
    ValueError for a bin_capacity <= 0, a repeated id, a weight <= 0, a bad conflict
    or a bin outside -1..item count - 1."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget (30 s). 0 = no search: the construction / seed comes back."""
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    """Independent starts (threads), seed + i each; the best wins."""
    late_acceptance_size: int
    """0 = auto: clamp(8 * item count, 400, 3200)."""
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
    ruinRecreate.asMoveWeight). 0 = off."""
    ruin_recreate_as_move_max_ruin: int
    change_move_weight: float
    swap_move_weight: float
    value_recommender_probability: float
    """Probability that a change move draws the item's new bin as the bin of a
    random other item (a bin in use) instead of uniformly over all n bins."""
    descent_weight: float
    """BASIC best-improvement descent strategy weight. 0 = off."""
    custom_move_probability: float
    """Gate on how often a step may draw one of the macro moves (EmptyBinMove,
    RepackMove); 1 = unrestricted."""
    empty_bin_moves: bool
    """Enable the bin-emptying macro move: it moves every item of one light bin
    into the other bins in a single step, the only way past the bins-used step
    function. It checks the capacity only."""
    empty_bin_candidates: int
    """Both macro moves empty one of this many lightest bins (1 = always the
    lightest)."""
    repack_moves: bool
    """Enable RepackMove: empty one light bin together with 1 to
    repack_partner_bins other light bins and put their items back best fit
    decreasing, only where they add no capacity, colour or conflict violation.
    The move that closes bins on instances with colours or conflicts."""
    repack_partner_bins: int
    """Most partner bins RepackMove empties with its source (2; 0 = the source
    alone). ValueError below 0."""
    best_score_limit: tuple[int, float]
    """(hard, soft): stop early once bestScore >= this. (0, -inf) stops at
    the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Best-fit-decreasing construction + Late Acceptance local search (+ the
    EmptyBinMove, RepackMove and adaptive ruin-and-recreate). Solves IN PLACE: fills
    item.bin and problem.score, clears warm_start."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: BinPackingProblem) -> None: ...
    def solve_with_constraints(self, problem: BinPackingProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see bin_packing.constraints). The given list replaces the default set; None
        keeps the default set. Single start. A name may be picked several times: each
        pick is its own instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost.
    The same names bin_packing.constraints uses."""

def constraint_costs(
    problem: BinPackingProblem, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "hard": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}. The three per-bin constraints
    list one cost per bin 0..item count - 1, an empty bin included; used_bins_cost
    (global) lists the one bin count. None = the default constraints; a pick list (as
    for solve) scores exactly those picks, each entry carrying its pick's knobs, the
    k-th pick of a name keyed "<name>#<k>" from the second on.
    bin_packing.constraints.score() turns it into a score. Raises ValueError for the
    same facts as calculate_score."""

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
    bin_capacity_cost (hard, 1), color_conflict_cost (hard, 1),
    item_conflict_cost (hard, 1), used_bins_cost (soft, 1); all flippable, none a
    reward, all scored by default (default_enabled)."""
