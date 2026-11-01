"""Type stubs for `knapsack._core` — the knapsack C++ solver.

The problem is the planning solution: each item's `selected` (1 = selected,
0 = not, -1 = undecided, counted as not selected) is the single BASIC
planning variable.

Score (HardSoftScore, lexicographic), at the default weights: hard =
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
        """True iff hard >= 0."""

class Resource:
    """Problem fact: one additional capacity dimension (a resource_capacities entry)."""

    def __init__(self, id: str, capacity: int) -> None: ...
    id: str
    capacity: int

class Item:
    """Planning entity: one candidate item."""

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
    """The planning variable: 1 = selected, 0 = not selected, -1 = undecided."""
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
    """Planning solution. NOTE: `items` (and the other lists) element access
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
        """The planning variable per item (1 / 0 / -1)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk write of the planning variable (each value -1, 0 or 1)."""

def calculate_score(problem: KnapsackProblem) -> Score:
    """Full recompute of the HardSoftScore at the default constraints (also stores
    problem.score). Raises ValueError on invalid facts."""

def greedy_selection(problem: KnapsackProblem, fixed: list[int]) -> list[bool]:
    """The greedy construction: items by descending value / weight (ties in item
    order) while the weight fits the capacity. `fixed` holds one entry per item:
    0 / 1 kept as given (the load starts at the items fixed to 1), -1 decided by
    the greedy."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget (30 s by default). 0 returns the construction (or a
    warm-start seed) unchanged."""
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    """0 = no step limit."""
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    """Independent starts (seed + i each); the best wins. Ignored by
    solve_with_constraints."""
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
    ruinRecreate.asMoveWeight); 0.05 by default, 0 = off."""
    ruin_recreate_as_move_max_ruin: int
    change_move_weight: float
    """A change move flips one item."""
    swap_move_weight: float
    """A swap move trades a selected item for an unselected one."""
    descent_weight: float
    """BASIC best-improvement descent strategy weight. 0 = off."""
    nearby_probability: float
    """Bias swap partners toward items of similar weight with this
    probability. 0 = off (uniform-random)."""
    nearby_size: int
    best_score_limit: tuple[int, float]
    """(hard, soft): stop early once bestScore >= this. (0, -inf) stops at
    the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Greedy construction + Late Acceptance local search (+ adaptive
    ruin-and-recreate). Solves IN PLACE: sets item.selected, problem.score and
    warm_start = False."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: KnapsackProblem) -> None: ...
    def solve_with_constraints(self, problem: KnapsackProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see knapsack.constraints). The given list replaces the default set; None
        keeps the default set. Single start."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    knapsack_capacity_cost, resource_capacity_cost, mandatory_items_cost,
    forbidden_items_cost, selection_group_bounds_cost, item_dependency_cost,
    selected_value_cost. The same names knapsack.constraints uses."""

def constraint_costs(problem: KnapsackProblem) -> dict[str, dict[str, Any]]:
    """Raw cost of every default constraint, before weight/exponent/level:
    {name: {"costs": [total], "weight": float, "exponent": float, "hard": bool,
    "global": bool, "level": "hard" | "medium" | "soft", "reward": bool,
    "level_flippable": bool, "name": str, "label": str}}. Every knapsack constraint
    is global, so "costs" holds one raw total. knapsack.constraints.score() turns it
    into a score."""

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
    a reward: hard=True is refused and its level cannot change); all scored by
    default (default_enabled)."""
