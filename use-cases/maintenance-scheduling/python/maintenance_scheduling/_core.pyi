"""Type stubs for `maintenance_scheduling._core` — the maintenance-scheduling C++ solver.

The compiled module wraps the C++ domain in `use-cases/maintenance-scheduling/cpp`,
which imports the generic engine in `include/optimaru`.

The Java quickstart's two planning variables (crew, startDate) are composite-encoded
into one BASIC int variable per entity (Job)::

    crew_date = start_date_index * crew_count + crew_index   # -1 == unassigned

start_date_index indexes the list of weekdays (Mon-Fri) of the calendar [from_date, to_date)
(`MaintenanceSchedule.start_dates()`). All dates are epoch days (days since
1970-01-01, Java LocalDate.toEpochDay), and the endDate shadow is the exclusive end date
reached by advancing duration days while skipping weekends
(`plus_business_days`).

Score convention: `hard` is the sum of constraint-violating days (0 == feasible), `soft` is
the optimization objective. Both are maximized (less negative is better).

Constraints (HardSoftScore; days are calendar days, like Java ChronoUnit.DAYS):
  hard 1. crewConflict        -overlapping days per overlapping job pair on the same crew
  hard 2. minStartDate        -1 per day started before minStartDate
  hard 3. maxEndDate          -1 per day ended after maxEndDate
  soft 4. beforeIdealEndDate  -1 per day ended before idealEndDate
  soft 5. afterIdealEndDate   -1'000'000 per day ended after idealEndDate
  soft 6. tagConflict         shared tags x overlapping days of overlapping job pairs
                              -1'000
"""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    """HardSoft score. `hard` is the integer violation sum (0 == feasible); `soft`
    is the objective. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True if hard == 0."""

class Crew:
    """Problem fact: a crew. It can do at most one job per day."""
    def __init__(self, id: str, name: str) -> None: ...
    id: str
    name: str

class Job:
    """Planning entity: a maintenance job to assign. `crew_date` is the planning
    variable (-1 == unassigned), written by the solver. Dates are epoch days."""
    def __init__(
        self,
        id: str,
        name: str,
        duration_in_days: int,
        min_start_date: int | None = None,
        max_end_date: int | None = None,
        ideal_end_date: int | None = None,
        tags: list[str] = ...,
    ) -> None: ...
    id: str
    name: str
    duration_in_days: int
    """Working days (business days, weekends excluded)."""
    min_start_date: int | None
    """inclusive; None = no limit."""
    max_end_date: int | None
    """exclusive; None = no limit."""
    ideal_end_date: int | None
    """exclusive; None = no ideal end date."""
    tags: list[str]
    crew_date: int
    """start_date_index * crew_count + crew_index; -1 == unassigned."""
    pinned: bool
    """If True, crew_date is fixed (it counts in the score but never moves).
    An engine extension the quickstart does not have."""

class MaintenanceSchedule:
    """Planning solution: the calendar [from_date, to_date) x crews grid (value range)
    + jobs (entities). solve changes the jobs' `crew_date` and `score` in
    place.

    Note: list attributes such as `jobs` return copies, so to change assignments use
    `set_assignment` or assign the whole list again."""
    def __init__(
        self,
        from_date: int,
        to_date: int,
        crews: list[Crew],
        jobs: list[Job],
    ) -> None: ...
    from_date: int
    to_date: int
    crews: list[Crew]
    jobs: list[Job]
    @property
    def score(self) -> Score: ...
    def crew_count(self) -> int: ...
    def date_count(self) -> int:
        """Size of the start date value range (weekdays in the calendar)."""
    def crew_date_count(self) -> int: ...
    def crew_date_of(self, date_index: int, crew_index: int) -> int: ...
    def date_index_of(self, crew_date: int) -> int: ...
    def crew_index_of(self, crew_date: int) -> int: ...
    def start_dates(self) -> list[int]:
        """Start date value range: the weekday epoch days of [from_date, to_date)
        (Java createStartDateList())."""
    def job_start_date(self, job_index: int) -> int | None:
        """Assigned start epoch day; None when unassigned."""
    def job_end_date(self, job_index: int) -> int | None:
        """endDate shadow (exclusive; skips weekends); None when unassigned."""
    def assignment(self) -> list[int]: ...
    def set_assignment(self, assignment: list[int]) -> None: ...

def calculate_score(schedule: MaintenanceSchedule) -> Score:
    """Full recalculation (also stored in schedule.score)."""

def plus_business_days(start_date: int, business_days: int) -> int:
    """Java Job.calculateEndDate: the exclusive end epoch day reached by advancing
    business_days while skipping weekends."""

def generate_demo_data(size: str = "SMALL", from_date: int | None = None) -> MaintenanceSchedule:
    """Java DemoDataGenerator instance (bit-identical java.util.Random(17)
    reproduction). SMALL: 3 crews/8 weeks/14 jobs, LARGE: 5 crews/16 weeks/48 jobs.
    from_date defaults to next Monday (as in the Java demo); for reproducible tests
    pass a fixed Monday epoch day."""

class SolverConfig:
    """Engine settings (default: 30 s budget, late acceptance + ALNS)."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = auto: clamp(8 * jobs, 400, 3200)."""
    full_assert: bool
    """If True, re-checks incremental == full on every move (for debugging; very
    slow)."""
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: Iterable[float]
    ruin_recreate_polish_steps: int
    change_move_weight: float
    swap_move_weight: float
    window_value_probability: float
    """Probability that a change move draws a date from the job's own [minStart, idealEnd]
    start window. 0 draws uniformly from all of (date x crew)."""
    hot_job_probability: float
    """Probability of seeding a move with a job currently caught in tagConflict."""
    free_crew_candidates: int
    """Random crews a guided change move tries to find one free for the whole job
    duration. 0 draws uniformly."""
    best_score_limit: tuple[int, float]
    """Stop early once bestScore >= (hard, soft). (0, -1e18) stops at the first
    feasible."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: MaintenanceSchedule) -> None:
        """Solves in place: writes jobs[*].crew_date and schedule.score.
        Pinned jobs keep their assignment."""

    def solve_with_constraints(self, schedule: MaintenanceSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see maintenance_scheduling.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names maintenance_scheduling.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `maintenance_scheduling.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
