"""Type stubs for `optimaru.order_picking._core` — the order-picking C++ solver.

Each trolley's ordered `pick_task_indices` is the decision; a pick task in
no list is unassigned. `trolley_index` / `previous_pick_task` /
`next_pick_task` on a pick task are derived from the lists.

Score (hard / soft, lexicographic), at the default weights:
  hard = -(missing trolley buckets)                 # trolley_capacity_cost
  soft = -(walk to and between the picks, m         # distance_from_previous_pick_task_cost
           + walk back to the trolley origin, m     # distance_from_last_pick_task_to_path_origin_cost
           + 1000 x (order, trolley) pairs)         # order_split_by_trolley_cost
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

class Side:
    """Shelving side: LEFT or RIGHT."""

    LEFT: Side
    RIGHT: Side

class WarehouseLocation:
    """A spot in the warehouse: shelving (column 0..4 == A..E, shelving_row
    0..2 == rows 1..3), side and the row 0..10 within the shelving side."""

    def __init__(self, column: int, shelving_row: int, side: Side, row: int) -> None: ...
    @property
    def column(self) -> int: ...
    @property
    def shelving_row(self) -> int: ...
    @property
    def side(self) -> Side: ...
    @property
    def row(self) -> int: ...
    @property
    def shelving_id(self) -> str:
        """The shelving id, e.g. "(A,1)"."""

class Order:
    def __init__(self, id: str) -> None: ...
    id: str

class PickTask:
    """The picking of one order item: an element of a trolley's pick list."""

    def __init__(
        self,
        id: str,
        order_index: int,
        volume: int,
        location: WarehouseLocation,
        product_name: str = "",
    ) -> None: ...
    id: str
    order_index: int
    product_name: str
    volume: int
    location: WarehouseLocation
    trolley_index: int
    """Derived: index of the owning trolley; -1 == unassigned."""
    previous_pick_task: int
    """Derived: previous pick in the trolley's list; -1 == none."""
    next_pick_task: int
    """Derived: next pick in the trolley's list; -1 == none."""
    def is_last(self) -> bool:
        """True iff next_pick_task == -1."""

class Trolley:
    def __init__(
        self,
        id: str,
        bucket_count: int,
        bucket_capacity: int,
        location: WarehouseLocation,
    ) -> None: ...
    id: str
    bucket_count: int
    bucket_capacity: int
    location: WarehouseLocation
    pick_task_indices: list[int]
    """The decision: ordered indices into pick_tasks."""

class OrderPickingSolution:
    """The problem data plus the trolleys' pick lists and the score."""

    def __init__(
        self,
        orders: list[Order],
        trolleys: list[Trolley],
        pick_tasks: list[PickTask],
    ) -> None: ...
    @property
    def orders(self) -> list[Order]: ...
    trolleys: list[Trolley]
    @property
    def pick_tasks(self) -> list[PickTask]: ...
    @property
    def score(self) -> Score: ...
    def routes(self) -> dict[str, list[str]]:
        """{trolley id: [pick task ids]} in pick order."""

def calculate_distance(start: WarehouseLocation, end: WarehouseLocation) -> int:
    """Distance in meters between two locations."""

def distance_to_travel(solution: OrderPickingSolution, trolley_index: int) -> int:
    """One trolley's round-trip distance."""

def update_shadow_variables(solution: OrderPickingSolution) -> None:
    """Re-derive the pick tasks' derived fields from the pick lists."""

def calculate_score(solution: OrderPickingSolution) -> Score:
    """Refresh the derived fields, recompute and store solution.score."""

def generate_demo_data(
    trolleys_count: int = 5,
    bucket_count: int = 4,
    bucket_capacity: int = 48000,
    orders_count: int = 8,
    random_seed: int = 37,
) -> OrderPickingSolution:
    """The demo warehouse (deterministic for a given random_seed)."""

build_demo_data = generate_demo_data  # the name before 0.5.1

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = auto: clamp(4 * pick task count, 200, 1600)."""
    full_assert: bool
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    relocate_move_weight: float
    list_swap_move_weight: float
    two_opt_move_weight: float
    or_opt_move_weight: float
    tail_swap_move_weight: float
    distance_nearby: bool
    distance_nearby_probability: float
    distance_nearby_size: int
    order_consolidate_moves: bool
    best_score_limit: tuple[int, float]
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, solution: OrderPickingSolution) -> None:
        """Solve in place: mutates pick_task_indices, refreshes the derived
        variables and stores the best score in solution.score."""

    def solve_with_constraints(self, solution: OrderPickingSolution, constraints: Iterable[Any] | None = ...) -> None:
        """Same solve, scored by a caller-chosen constraint set (see
        `optimaru.order_picking.constraints`). The picks replace the defaults; None
        keeps them. A name may be picked several times: each pick is its own
        instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    trolley_capacity_cost, distance_from_previous_pick_task_cost,
    distance_from_last_pick_task_to_path_origin_cost, order_split_by_trolley_cost.
    The same names `optimaru.order_picking.constraints` uses."""

def constraint_costs(
    solution: OrderPickingSolution, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw per-trolley cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}, one cost per trolley (every
    trolley is evaluated, an empty one included). None = the default constraints; a
    pick list (as for solve) scores exactly those picks, each entry carrying its pick's
    knobs, the k-th pick of a name keyed "<name>#<k>" from the second on.
    `optimaru.order_picking.constraints.score()` turns it into a score. The only
    per-constraint surface: the travel distance is the sum of the two distance
    entries' costs, the (order, trolley) pair count the sum of
    order_split_by_trolley_cost's."""

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
    trolley_capacity_cost (hard, 1), distance_from_previous_pick_task_cost (soft, 1),
    distance_from_last_pick_task_to_path_origin_cost (soft, 1),
    order_split_by_trolley_cost (soft, 1000); all flippable, none a reward, all
    scored by default (default_enabled)."""
