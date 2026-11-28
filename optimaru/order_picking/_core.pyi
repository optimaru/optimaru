"""Type stubs for `optimaru.order_picking._core` — the order-picking C++ solver.

Each trolley's ordered `pick_task_indices` is the decision; a pick task in
no list is unassigned. `trolley_index` / `previous_pick_task` /
`next_pick_task` on a pick task are derived from the lists.

The layout is the solution's `warehouse`: a grid of shelvings of any size (the default:
5 columns x 3 shelving rows), or named locations with a walking-distance table.

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
    """A spot in the warehouse.

    On a grid: the shelving (column 0.. == A.., shelving_row 0.. == rows 1..), its side and
    the row 0..shelving_height within the shelving side. The constructor makes one of
    these; whether it is inside the grid is checked where the warehouse is known
    (`OrderPickingSolution`, `Solver.solve`).

    In a warehouse of named locations: one of them, from `Warehouse.location(id)`. It is that
    warehouse's index of the location and belongs to it: given to a solution of another
    warehouse of named locations, it is read as that one's location of the same index (or
    refused when it has none)."""

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
    def index(self) -> int:
        """The index of a named location in `Warehouse.location_ids`; -1 for a grid location."""
    @property
    def shelving_id(self) -> str:
        """The shelving id of a grid location, e.g. "(A,1)" (columns beyond Z: "(AA,1)")."""

class Warehouse:
    """The layout of a warehouse: a grid of shelvings, or named locations.

    `Warehouse()` is the default grid: 5 columns (A..E) x 3 shelving rows (1..3) of shelvings
    2 m wide and 10 m tall, 3 m of corridor between neighbors. The arguments size another
    one, in whole meters: `column_count` 1..18278, `shelving_row_count` 1..10000, and
    `shelving_width`, `shelving_height`, `shelving_padding` 1..1000 each (ValueError
    outside). A shelving side holds the rows 0..`shelving_height`.

    `Warehouse.from_distances(location_ids, distances)` is a warehouse of named locations."""

    MAX_COLUMN_COUNT: int
    """18278: columns A..ZZZ."""
    MAX_SHELVING_ROW_COUNT: int
    """10000."""
    MAX_SHELVING_SIZE: int
    """1000: the most meters of `shelving_width`, `shelving_height` and `shelving_padding`."""
    MAX_DISTANCE: int
    """1000000: the longest distance, in meters, a table of named locations may hold."""
    MAX_LOCATIONS: int
    """4096: the most named locations of a warehouse."""

    def __init__(
        self,
        column_count: int = 5,
        shelving_row_count: int = 3,
        shelving_width: int = 2,
        shelving_height: int = 10,
        shelving_padding: int = 3,
    ) -> None: ...
    @staticmethod
    def from_distances(location_ids: list[str], distances: list[list[int]]) -> Warehouse:
        """A warehouse of named locations: `distances[i][j]` is the whole meters from
        `location_ids[i]` to `location_ids[j]`.

        The table is square over the ids (at most 4096) and may be asymmetric. ValueError for
        a repeated id, a distance outside 0..1000000 or a location that is not 0 from itself."""
    @property
    def is_grid(self) -> bool:
        """True for a grid, False for named locations."""
    @property
    def column_count(self) -> int: ...
    @property
    def shelving_row_count(self) -> int: ...
    @property
    def shelving_width(self) -> int: ...
    @property
    def shelving_height(self) -> int: ...
    @property
    def shelving_padding(self) -> int: ...
    @property
    def location_ids(self) -> list[str]:
        """The ids of the named locations; empty for a grid."""
    def location(self, id: str) -> WarehouseLocation:
        """The named location with this id (ValueError: unknown id, or a grid)."""
    def location_id(self, location: WarehouseLocation) -> str:
        """What the location is called: the id of a named location, the shelving id of a grid one."""

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
        warehouse: Warehouse | None = None,
    ) -> None:
        """`warehouse` is the layout the locations are in; None is the default grid.

        ValueError when a trolley or a pick task is at a location outside it: beyond the
        grid, or a grid location in a warehouse of named locations (and the reverse)."""
    @property
    def warehouse(self) -> Warehouse: ...
    @property
    def orders(self) -> list[Order]: ...
    trolleys: list[Trolley]
    @property
    def pick_tasks(self) -> list[PickTask]: ...
    @property
    def score(self) -> Score: ...
    def routes(self) -> dict[str, list[str]]:
        """{trolley id: [pick task ids]} in pick order."""

def calculate_distance(start: WarehouseLocation, end: WarehouseLocation, warehouse: Warehouse | None = None) -> int:
    """Distance in meters between two locations of `warehouse` (None: the default grid).

    ValueError when one of them is not a location of it (outside the grid, or not one of its
    named locations)."""

def distance_to_travel(solution: OrderPickingSolution, trolley_index: int) -> int:
    """One trolley's round-trip distance.

    Like `calculate_score` and `constraint_costs`, it first checks every location against the
    solution's warehouse (ValueError), since a location can be replaced after the solution
    was made."""

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
    """The solver settings, by stage of the solve: ``stop``, ``alns_trigger``, ``custom_moves``, ``moves``, ``selection``, ``acceptance``, ``alns``, ``run``.

    Read and write a setting as ``config.<group>.<name>``, or give whole groups when making it:
    ``SolverConfig(stop={"time_limit_seconds": 10})``. See `optimaru._config`."""

    def __init__(
        self,
        *,
        stop: dict[str, object] | None = None,
        alns_trigger: dict[str, object] | None = None,
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
        unimproved_time_limit_seconds: float
        step_limit: int
        unimproved_step_limit: int
        target_score: tuple[int, float]

    stop: Stop
    """When the search stops: the first limit reached."""

    class AlnsTrigger:
        """``config.alns_trigger``: When a stalled search is kicked by ruin & recreate."""

        enabled: bool
        unimproved_steps: int

    alns_trigger: AlnsTrigger
    """When a stalled search is kicked by ruin & recreate."""

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        order_consolidate: bool

    custom_moves: CustomMoves
    """A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

    class Moves:
        """``config.moves``: A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

        relocate_weight: float
        list_swap_weight: float
        two_opt_weight: float
        or_opt_weight: float
        tail_swap_weight: float

    moves: Moves
    """A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

    class Selection:
        """``config.selection``: What the move mix moves: nearby candidates and this use case's hints."""

        distance_nearby: bool
        distance_nearby_probability: float
        distance_nearby_size: int

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(4 * pick task count, 200, 1600)."""

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
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, solution: OrderPickingSolution) -> None:
        """Solve in place: mutates pick_task_indices, refreshes the derived
        variables and stores the best score in solution.score.

        The solution is the solve's own while it runs (it also keeps its table of distances
        on it, and on a grid numbers the locations, both removed at the end): do not read,
        copy, score or solve the same solution object from another thread meanwhile."""

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
