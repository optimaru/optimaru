"""Typed stubs for the pybind11 extension food_packaging._core.

Score convention (HardMediumSoftScore, as in the Java quickstart; all penalties), at
the default weights (the C++ components' kDefaultWeight, see constraint_info()):
  hard   1. max_end_time_cost                minutes past Job.max_end_time, x -1
  hard   2. operator_cleaning_conflict_cost  overlapping cleaning minutes on lines of one
                                             operator, x -1 (global)
  medium 3. after_ideal_end_time_cost        minutes past Job.ideal_end_time, x -1
                                             (medium units)
  medium 4. unassigned_jobs_cost             duration minutes of jobs on no line, x -1
                                             (medium units, global)
  soft   5. squared_makespan_cost            (end of a line's last job - line start)^2,
                                             x -1

Internally the engine uses HardSoftScore(int64 hard, double soft), mapped as
`engine.soft = medium * 10**10 + soft` (the domain's medium fold, "into_soft" with unit
10**10; level priority holds while |soft| < 10**10 and |medium| < 9 * 10**5; see
cpp/include/fp/domain.hpp). Medium weights are in medium units: the two medium
constraints' default weight is 1, not 10**10.
"""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple

class Score:
    def __init__(self, hard: int = 0, medium: int = 0, soft: int = 0) -> None: ...
    @property
    def hard(self) -> int: ...
    @property
    def medium(self) -> int: ...
    @property
    def soft(self) -> int: ...
    def is_feasible(self) -> bool: ...
    def __eq__(self, other: object) -> bool: ...

class Product:
    id: str
    name: str
    def __init__(self, id: str, name: str) -> None: ...

class Operator:
    id: str
    def __init__(self, id: str) -> None: ...

class Job:
    id: str
    name: str
    product_index: int
    duration_minutes: int
    min_start_minute: int
    ideal_end_minute: int
    max_end_minute: int
    priority: int
    pinned: bool
    def __init__(
        self,
        id: str,
        name: str,
        product_index: int,
        duration_minutes: int,
        min_start_minute: int,
        ideal_end_minute: int,
        max_end_minute: int,
        priority: int = 1,
        pinned: bool = False,
    ) -> None: ...
    @property
    def line_index(self) -> int: ...
    @property
    def start_cleaning_minute(self) -> int | None: ...
    @property
    def start_production_minute(self) -> int | None: ...
    @property
    def end_minute(self) -> int | None: ...

class Line:
    id: str
    name: str
    start_minute: int
    operator_index: int
    jobs: list[int]
    def __init__(
        self,
        id: str,
        name: str,
        start_minute: int,
        operator_index: int = -1,
        jobs: list[int] = ...,
    ) -> None: ...

class PackagingSchedule:
    from_minute: int
    to_minute: int
    products: list[Product]
    cleanup_minutes: list[int]
    operators: list[Operator]
    lines: list[Line]
    jobs: list[Job]
    unassigned_jobs: list[int]
    def __init__(
        self,
        from_minute: int,
        to_minute: int,
        products: list[Product],
        cleanup_minutes: list[int],
        operators: list[Operator],
        lines: list[Line],
        jobs: list[Job],
        unassigned_jobs: list[int] = ...,
    ) -> None: ...
    @property
    def score(self) -> Score: ...
    def line_count(self) -> int: ...
    def cleanup_of(self, previous_product: int, product: int) -> int: ...
    def update_shadow_variables(self) -> None: ...

def calculate_score(schedule: PackagingSchedule) -> Score: ...
def assign_missing_operators(schedule: PackagingSchedule) -> None: ...
def improve_operator_assignment(schedule: PackagingSchedule) -> bool: ...
def generate_demo_data(line_count: int = 5, job_count: int = 100) -> PackagingSchedule: ...

class SolverConfig:
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    full_assert: bool
    operator_rounds: int
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
    best_score_limit: tuple[int, float]
    on_new_best: Callable[[int, Score], None] | None
    """Progress callback (steps, best score). The score has the same three levels as the
    final schedule.score: with solve_with_constraints both come from the chosen
    constraints' hard / medium / soft totals, so a soft total of 10^10 or more stays on
    soft instead of spilling into medium."""
    def __init__(self) -> None: ...

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: PackagingSchedule) -> None: ...
    def solve_with_constraints(self, schedule: PackagingSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see food_packaging.constraints). The given list replaces the default set; None
        keeps the default set. A name may be picked several times: each pick is its own
        instance with its own knobs, and the instances add up. schedule.score reports the
        three levels the assembly put the terms on (a medium constraint with hard=False
        lands on soft)."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case:
    the unit constraints, then the globals operator_cleaning_conflict_cost and
    unassigned_jobs_cost). The same names food_packaging.constraints uses."""

def constraint_costs(
    schedule: PackagingSchedule, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw per-unit cost of every constraint, before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "hard": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str, "units": [...],
    "medium_fold": {"kind": "into_soft", "unit": 1e10}}}. Units are the lists (the
    packaging lines in order, then the unassigned pool); a global constraint has one
    entry and no "units". A per-line constraint's "units" are the lines it evaluates
    (every line, never the pool), where a literal offset (clip=False) charges a raw 0.
    None = the default constraints; a pick list (as for solve) scores exactly those
    picks, each entry carrying its pick's knobs, the k-th pick of a name keyed
    "<name>#<k>" from the second on. `medium_fold` is on EVERY entry (the fold belongs
    to the domain), and `food_packaging.constraints.score()` applies it. The only
    per-constraint surface since 0.2.0 (analyze() / ConstraintBreakdown were removed)."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: each component's default
    level and weight (in units of that level; after_ideal_end_time_cost and
    unassigned_jobs_cost are medium, weight 1), whether its level may be flipped,
    whether it is a reward, and whether it is scored when no constraints are given."""
