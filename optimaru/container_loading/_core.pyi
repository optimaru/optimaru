"""Type stubs for `optimaru.container_loading._core` — the container-loading C++ solver.

The problem object holds the solution: every container's `items` (item indices in
loading order) is the single sequence-shape decision variable. Where an item stands is
derived from that order by the placement decoder and written on the item
(`container`, `sequence`, `x`, `y`, `z`, `placed_length`, `placed_width`,
`placed_height`) by `calculate_score` and by every solve.

Coordinates: x runs along a container's `length` (0 = the closed end, the door is at
x = length), y along its `width`, z up. Lengths and weights are whole numbers in the
caller's unit.

Score (hard/soft, lexicographic), at the default weights: hard = -(items that found no
place + weight above a container's max_weight + weight on an item above its
max_load_on_top + stacking rules and layer limits broken + pairs where a later item
blocks an earlier one + centre of gravity beyond its limits, in percent + pairs kept apart
by a segregation rule that are not + all-or-none groups
loaded in part + one-container groups in several), soft = -(cost of the containers in use +
optional items left out + containers a group is in beyond its first). An unplaced item counts
its priority. The weights are the C++
components' defaults (constraint_info()); the problem carries none.
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any, Literal, NamedTuple

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True when hard >= 0: no hard constraint is violated."""

class Rotation(Enum):
    """Which orientations the decoder may use for an item."""

    ALL = 0
    """The six axis-parallel orientations."""
    UPRIGHT = 1
    """The height stays vertical: as given, or turned by 90 degrees."""
    FIXED = 2
    """As given."""

class Item:
    """One box. The fields after `segregation_class` are derived: `calculate_score` and every solve write them."""

    def __init__(
        self,
        id: str,
        length: int,
        width: int,
        height: int,
        weight: int = 0,
        rotations: Rotation = ...,
        max_load_on_top: int | None = None,
        unload_order: int | None = None,
        group: str | None = None,
        bottom_only: bool = False,
        max_stack_layers: int | None = None,
        stack_group: str | None = None,
        priority: int = 1,
        optional: bool = False,
        allowed_containers: list[str] = ...,
        segregation_class: str | None = None,
    ) -> None: ...
    id: str
    length: int
    """Size along x as given, before any rotation (1 to 1,000,000)."""
    width: int
    height: int
    weight: int
    """0 to 1,000,000,000."""
    rotations: Rotation
    max_load_on_top: int | None
    """Weight this item can carry; 0 = nothing on top. None = no limit."""
    unload_order: int | None
    """Lower leaves first (a route's stop number). None = no order."""
    group: str | None
    """Items of one group belong in one container."""
    bottom_only: bool
    """Only on the floor (z = 0); other items may stand on it."""
    max_stack_layers: int | None
    """At most this many items in a column through it, itself included (>= 1). None = no limit."""
    stack_group: str | None
    """The group the problem's stacking_rules name."""
    priority: int
    """What leaving it out costs: an unplaced item counts its priority (1 to 1,000,000)."""
    optional: bool
    """Loaded where there is room: left out, it counts in optional_item_cost (soft), not in
    unplaced_item_cost, its weight does not count against max_weight, and the decoder leaves it
    out where it would put the placed weight above max_weight."""
    allowed_containers: list[str]
    """The containers it may go in, by id or tag; [] = any. In another container's list it finds
    no place."""
    segregation_class: str | None
    """The class the problem's segregation_rules name."""
    @property
    def container(self) -> int:
        """Index of the container it stands in; -1 = not placed."""
    @property
    def sequence(self) -> int:
        """0-based place among its container's placed items; -1 = not placed."""
    @property
    def x(self) -> int: ...
    @property
    def y(self) -> int: ...
    @property
    def z(self) -> int: ...
    @property
    def placed_length(self) -> int:
        """Size along x after rotation; 0 = not placed."""
    @property
    def placed_width(self) -> int: ...
    @property
    def placed_height(self) -> int: ...
    def is_placed(self) -> bool: ...

class Container:
    """One physical space: a pallet, a rack bay, a cargo space, one carton. The fields
    after `items` are derived: `calculate_score` and every solve write them."""

    def __init__(
        self,
        id: str,
        length: int,
        width: int,
        height: int,
        max_weight: int | None = None,
        cost: float = 1.0,
        items: list[int] = ...,
        max_stack_height: int | None = None,
        max_offset_length: float | None = None,
        max_offset_width: float | None = None,
        tags: list[str] = ...,
    ) -> None: ...
    id: str
    length: int
    """Inner size along x: from the closed end to the door (1 to 1,000,000)."""
    width: int
    height: int
    max_weight: int | None
    cost: float
    """What using this container costs (1 = count containers)."""
    max_stack_height: int | None
    """How high the load may reach (1 to 1,000,000; the decoder fills up to the lower of it and
    `height`). None = up to `height`."""
    max_offset_length: float | None
    """How far the placed load's centre of gravity may lie from the centre along x, as a share
    of half the length (0 to 1). None = no limit."""
    max_offset_width: float | None
    """The same across, as a share of half the width. None = no limit."""
    tags: list[str]
    """Names an item's allowed_containers may use besides the id."""
    items: list[int]
    """The decision variable: item indices in loading order."""
    @property
    def placed_items(self) -> list[int]:
        """The items of the list that found a place, in loading order."""
    @property
    def unplaced_items(self) -> list[int]:
        """The items of the list that found none."""
    @property
    def loaded_weight(self) -> int:
        """Weight of the placed items."""
    @property
    def loaded_volume(self) -> int:
        """Volume of the placed items."""
    @property
    def used_length(self) -> int:
        """Largest x + placed_length of a placed item; 0 when empty."""
    @property
    def fill_ratio(self) -> float:
        """loaded_volume / the container's volume."""

class ContainerLoadingProblem:
    """Problem and solution in one object. NOTE: `containers` and `items` element access
    returns copies; write loading orders with set_loading, or assign a whole list back.

    ValueError for what the engine refuses: a min_support_ratio outside (0, 1], a repeated
    id, a size outside 1..1,000,000, a weight outside 0..1,000,000,000, a negative limit
    or cost, a max_stack_layers outside 1..1,000,000,000, a max_stack_height outside 1..1,000,000, an offset
    limit outside 0..1, a stacking rule with an empty group, a priority outside
    1..1,000,000, a group rule for no item's group or a group ruled twice, allowed_containers
    naming no container's id or tag, a segregation rule without a class or with a distance
    outside 1..1,000,000, an item listed
    twice, or items without a container."""

    def __init__(
        self,
        containers: list[Container],
        items: list[Item],
        min_support_ratio: float = 0.8,
        stacking_rules: list[tuple[str, str]] = ...,
        group_rules: list[tuple[str, bool, bool]] = ...,
        segregation_rules: list[tuple[str, str, int | None]] = ...,
    ) -> None: ...
    containers: list[Container]
    items: list[Item]
    min_support_ratio: float
    """Share of an item's base that must rest on the floor or on other items, in (0, 1]."""
    stacking_rules: list[tuple[str, str]]
    """(top, bottom): items whose stack_group is top may not stand on items whose stack_group
    is bottom. Anything not listed is allowed."""
    group_rules: list[tuple[str, bool, bool]]
    """(group, all_or_none, single_container): load the items whose group is `group` all or
    none, and in one container."""
    segregation_rules: list[tuple[str, str, int | None]]
    """(a, b, distance): items of classes a and b in different containers (distance None), or
    at least `distance` apart in the floor plane."""
    @property
    def score(self) -> Score: ...
    def loading(self) -> list[list[int]]:
        """The decision: per container, the item indices in loading order."""
    def set_loading(self, loading: list[list[int]]) -> None:
        """Bulk write of the decision: one list of item indices per container. ValueError
        for a wrong number of lists, an index out of range or an item listed twice."""
    def containers_used(self) -> int:
        """Containers that hold a placed item, as of the last calculate_score / solve."""
    def unplaced_items(self) -> list[int]:
        """Indices of the items that are not placed (in a list without a place, or in no
        list), as of the last calculate_score / solve."""

def calculate_score(problem: ContainerLoadingProblem) -> Score:
    """Decode every container, write where each item stands and recompute the score at the
    default weights (also stores problem.score). ValueError for a problem the engine
    refuses (see ContainerLoadingProblem). An item in no container's list is reported by
    `unplaced_items()` but costs nothing (solve lists every item before it scores)."""

def update_placements(problem: ContainerLoadingProblem) -> None:
    """Decode every container and write where each item stands (the derived fields of the
    items and containers), leaving problem.score as it is. `calculate_score` does this and
    scores at the default weights; a solve leaves both current."""

def generate_demo_data(
    item_count: int = 40, container_count: int = 4, random_seed: int = 37
) -> ContainerLoadingProblem:
    """The demo: boxes of six kinds (mm and g; one kind fragile) for pallets of
    1200 x 800 x 1500 mm that carry 600 kg each; the boxes go to three stores in turn (the
    groups "store-1" to "store-3").
    Deterministic for a given random_seed."""

class SolverConfig:
    """The solver settings, by stage of the solve: ``construction``, ``stop``, ``alns_trigger``, ``custom_moves``, ``moves``, ``acceptance``, ``alns``, ``run``.

    Read and write a setting as ``config.<group>.<name>``, or give whole groups when making it:
    ``SolverConfig(stop={"time_limit_seconds": 10})``. See `optimaru._config`."""

    def __init__(
        self,
        *,
        construction: dict[str, object] | None = None,
        stop: dict[str, object] | None = None,
        alns_trigger: dict[str, object] | None = None,
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

    class Construction:
        """``config.construction``: How the first solution is built."""

        first_fit_decreasing: bool
        """True (default): the items no list holds go group by group and largest first
        (those that leave last before the others, the bottom_only ones before the rest,
        those with a max_load_on_top after them), each into the first container that takes
        it. False: the engine's
        cheapest insertion places them."""

    construction: Construction
    """How the first solution is built."""

    class Stop:
        """``config.stop``: When the search stops: the first limit reached."""

        time_limit_seconds: float
        """Wall-clock budget (30 s). 0 = no search: the construction comes back."""
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

    class CustomMoves:
        """``config.custom_moves``: A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

        empty_container: bool
        """Enable EmptyContainerMove: it moves the items of one of the least filled
        containers into the others in a single step, the only way past the
        containers-used step function. An item moves only where the decoder still
        places it and everything placed before, within the weight limit, and without
        breaking a stacking limit, a stacking rule or the unloading order."""
        empty_container_candidates: int
        """EmptyContainerMove empties one of this many least filled containers in use
        (3; 1 = always the least filled). ValueError below 1."""
        repack: bool
        """Enable RepackPairMove: the items of two containers in use are loaded again
        (into those two and, every second time, an empty one as a third), dealt out
        group by group, a group whole wherever a container has room for it, and each
        container's share in the construction's loading order. It gathers a group two
        containers share and repacks loading orders that grew by moves; it is made
        only when every load comes out clean."""
        switch_container: bool
        """Enable SwitchContainerMove: the load of one container goes into empty
        containers of another size or cost that cost no more, as it is into one or
        split over several. Used only when the containers differ; it is how a load
        reaches a cheaper carton."""
        stop_order: bool
        """Enable StopOrderMove (registered only when at least two items set an
        unload_order, also for a single container): a container's loading order is
        put back in stop blocks, the latest stop first (items without an order keep
        their places), with one change inside one stop's block: an item moved or two
        swapped, or the block ordered afresh by volume, height, base area, depth or
        at random."""

    custom_moves: CustomMoves
    """A step's move, next: this use case's own moves, chosen adaptively on their share of steps."""

    class Moves:
        """``config.moves``: A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

        relocate_weight: float
        list_swap_weight: float
        two_opt_weight: float
        or_opt_weight: float

    moves: Moves
    """A step's move, otherwise: the move mix, by relative weight (0 turns a move off)."""

    class Selection:
        """``config.selection``: What the move mix moves: nearby candidates and this use case's hints."""

        nearby_probability: float
        """Used only when at least half the items set an unload_order (a vehicle
        load): how often the move mix moves an item next to one of its nearby items,
        those of the same stop and the stops next to it (then the closest in volume),
        which keeps the stops in blocks; a ruin-and-recreate kick then tries only the
        places next to them (0.8). 0 disables nearby selection."""
        nearby_size: int
        """Size of the nearby neighbor list (20). Used, like nearby_probability, only
        when at least half the items set an unload_order."""

    selection: Selection
    """What the move mix moves: nearby candidates and this use case's hints."""

    class Acceptance:
        """``config.acceptance``: Whether the candidate becomes the current solution."""

        late_acceptance_size: int
        """0 = auto: clamp(4 * item count, 200, 1600)."""

    acceptance: Acceptance
    """Whether the candidate becomes the current solution."""

    class Alns:
        """``config.alns``: The ruin & recreate kick itself."""

        min_ruin_fraction: float
        max_ruin_fraction: float
        max_ruin_count: int
        """12 here: every place a ruined item is tried at is a decode."""
        polish_steps: int
        """1000 here."""
        operator_decay: float
        rewards: list[float]

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
    """First-fit-decreasing construction by decode + Late Acceptance local search over
    the loading orders (+ EmptyContainerMove, RepackPairMove, SwitchContainerMove,
    StopOrderMove and adaptive ruin-and-recreate). Solves IN PLACE: sets every container's `items`, the derived
    fields and problem.score. The lists the problem comes with are kept as the starting
    point."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, problem: ContainerLoadingProblem) -> None: ...
    def solve_with_constraints(self, problem: ContainerLoadingProblem, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.container_loading.constraints). The given list replaces the default
        set; None keeps the default set. Single start. A name may be picked several
        times: each pick is its own instance with its own knobs, and the instances add
        up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    unplaced_item_cost, container_weight_cost, stacking_load_cost, stacking_rule_cost,
    unloading_order_cost, balance_limit_cost, segregation_cost, used_container_cost,
    optional_item_cost,
    used_length_cost, load_balance_cost, group_split_cost, incomplete_group_cost,
    single_container_cost.
    The same names optimaru.container_loading.constraints uses."""

def constraint_costs(
    problem: ContainerLoadingProblem, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str}}. The eleven per-container
    constraints list one cost per container, an empty one included (the scoped
    stacking_rule_cost, balance_limit_cost, segregation_cost and optional_item_cost also name the containers
    they evaluate, `units`); the three globals (group_split_cost, incomplete_group_cost,
    single_container_cost) list their one total. None = the default constraints; a pick list (as for
    solve) scores exactly those picks, each entry carrying its pick's knobs, the k-th
    pick of a name keyed "<name>#<k>" from the second on.
    optimaru.container_loading.constraints.score() turns it into a score. Raises
    ValueError for the same facts as calculate_score."""

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
    unplaced_item_cost, container_weight_cost, stacking_load_cost, stacking_rule_cost,
    unloading_order_cost, balance_limit_cost and segregation_cost (hard, 1), used_container_cost and
    optional_item_cost (soft, 1), used_length_cost and
    load_balance_cost (soft, 0: opt-in, default_enabled False), group_split_cost
    (soft, 1, global), incomplete_group_cost and single_container_cost (hard, 1, global);
    all flippable, none a reward."""
