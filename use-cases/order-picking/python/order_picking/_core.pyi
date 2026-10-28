"""Type stubs for `order_picking._core` — the order-picking C++ solver.

The solution is the planning solution: each trolley's ordered
`pick_task_indices` is the planning list variable; a pick task in no list is
unassigned. `trolley_index` / `previous_pick_task` / `next_pick_task` on a
pick task are the shadow variables derived from the lists.

Score (HardSoft, lexicographic): hard = -(missing trolley buckets),
soft = -(travel distance in meters + 1000 * order-split count).
"""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True iff hard == 0."""

class Side:
    """Shelving side (Shelving.Side): LEFT or RIGHT."""

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
        """The Java shelving id, e.g. "(A,1)"."""

class Order:
    def __init__(self, id: str) -> None: ...
    id: str

class PickTask:
    """Planning entity (list element): the picking of one order item."""

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
    """Shadow: index of the owning trolley; -1 == unassigned."""
    previous_pick_task: int
    """Shadow: previous pick in the trolley's list; -1 == none."""
    next_pick_task: int
    """Shadow: next pick in the trolley's list; -1 == none."""
    def is_last(self) -> bool:
        """True iff next_pick_task == -1 (the Java PickTask.isLast)."""

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
    """The planning list variable: ordered indices into pick_tasks."""

class OrderPickingSolution:
    """Planning solution."""

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

class ConstraintTotals:
    """Per-constraint penalty totals (positive), one per Java constraint."""

    @property
    def required_number_of_buckets(self) -> int: ...
    @property
    def distance_from_previous_pick_task(self) -> int: ...
    @property
    def distance_from_last_pick_task_to_path_origin(self) -> int: ...
    @property
    def order_split_by_trolley(self) -> int: ...
    def to_score(self) -> Score: ...

def calculate_distance(start: WarehouseLocation, end: WarehouseLocation) -> int:
    """Distance in meters between two locations (Warehouse.calculateDistance)."""

def distance_to_travel(solution: OrderPickingSolution, trolley_index: int) -> int:
    """One trolley's round-trip distance (Warehouse.calculateDistanceToTravel)."""

def update_shadow_variables(solution: OrderPickingSolution) -> None:
    """Re-derive the pick tasks' shadow variables from the pick lists."""

def calculate_score(solution: OrderPickingSolution) -> Score:
    """Refresh shadows, recompute and store solution.score."""

def calculate_totals(solution: OrderPickingSolution) -> ConstraintTotals:
    """Per-constraint penalty totals on the current assignment."""

def build_demo_data(
    trolleys_count: int = 5,
    bucket_count: int = 4,
    bucket_capacity: int = 48000,
    orders_count: int = 8,
    random_seed: int = 37,
) -> OrderPickingSolution:
    """The Java quickstart's DemoDataGenerator dataset (bit-exact replay)."""

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
        """Solve in place: mutates pick_task_indices, refreshes the shadow
        variables and stores the best score in solution.score."""

    def solve_with_constraints(self, solution: OrderPickingSolution, constraints: Iterable[Any] | None = ...) -> None:
        """Same solve, scored by a caller-chosen constraint set (see
        `order_picking.constraints`). The picks replace the defaults; None
        keeps them."""

def list_constraints() -> list[str]:
    """The constraint names this domain scores, in default-assembly order
    (snake_case) — the names `order_picking.constraints` uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `order_picking.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
