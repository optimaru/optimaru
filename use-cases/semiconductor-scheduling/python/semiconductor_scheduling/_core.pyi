"""Type stubs for `semiconductor_scheduling._core` — the semiconductor fab C++ solver.

The schedule is the planning solution. Its search state is one BASIC planning value per
operation row (``rank * K + slot``: a decode priority and a machine option), which the
serial schedule-generation decoder turns into ``assignments``. Facts are indices into the
schedule's lists (machines, auxiliary resources, lots, a lot's operations).

Score (HardSoftScore, lexicographic), at the default weights: hard = -(the 14 hard
constraints' raw costs), soft = -(preference + min batch size + load imbalance +
10 * weighted tardiness + cycle time + makespan). The weights are the C++ components'
defaults (constraint_info()); the schedule carries none.
"""

from collections.abc import Callable, Iterable, Iterator
from typing import Any, Literal, NamedTuple

class Score:
    """(hard, soft), compared lexicographically; also iterable and indexable as a pair."""

    def __init__(self, hard: int = 0, soft: float = 0.0) -> None: ...
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True iff hard >= 0."""
    def __len__(self) -> int: ...
    def __iter__(self) -> Iterator[int | float]: ...
    def __getitem__(self, index: int) -> int | float: ...
    def __lt__(self, other: object) -> bool: ...
    def __le__(self, other: object) -> bool: ...
    def __gt__(self, other: object) -> bool: ...
    def __ge__(self, other: object) -> bool: ...

class TimeWindow:
    """[start, end), end > start >= 0."""

    def __init__(self, start: int, end: int) -> None: ...
    @property
    def start(self) -> int: ...
    @property
    def end(self) -> int: ...

class Machine:
    def __init__(
        self,
        id: str,
        group: str,
        batch_capacity: int = 1,
        minimum_batch_size: int = 1,
        unavailable: list[TimeWindow] = ...,
    ) -> None: ...
    @property
    def id(self) -> str: ...
    @property
    def group(self) -> str: ...
    @property
    def batch_capacity(self) -> int: ...
    @property
    def minimum_batch_size(self) -> int: ...
    @property
    def unavailable(self) -> list[TimeWindow]: ...

class AuxiliaryResource:
    def __init__(self, id: str, capacity: int = 1, unavailable: list[TimeWindow] = ...) -> None: ...
    @property
    def id(self) -> str: ...
    @property
    def capacity(self) -> int: ...
    @property
    def unavailable(self) -> list[TimeWindow]: ...

class MachineOption:
    def __init__(self, machine: int, duration: int, preference_cost: int = 0) -> None: ...
    @property
    def machine(self) -> int: ...
    @property
    def duration(self) -> int: ...
    @property
    def preference_cost(self) -> int: ...

class Operation:
    def __init__(
        self,
        id: str,
        family: str,
        eligible_machines: list[MachineOption],
        required_resources: list[int] = ...,
        min_queue_time: int = 0,
        max_queue_time: int | None = None,
    ) -> None: ...
    @property
    def id(self) -> str: ...
    @property
    def family(self) -> str: ...
    @property
    def eligible_machines(self) -> list[MachineOption]: ...
    @property
    def required_resources(self) -> list[int]:
        """Indices into auxiliary_resources; a resource listed twice is used twice."""
    @property
    def min_queue_time(self) -> int: ...
    @property
    def max_queue_time(self) -> int | None: ...

class Lot:
    def __init__(
        self,
        id: str,
        product: str,
        release_time: int,
        due_time: int,
        priority: int = 1,
        max_cycle_time: int | None = None,
        *,
        operations: list[Operation],
    ) -> None: ...
    @property
    def id(self) -> str: ...
    @property
    def product(self) -> str: ...
    @property
    def release_time(self) -> int: ...
    @property
    def due_time(self) -> int: ...
    @property
    def priority(self) -> int: ...
    @property
    def max_cycle_time(self) -> int | None: ...
    @property
    def operations(self) -> list[Operation]: ...

class SetupTime:
    def __init__(self, machine_group: str, from_family: str, to_family: str, duration: int) -> None: ...
    @property
    def machine_group(self) -> str: ...
    @property
    def from_family(self) -> str: ...
    @property
    def to_family(self) -> str: ...
    @property
    def duration(self) -> int: ...

class TransportTime:
    def __init__(self, from_group: str, to_group: str, duration: int) -> None: ...
    @property
    def from_group(self) -> str: ...
    @property
    def to_group(self) -> str: ...
    @property
    def duration(self) -> int: ...

class Assignment:
    """One operation (lot index, route position) on a machine index, [start, end)."""

    def __init__(
        self, lot: int, operation: int, machine: int, start: int, end: int, batch_id: str | None = None
    ) -> None: ...
    @property
    def lot(self) -> int: ...
    @property
    def operation(self) -> int: ...
    @property
    def machine(self) -> int: ...
    @property
    def start(self) -> int: ...
    @property
    def end(self) -> int: ...
    @property
    def batch_id(self) -> str | None: ...

class SemiconductorSchedule:
    """Planning solution. The constructor validates (ValueError) like the Pydantic model.
    Facts are read-only copies; `assignments` may be replaced (validated; clears the score)."""

    def __init__(
        self,
        machines: list[Machine],
        auxiliary_resources: list[AuxiliaryResource] = ...,
        setup_times: list[SetupTime] = ...,
        transport_times: list[TransportTime] = ...,
        *,
        lots: list[Lot],
        pinned_assignments: list[Assignment] = ...,
        assignments: list[Assignment] = ...,
        warm_start: bool = False,
    ) -> None: ...
    @property
    def machines(self) -> list[Machine]: ...
    @property
    def auxiliary_resources(self) -> list[AuxiliaryResource]: ...
    @property
    def setup_times(self) -> list[SetupTime]: ...
    @property
    def transport_times(self) -> list[TransportTime]: ...
    @property
    def lots(self) -> list[Lot]: ...
    @property
    def pinned_assignments(self) -> list[Assignment]: ...
    assignments: list[Assignment]
    warm_start: bool
    """Start the next solve from `assignments`; reset to False by the solve."""
    @property
    def makespan(self) -> int: ...
    @property
    def total_weighted_tardiness(self) -> int: ...
    @property
    def total_cycle_time(self) -> int: ...
    @property
    def score(self) -> Score: ...
    @property
    def scored(self) -> bool:
        """False until calculate_score or a solve set score / makespan / totals."""
    def row_count(self) -> int: ...
    @property
    def max_option_count(self) -> int: ...

def calculate_score(schedule: SemiconductorSchedule, constraints: Iterable[Any] | None = None) -> Score:
    """Score schedule.assignments as given (None = the default constraints, else the picks)
    and store score, makespan, total_weighted_tardiness and total_cycle_time."""

class AcceptorType:
    HILL_CLIMBING: AcceptorType
    LATE_ACCEPTANCE: AcceptorType
    ENTITY_TABU: AcceptorType
    SIMULATED_ANNEALING: AcceptorType
    GREAT_DELUGE: AcceptorType
    STEP_COUNTING_HILL_CLIMBING: AcceptorType
    RECORD_TO_RECORD_TRAVEL: AcceptorType
    @property
    def name(self) -> str: ...
    @property
    def value(self) -> int: ...

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget (30 s); 0 returns the start as is."""
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    """0 = no limit."""
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    """Independent starts, one thread each (seed + i); the strictly best wins. The
    constraints path runs one start."""
    full_assert: bool
    acceptor_type: AcceptorType
    """SIMULATED_ANNEALING by default."""
    simulated_annealing_starting_temperature: tuple[int, float]
    """(0, 0) = auto: hard 0, soft = simulated_annealing_temperature_fraction x |start soft|."""
    simulated_annealing_temperature_fraction: float
    """0.01."""
    late_acceptance_size: int
    """LATE_ACCEPTANCE: 0 = auto: clamp(16 * operation rows, 400, 3200)."""
    change_move_weight: float
    swap_move_weight: float
    """The engine's whole-value swap; off (0) by default."""
    machine_move_probability: float
    """Share of change moves that switch a row's machine (the rest shift its priority)."""
    priority_swap_moves: bool
    """Swap the priorities of two operations (machines kept)."""
    custom_move_probability: float
    """Gate on how often a step may draw the priority swap."""
    ruin_recreate: bool
    """Off: a recreate evaluates every value of every ruined row, each one a decode."""
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    best_score_limit: tuple[int, float]
    """(hard, soft): stop once bestScore >= this; (0, -1e18) stops at the first feasible."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    """Simulated annealing over the decoded search state. Solves IN PLACE: assignments, score,
    makespan, totals; warm_start = False."""

    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: SemiconductorSchedule) -> None: ...
    def solve_with_constraints(self, schedule: SemiconductorSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """The same search scored by a caller-chosen constraint set (single start); the
        list replaces the defaults, None keeps them."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case): the
    names semiconductor_scheduling.constraints uses."""

def constraint_costs(schedule: SemiconductorSchedule) -> dict[str, dict[str, Any]]:
    """Raw cost of every default constraint on schedule.assignments, before
    weight/exponent/level: {name: {"costs": [total], "weight": float, "exponent": float,
    "hard": bool, "global": True, "level": "hard" | "soft", "reward": bool,
    "level_flippable": bool, "name": str, "label": str}}. Every constraint is global, so
    "costs" holds one element. semiconductor_scheduling.constraints.score() turns it into a
    score."""

def constraint_items(schedule: SemiconductorSchedule) -> dict[str, list[int]]:
    """Per-item raw costs (0.1.x lists: one item per lot, route pair, machine, window, ...)."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the first 14 hard at weight 1,
    then machine_preference_cost, min_batch_size_cost, machine_load_imbalance_cost (soft,
    1), weighted_tardiness_cost (soft, 10), cycle_time_cost and makespan_cost (soft, 1); all
    flippable, none a reward, all scored by default (default_enabled)."""

def default_choices(schedule: SemiconductorSchedule) -> list[int]:
    """Per operation row, the option with the least (preference cost, duration)."""

def seed_state(schedule: SemiconductorSchedule, choices: list[int]) -> tuple[list[int], list[int]]:
    """schedule.assignments as a (choices, ranks) state: a seeded row takes the LAST option
    with its machine and duration; ranks follow (unseeded last, seeded start, row)."""

def decode(schedule: SemiconductorSchedule, choices: list[int], ranks: list[int]) -> list[Assignment]:
    """The serial schedule-generation decoder; choices are used modulo the option count."""
