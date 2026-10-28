"""Type stubs for `project_job._core` — the project-job-scheduling C++ solver.

The compiled module wraps the C++ domain in `use-cases/project-job-scheduling/cpp`,
which imports the generic engine in `include/optimaru`.

The only planning variable is `Allocation.mode_delay` (BASIC int) — a composite
encoding of the Java quickstart's two variables (executionMode, delay∈[0, delay_range_size))::

    -1                                  uninitialized (no resource use;
                                        dates pass through with delay 0/duration 0)
    delay * max_mode_count + mode_slot  real mode index = mode_slot %
                                        the job's mode count — every value of
                                        the global range is valid for every
                                        job (preserving Java's per-entity
                                        value range semantics)

Java's shadow variable chain (predecessorsDoneDate → startDate → endDate) is
derived state: `calculate_score` / `Solver.solve` recompute and fill it in
topological order.

Score convention (HardMediumSoftScore, as in the Java quickstart; all penalties):
  hard 1.  nonRenewableResourceCapacity  total use per non-renewable resource
                                         above capacity: -(excess)
  hard 2.  renewableResourceCapacity     use per renewable resource x day
                                         above capacity: -(excess)
  medium 3. totalProjectDelay            SINK end date - critical path end date
                                         (positive only): -(sum of delays)
  soft 4.  totalMakespan                 -(latest SINK end date)

Internally the engine uses HardSoftScore(int64 hard, double soft), mapped as
`engine.soft = medium * 10**9 + soft` (level priority holds while |soft| < 10**9;
see cpp/include/pjs/domain.hpp for the exact rules).
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any

class Score:
    """HardMediumSoft score. hard 0 == feasible. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def medium(self) -> int: ...
    @property
    def soft(self) -> int: ...
    def is_feasible(self) -> bool:
        """True if hard >= 0."""

class JobType(Enum):
    SOURCE = ...
    STANDARD = ...
    SINK = ...

class Project:
    """Problem fact: a project. release_date is only the reference of the 'Total project
    delay' constraint (it does not force the start — as in the Java quickstart)."""
    def __init__(self, id: str, release_date: int = 0, critical_path_duration: int = 0) -> None: ...
    id: str
    release_date: int
    critical_path_duration: int
    def critical_path_end_date(self) -> int:
        """release_date + critical_path_duration."""

class Resource:
    """Problem fact: a resource. Renewable capacity is per day, otherwise for the whole
    horizon. `project` is an index into ProjectJobSchedule.projects, -1 == global
    (informational — constraints only look at capacity/renewable)."""
    def __init__(self, id: str, capacity: int, renewable: bool = True, project: int = -1) -> None: ...
    id: str
    capacity: int
    renewable: bool
    project: int

class ResourceRequirement:
    """Resource demand of an execution mode. `resource` is an index into
    ProjectJobSchedule.resources."""
    def __init__(self, resource: int, requirement: int) -> None: ...
    resource: int
    requirement: int

class ExecutionMode:
    """One execution mode of a job (duration + resource demand)."""
    def __init__(self, id: str, duration: int = 0, resource_requirements: list[ResourceRequirement] = ...) -> None: ...
    id: str
    duration: int
    resource_requirements: list[ResourceRequirement]

class Job:
    """Problem fact: a job. `project` is an index; every job needs at least one
    execution mode (SOURCE/SINK have one zero-duration mode — the Java quickstart convention)."""
    def __init__(
        self, id: str, project: int, job_type: JobType = ..., execution_modes: list[ExecutionMode] = ...
    ) -> None: ...
    id: str
    project: int
    job_type: JobType
    execution_modes: list[ExecutionMode]

class Allocation:
    """Planning entity: the allocation of one job (1:1 with jobs). `mode_delay` is the
    planning variable (see the encoding in the module docstring), written by the solver.
    `predecessor_allocations` are predecessor allocation indices (must be acyclic).
    SOURCE/SINK allocations are always pinned; `pinned` also fixes a STANDARD one."""
    def __init__(
        self,
        id: str,
        job: int,
        predecessor_allocations: list[int] = ...,
        mode_delay: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: str
    job: int
    predecessor_allocations: list[int]
    mode_delay: int
    pinned: bool
    @property
    def predecessors_done_date(self) -> int: ...
    @property
    def start_date(self) -> int: ...
    @property
    def end_date(self) -> int: ...

class ProjectJobSchedule:
    """Planning solution. The constructor validates the instance (index ranges, jobs
    without modes, predecessor graph cycles -> ValueError). solve changes the allocations'
    `mode_delay`, the derived dates and `score` in place.

    Note: list attributes such as `allocations` return copies, so use
    `set_assignment` to change assignments."""
    def __init__(
        self,
        projects: list[Project],
        resources: list[Resource],
        jobs: list[Job],
        allocations: list[Allocation],
        delay_range_size: int = 500,
    ) -> None: ...
    @property
    def projects(self) -> list[Project]: ...
    @property
    def resources(self) -> list[Resource]: ...
    @property
    def jobs(self) -> list[Job]: ...
    @property
    def allocations(self) -> list[Allocation]: ...
    @property
    def delay_range_size(self) -> int: ...
    @property
    def score(self) -> Score: ...
    @property
    def max_mode_count(self) -> int: ...
    def allocation_count(self) -> int: ...
    def value_count(self) -> int:
        """delay_range_size * max_mode_count (size of the BASIC value range)."""
    def encode_value(self, delay: int, mode_slot: int) -> int:
        """delay * max_mode_count + mode_slot. A job's mode index is always a
        valid slot."""
    def delay_of_value(self, value: int) -> int: ...
    def mode_slot_of_value(self, value: int) -> int: ...
    def execution_mode_index_of_value(self, allocation: int, value: int) -> int:
        """slot % the job's mode count (composite value -> real mode index)."""
    def assignment(self) -> list[int]:
        """Current mode_delay value per allocation (the solution)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Bulk-write the planning variable (`allocations` returns copies, so writing
        individual elements has no effect)."""

def calculate_score(schedule: ProjectJobSchedule) -> Score:
    """Full recompute of the HardMediumSoft score; also refreshes the derived dates
    (predecessors_done_date/start_date/end_date) (also stored in schedule.score)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = automatic: clamp(8 * allocations, 400, 3200)."""
    full_assert: bool
    """FULL_ASSERT: compares the incremental score to a full recompute after every move
    (for debugging; very slow)."""
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    change_move_weight: float
    swap_move_weight: float
    best_score_limit: tuple[int, int, int]
    """Stop early once (hard, medium, soft) is reached. E.g. (0, -(2**62),
    -(2**62)) stops at the first feasible solution."""
    on_new_best: Callable[[int, Score], None] | None
    """Called with (step, score) on every new best solution."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: ProjectJobSchedule) -> None:
        """Solves in place: fills allocations[*].mode_delay, the derived dates and
        schedule.score. Pinned allocations (every SOURCE/SINK + user-pinned STANDARD)
        keep their values, and uninitialized pinned allocations are first fixed to
        (first mode, delay 0)."""

    def solve_with_constraints(self, schedule: ProjectJobSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see project_job.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names project_job.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `project_job.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
