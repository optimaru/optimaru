"""Type stubs for `optimaru.box_sizing._core` — the box-sizing C++ solver.

The problem object holds the solution: each parcel's `box` (an index into
`problem.boxes`, -1 == unassigned) is the single assignment-shape decision variable.

Score (hard/soft, lexicographic), at the default weights (1 each): hard =
-(length by which parcels exceed their box + parcel weight above the box's
max_weight + distinct boxes in use above max_box_types), soft = -(quantity x
(box volume - parcel volume) + quantity x box cost, over the parcels). The
weights are the C++ components' defaults (constraint_info()); the problem
carries none.
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

class Box:
    """Problem fact: one candidate box size. Lengths and weights are whole numbers in
    the caller's unit."""

    def __init__(
        self,
        id: str,
        length: int,
        width: int,
        height: int,
        max_weight: int | None = None,
        cost: float = 0.0,
        required: bool = False,
    ) -> None: ...
    id: str
    length: int
    """Inner size: 1 to 2,000,000 each side."""
    width: int
    height: int
    max_weight: int | None
    """The heaviest parcel the box carries; None == no limit."""
    cost: float
    """What one box of this size costs (material, freight); >= 0."""
    required: bool
    """Already stocked and staying: it counts as in use even when no parcel takes it."""

class Parcel:
    """The packed content of one shipment, by its bounding size. It fits a box when its
    three sides, sorted, are each at most the box's sorted sides (any rotation)."""

    def __init__(
        self,
        id: str,
        length: int,
        width: int,
        height: int,
        weight: int = 0,
        quantity: int = 1,
        box: int = -1,
    ) -> None: ...
    id: str
    length: int
    width: int
    height: int
    weight: int
    """Of one shipment."""
    quantity: int
    """How many shipments had this size; they all ship in the same box."""
    box: int
    """The decision variable: index into problem.boxes; -1 == unassigned."""
    def is_assigned(self) -> bool: ...

class BoxSizingProblem:
    """Problem and solution in one object. NOTE: `boxes` / `parcels` element access returns
    copies; use set_assignment for the parcels' boxes. Replacing either list is fine."""

    def __init__(
        self,
        boxes: list[Box],
        parcels: list[Parcel],
        max_box_types: int | None = None,
        warm_start: bool = False,
    ) -> None: ...
    boxes: list[Box]
    parcels: list[Parcel]
    max_box_types: int | None
    """How many distinct boxes may be in use, the required ones included; None == no limit."""
    warm_start: bool
    """True = the next solve starts from the parcels' boxes (a seed); every solve clears it."""
    @property
    def score(self) -> Score: ...
    def box_count(self) -> int: ...
    def parcel_count(self) -> int: ...
    def box_types_used(self) -> int:
        """Distinct boxes in use: those a parcel ships in and the required ones."""
    def void_volume(self) -> int:
        """Sum over the assigned parcels of quantity * (box volume - parcel volume); a parcel
        larger than its box counts 0."""
    def fill_ratio(self) -> float:
        """Shipped parcel volume / shipped box volume; 0.0 when nothing is assigned. A
        parcel larger than its box (which does not fit it) counts as the box's volume, so
        the ratio is at most 1 and void_volume() is the shipped box volume times
        (1 - fill_ratio())."""
    def parcels_per_box(self) -> list[int]:
        """Per box, the shipments in it: the quantities of its parcels, summed."""
    def assignment(self) -> list[int]:
        """The current box index per parcel (-1 == unassigned)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the decision variable (range-checked when scored or solved)."""

def calculate_score(problem: BoxSizingProblem, constraints: Iterable[Any] | None = ...) -> Score:
    """Full recompute of the score (also stores problem.score). None scores the
    default set; a list scores exactly those picks (see optimaru.box_sizing.constraints), a
    name picked several times once per pick.
    ValueError for no candidate box, a repeated or empty id, a side outside
    1..2,000,000, a weight or max_weight outside 0..1e12, a quantity below 1, a cost
    that is negative or not finite, max_box_types below 1, a shipped volume a double
    cannot hold exactly (summed quantities x the largest box volume above 2^53), or a
    box outside -1..box count - 1."""

class SolverConfig:
    """The solver settings, by stage of the solve: ``stop``, ``alns_trigger``, ``strategies``, ``custom_moves``, ``moves``, ``acceptance``, ``alns``, ``run``.

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
        """Wall-clock budget (30 s). 0 = no search: the greedy construction (or the seed,
        completed) comes back."""
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
        """False by default here: a kick re-places the ruined parcels one at a time,
        and each goes back to the size it had."""
        unimproved_steps: int

    alns_trigger: AlnsTrigger
    """When a stalled search is kicked by ruin & recreate."""

    class Strategies:
        """``config.strategies``: A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

        ruin_recreate_weight: float
        """Destroy-and-repair as an ordinary acceptor-judged move (the engine's
        strategies.ruin_recreate_weight). 0 = off."""
        ruin_recreate_max_ruin: int

    strategies: Strategies
    """A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, the moves on the set of sizes, taking turns evenly with the move mix on their share of steps."""

        share: float
        """Gate on how often a step may draw one of the moves on the set of sizes
        (CloseBoxMove, OpenBoxMove, SwapBoxMove); 1 = unrestricted. Inside the gate
        the three and the move mix take turns evenly, so three steps in four are
        theirs."""
        close_box: bool
        """Enable CloseBoxMove: take one size out of use and send each of its parcels
        to the cheapest remaining size that takes it."""
        open_box: bool
        """Enable OpenBoxMove: put one more size into use and pull in every parcel it
        serves cheaper. No move at max_box_types."""
        swap_box: bool
        """Enable SwapBoxMove: open a size and close another in one step, the size
        whose closing costs the least three times in four. At max_box_types the only
        way to another set of sizes."""

    custom_moves: CustomMoves
    """A step's move, next: this use case's own moves, taking turns evenly with the move mix on their share of steps."""

    class Moves:
        """``config.moves``: A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

        change_weight: float
        swap_weight: float

    moves: Moves
    """A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(8 * box count, 100, 800)."""
        reheat: bool
        """Late acceptance refills its history with a score just below its best (0.5%
        to 8% of the soft score, in turn) after two histories without an improving step,
        so the search leaves a set of sizes no single move improves. False: plain late
        acceptance, which from the greedy construction is a descent and nothing more."""

    acceptance: Acceptance
    """Whether the candidate becomes the current solution."""

    class Alns:
        """``config.alns``: The ruin & recreate kick itself (off unless alns_trigger.enabled)."""

        min_ruin_fraction: float
        max_ruin_fraction: float
        max_ruin_count: int
        operator_decay: float
        rewards: list[float]
        polish_steps: int

    alns: Alns
    """The ruin & recreate kick itself (off unless alns_trigger.enabled)."""

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
    """Greedy construction over the sizes + late acceptance that starts over just
    below its best, with CloseBoxMove, OpenBoxMove and SwapBoxMove (ruin & recreate
    kicks off by default). Solves IN PLACE: fills parcel.box and problem.score,
    clears warm_start. A plain solve ignores the boxes on the parcels; with warm_start
    it starts from them. ValueError for what calculate_score refuses, for a parcel that
    fits none of the candidate boxes (by size and weight) and for more required boxes
    than max_box_types."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: BoxSizingProblem) -> None: ...
    def solve_with_constraints(self, problem: BoxSizingProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.box_sizing.constraints). The given list replaces the default set; None
        keeps the default set. Single start. The construction and the moves price the
        sizes with the picked weights of void_volume_cost, box_cost and box_type_cost
        (the weights alone: a level, an exponent or an offset shapes the score a step is
        judged by, not which step is proposed).
        A name may be picked several times: each pick is its own instance with its own
        knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    parcel_fit_cost, box_weight_cost, void_volume_cost, box_cost,
    box_type_limit_cost, box_type_cost. The same names
    optimaru.box_sizing.constraints uses."""

def constraint_costs(problem: BoxSizingProblem, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}. The four per-box constraints
    list one cost per candidate box, a box no parcel ships in included;
    box_type_limit_cost and box_type_cost (global) list their one number. None = the
    default constraints; a pick list (as for solve) scores exactly those picks, each
    entry carrying its pick's knobs, the k-th pick of a name keyed "<name>#<k>" from
    the second on. optimaru.box_sizing.constraints.score() turns it into a score.
    Raises ValueError for the same facts as calculate_score."""

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
    parcel_fit_cost (hard, 1), box_weight_cost (hard, 1), void_volume_cost (soft, 1),
    box_cost (soft, 1), box_type_limit_cost (hard, 1), box_type_cost (soft, 0: opt-in,
    default_enabled False); all flippable, none a reward."""
