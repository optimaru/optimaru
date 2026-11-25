"""Type stubs for `optimaru.knapsack._core` — the knapsack C++ solver.

The problem object holds the solution: each item's `selected` (1 = selected,
0 = not, -1 = undecided, counted as not selected) is the single assignment-shape
decision variable.

Score (hard/soft, lexicographic), at the default weights: hard =
-(weight over capacity + resource overflow + unselected mandatory entries +
selected forbidden entries + selection-group shortfall/excess + unmet item
dependencies), soft = +(selected value), a reward. The weights are the C++
components' defaults (constraint_info()); the problem carries none.
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

class Resource:
    """Problem fact: one additional capacity dimension (a resource_capacities entry)."""

    def __init__(self, id: str, capacity: int) -> None: ...
    id: str
    capacity: int

class Item:
    """One candidate item."""

    def __init__(
        self,
        id: str,
        weight: int,
        value: int,
        resource_usage: dict[str, int] = ...,
        required_items: list[int] = ...,
        selected: int = 0,
    ) -> None: ...
    id: str
    weight: int
    value: int
    resource_usage: dict[str, int]
    """Usage per resource id; ids without a declared Resource are ignored."""
    required_items: list[int]
    """Indices of the items that must be selected with this one (the model's
    `requires`); repeats count separately, a self-dependency never counts."""
    selected: int
    """The decision variable: 1 = selected, 0 = not selected, -1 = undecided."""
    def is_selected(self) -> bool: ...

class SelectionGroup:
    """Problem fact: between min_selected and max_selected of `items` must be selected."""

    def __init__(self, id: str, items: list[int], min_selected: int = 0, max_selected: int | None = None) -> None: ...
    id: str
    items: list[int]
    min_selected: int
    max_selected: int | None
    """None = the group size."""

class KnapsackProblem:
    """Problem and solution in one object. NOTE: `items` (and the other lists) element access
    returns copies; use set_assignment for bulk selection writes."""

    def __init__(
        self,
        capacity: int,
        items: list[Item],
        resources: list[Resource] = ...,
        mandatory_items: list[int] = ...,
        forbidden_items: list[int] = ...,
        selection_groups: list[SelectionGroup] = ...,
        warm_start: bool = False,
    ) -> None: ...
    capacity: int
    items: list[Item]
    resources: list[Resource]
    """In declaration order (the order of the model's resource_capacities)."""
    mandatory_items: list[int]
    forbidden_items: list[int]
    selection_groups: list[SelectionGroup]
    warm_start: bool
    """Start the next solve from the current selection (undecided items go
    through the greedy construction); the solve resets it to False."""
    @property
    def score(self) -> Score: ...
    def item_count(self) -> int: ...
    def total_weight(self) -> int:
        """Weight of the selected items."""
    def total_value(self) -> int:
        """Value of the selected items."""
    def assignment(self) -> list[int]:
        """The decision variable per item (1 / 0 / -1)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the decision variable (each value -1, 0 or 1)."""

def calculate_score(problem: KnapsackProblem) -> Score:
    """Full recompute of the score at the default constraints (also stores
    problem.score). Raises ValueError on invalid facts."""

def greedy_selection(problem: KnapsackProblem, fixed: list[int]) -> list[bool]:
    """The greedy construction: items by descending value / weight (ties in item
    order) while the weight fits the capacity. `fixed` holds one entry per item:
    0 / 1 kept as given (the load starts at the items fixed to 1), -1 decided by
    the greedy."""

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
        """Wall-clock budget (30 s by default). 0 returns the construction (or a
        warm-start seed) unchanged."""
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
        ruin_recreate.as_move_weight); 0.05 by default, 0 = off."""
        ruin_recreate_max_ruin: int
        descent_weight: float
        """Assignment-shape best-improvement descent strategy weight. 0 = off."""

    strategies: Strategies
    """A step's move, first: built-in strategies, each drawn by its own weight in a fixed order."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        core_repack: bool
        """Re-decide a window of the value / weight order around the break item
        exactly, by dynamic programming over the weight (on by default): the
        exchanges at the capacity boundary that no single flip or swap reaches. It
        looks at the capacity only; the score judges everything else."""

    custom_moves: CustomMoves
    """A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

    class Moves:
        """``config.moves``: A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

        change_weight: float
        """A change move flips one item."""
        swap_weight: float
        """A swap move trades a selected item for an unselected one."""

    moves: Moves
    """A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

    class Selection:
        """``config.selection``: What the move mix moves: nearby candidates and this use case's hints."""

        nearby_probability: float
        """Bias swap partners toward items of similar weight with this
        probability. 0 = off (uniform-random)."""
        nearby_size: int

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(8 * item count, 400, 3200). The first stage's length: the search restarts with
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
        """Independent starts (seed + i each); the best wins. Ignored by
        solve_with_constraints."""
        full_assert: bool
        on_new_best: Callable[[int, Score], None] | None

    run: Run
    """Seeds, parallel starts and debugging aids."""

class Solver:
    """Greedy construction + Late Acceptance local search with the core repack
    move (+ adaptive ruin-and-recreate). Solves IN PLACE: sets item.selected,
    problem.score and warm_start = False."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: KnapsackProblem) -> None: ...
    def solve_with_constraints(self, problem: KnapsackProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.knapsack.constraints). The given list replaces the default set; None
        keeps the default set. Single start. A name may be picked several times: each
        pick is its own instance with its own knobs (its level included), and the
        instances add up. There is no medium level: level="medium" raises ValueError."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    knapsack_capacity_cost, resource_capacity_cost, mandatory_items_cost,
    forbidden_items_cost, selection_group_bounds_cost, item_dependency_cost,
    selected_value_cost. The same names optimaru.knapsack.constraints uses."""

def constraint_costs(problem: KnapsackProblem, constraints: Iterable[Any] | None = ...) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [total], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}, "level" being the level in
    effect (the pick's, or the default) and "hard" the same as level == "hard".
    Every knapsack constraint is global, so "costs" holds one raw total. None = the
    default constraints; a pick list (as for solve) scores exactly those picks, each
    entry carrying its pick's knobs, the k-th pick of a name keyed "<name>#<k>" from
    the second on. optimaru.knapsack.constraints.score() turns it into a score."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the six hard
    constraints at weight 1 (flippable), then selected_value_cost (soft, weight 1,
    a reward: level="hard" is refused and its level cannot change); all scored by
    default (default_enabled)."""
