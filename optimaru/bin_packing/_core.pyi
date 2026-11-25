"""Type stubs for `optimaru.bin_packing._core` — the bin-packing C++ solver.

The problem object holds the solution: each item's `bin` (a 0-based bin index
into the n identical bins, -1 == unassigned) is the single assignment-shape decision
variable.

Score (hard/soft, lexicographic), at the default weights (1 each): hard =
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
        """True when hard >= 0: no hard constraint is violated."""
    def __eq__(self, other: object) -> bool: ...
    def __ne__(self, other: object) -> bool: ...
    def __lt__(self, other: Score) -> bool: ...
    def __le__(self, other: Score) -> bool: ...
    def __gt__(self, other: Score) -> bool: ...
    def __ge__(self, other: Score) -> bool: ...
    def __hash__(self) -> int: ...

class Item:
    """One item packed into exactly one bin."""

    def __init__(self, id: str, weight: int, bin: int = -1, color: str | None = None) -> None: ...
    id: str
    weight: int
    bin: int
    """The decision variable: a 0-based bin index (0..item count - 1); -1 == unassigned."""
    color: str | None
    def is_assigned(self) -> bool: ...

class BinPackingProblem:
    """Problem and solution in one object. NOTE: `items` element access returns copies; use
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
        """Bulk write of the decision variable (range-checked when scored or solved)."""

def calculate_score(problem: BinPackingProblem, constraints: Iterable[Any] | None = ...) -> Score:
    """Full recompute of the score (also stores problem.score). None scores the
    default set; a list scores exactly those picks (see optimaru.bin_packing.constraints), a
    name picked several times once per pick.
    ValueError for a bin_capacity <= 0, a repeated id, a weight <= 0, a bad conflict
    or a bin outside -1..item count - 1."""

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
        """Wall-clock budget (30 s). 0 = no search: the construction / seed comes back."""
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
        ruin_recreate.as_move_weight). 0 = off."""
        ruin_recreate_max_ruin: int
        descent_weight: float
        """Assignment-shape best-improvement descent strategy weight. 0 = off."""

    strategies: Strategies
    """A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        share: float
        """Gate on how often a step may draw one of the macro moves (EmptyBinMove,
        RepackMove, TradeMove); 1 = unrestricted."""
        empty_bin: bool
        """Enable the bin-emptying macro move: it moves every item of one light bin
        into the other bins in a single step, the only way past the bins-used step
        function. It checks the capacity only."""
        empty_bin_candidates: int
        """EmptyBinMove and RepackMove empty one of this many lightest bins (1 =
        always the lightest); TradeMove takes its first bin from them every second
        time."""
        repack: bool
        """Enable RepackMove: empty one light bin together with 1 to
        repack_partner_bins other light bins and put their items back best fit
        decreasing, only where they add no capacity, colour or conflict violation.
        The move that closes bins on instances with colours or conflicts."""
        repack_partner_bins: int
        """Most partner bins RepackMove empties with its source (2; 0 = the source
        alone). ValueError below 0."""
        trade: bool
        """Enable TradeMove: trade up to two items of one bin for up to two of
        another so that the fuller of the two bins gets fuller, or, when no trade
        does that, so that both keep their loads with other items. The bins-used
        score does not change until a bin is empty; this move works towards that."""

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
        """Probability that a change move draws the item's new bin as the bin of a
        random other item (a bin in use) instead of uniformly over all n bins."""

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(8 * item count, 400, 3200)."""
        fill_tie_break: bool
        """A step that keeps the score is taken when it does not lower the sum of
        the squared bin loads, and otherwise one time in eight. False: every such
        step is taken, as plain late acceptance does. The score itself, the limits
        and on_new_best are the same either way."""

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
        """Independent starts (threads), seed + i each; the best wins."""
        full_assert: bool
        on_new_best: Callable[[int, Score], None] | None

    run: Run
    """Seeds, parallel starts and debugging aids."""

class Solver:
    """Best-fit-decreasing construction + Late Acceptance local search (+ the
    EmptyBinMove, RepackMove and adaptive ruin-and-recreate). Solves IN PLACE: fills
    item.bin and problem.score, clears warm_start."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: BinPackingProblem) -> None: ...
    def solve_with_constraints(self, problem: BinPackingProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.bin_packing.constraints). The given list replaces the default set; None
        keeps the default set. Single start. A name may be picked several times: each
        pick is its own instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    bin_capacity_cost, color_conflict_cost, item_conflict_cost, used_bins_cost.
    The same names optimaru.bin_packing.constraints uses."""

def constraint_costs(problem: BinPackingProblem, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}. The three per-bin constraints
    list one cost per bin 0..item count - 1, an empty bin included; used_bins_cost
    (global) lists the one bin count. None = the default constraints; a pick list (as
    for solve) scores exactly those picks, each entry carrying its pick's knobs, the
    k-th pick of a name keyed "<name>#<k>" from the second on.
    optimaru.bin_packing.constraints.score() turns it into a score. Raises ValueError for the
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
