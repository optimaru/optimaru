"""Type stubs for `es_scheduling._core` — the employee-scheduling C++ solver.

The compiled module wraps the C++ employee-scheduling domain implementation in
`use-cases/employee-scheduling/cpp`; that implementation imports the generic
engine from `include/optimaru`. Times are integer minutes from
an arbitrary "day 0, 00:00" reference; a calendar day is
`t // MINUTES_PER_DAY`.

Score convention: `hard` counts constraint violations (0 == feasible), `soft`
is the optimization objective; both are maximized (less negative is better).

Cost plugin system
------------------
Two ways to define the cost function:

1. Built-in (hand-tuned): ``Solver(cfg).solve(schedule)`` — the eight default
   constraints at weight 1, scored by a hand-written marginal-delta
   incremental calculator (the fastest path).

2. Assembled (plugin): build a :class:`DecomposedCalculator`, pick constraints
   by name with per-constraint ``weight`` / ``hard`` / ``exponent``, then
   ``Solver(cfg).solve(schedule, calc)``. Every built-in constraint carries
   its own incremental (marginal-delta) evaluation and precomputes lookup
   tables at reset, so any assembly — custom weights, hard/soft overrides,
   shaped ``exponent != 1`` included — searches incrementally out of the box,
   at ~0.9x the built-in step rate (effectively parity).
"""

from typing import Callable, Iterable, Protocol, TypedDict, overload

class ConstraintSpec(Protocol):
    """Anything with the spec attributes: the `Constraint` dataclasses
    (es_scheduling.constraints) and the pydantic rule models
    (es_scheduling.models) both qualify."""

    @property
    def name(self) -> str: ...
    @property
    def weight(self) -> float: ...
    @property
    def hard(self) -> bool | None: ...
    @property
    def exponent(self) -> float: ...

MINUTES_PER_DAY: int
"""1440 — minutes in a day; day index of a time t is `t // MINUTES_PER_DAY`."""

MAX_DAY: int
"""The last day a shift or a day range may reach."""
CONSTRAINT_NAMES: tuple[str, ...]
"""Names accepted by :meth:`DecomposedCalculator.add`:

- ``"required_skill"``       hard; 1 per assigned shift whose required skill
                             the employee lacks.
- ``"unavailable_employee"`` hard; minutes of assigned shifts overlapping the
                             employee's unavailable days.
- ``"overlapping_shift"``    hard; overlapping minutes between two shifts of
                             the same employee.
- ``"one_shift_per_day"``    hard; 1 per pair of same-employee shifts starting
                             on the same day.
- ``"min_rest"``             hard; shortfall minutes below 10h rest between
                             two same-employee shifts.
- ``"undesired_day"``        soft; minutes on the employee's undesired days.
- ``"desired_day"``          soft reward; minutes on the employee's desired
                             days (negative cost).
- ``"balance_assignments"``  soft; load-balancing "unfairness" statistic over
                             per-employee shift counts (global, not per-unit).

Extended rules (parameters via keyword arguments
to :meth:`DecomposedCalculator.add` or spec-class fields):

- ``"max_consecutive_days"`` hard; days beyond ``value`` in any run of
                             consecutive worked days.
- ``"min_consecutive_days"`` soft; shortfall below ``value`` of any run.
- ``"rest_after_consecutive_work"`` hard; after >= ``work_days`` worked days in
                             a row, shortfall of the following rest gap below
                             ``rest_days`` (end-of-horizon runs excluded).
- ``"max_weekly_minutes"``   hard; assigned minutes beyond ``minutes`` per
                             calendar week.
- ``"min_weekly_minutes"``   soft; shortfall below ``minutes`` in every week
                             (guaranteed hours).
- ``"min_weekly_rest_days"`` hard; worked days beyond ``7 - value`` per
                             calendar week (>= ``value`` rest days per week).
- ``"assignment_cost"``      soft; money: ``shift_costs[name][id]`` override,
                             else ``cost_per_hour``/``cost_per_shift``.
- ``"unassigned_shift"``     hard; 1 per shift left unassigned.
- ``"balance_min_max_gap"``  soft; busiest-vs-least-busy gap beyond
                             ``tolerance``.
- ``"balance_weekly_minutes_gap"`` soft; per calendar week, busiest-vs-least-
                             busy gap of assigned minutes beyond
                             ``tolerance_minutes`` (all employees compared;
                             cost in hours = excess minutes / 60).
- ``"shift_coverage"``       soft; per (day, location) group, count of assignees
                             holding ``worker_skill`` outside
                             [``min_count``, ``max_count``].
- ``"location_run"``         soft; maximal runs of days at ``location`` outside
                             [``min_run``, ``max_run``].
- ``"rest_after_location_run"`` hard; worked days in the ``rest_days`` after a
                             run at ``location``.
- ``"location_run_gap"``     soft; shortfall below ``min_gap`` days between two
                             runs at ``location``.
- ``"worker_location_count"`` hard; per worker holding ``worker_skill``, count of
                             ``location`` shifts outside
                             [``min_count``, ``max_count``].
- ``"day_pattern"``          soft; 1 per window matching ``pattern``, a
                             comma-separated list of locations, ``OFF`` (a day
                             with no work), ``*`` (any day), ``WORK`` (any worked
                             day) or ``!x`` (a worked day not at location x).
- ``"target_days_off"``      soft; shortfall below ``min_count`` days off among
                             ``target_days``.
- ``"balance_location"``     soft; fairness of a per-worker count across workers
                             holding ``worker_skill``.
- ``"min_rest"`` takes an optional ``min_gap_minutes`` (default 600).

Also accepted, though not listed in ``CONSTRAINT_NAMES``:
``"worker_day_count_in_range"`` and ``"worker_pair_same_shift"``.
"""

def minutes(day: int, hour: int = 0, minute: int = 0) -> int:
    """Minutes from day 0 00:00 — the time unit used by `Shift.start`/`end`."""

class Score:
    """HardSoft score. `hard` is an integer violation total (0 == feasible);
    `soft` carries the objective, including fractional terms such as the
    load-balancing unfairness. Compared lexicographically (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True iff hard == 0."""

class Employee:
    """Problem fact: a person with skills and per-day availability preferences.
    Date sets contain day indices (see `minutes` / `MINUTES_PER_DAY`)."""
    def __init__(
        self,
        name: str,
        skills: set[str] = ...,
        unavailable_dates: set[int] = ...,
        undesired_dates: set[int] = ...,
        desired_dates: set[int] = ...,
    ) -> None: ...
    name: str
    skills: set[str]
    unavailable_dates: set[int]
    undesired_dates: set[int]
    desired_dates: set[int]
    desired_shifts: set[tuple[int, str]]
    """(day, location) location-specific work preferences (see the desired_shift rule)."""

class Shift:
    """Planning entity: one shift to be assigned to one employee.
    `employee_index` is the planning variable (-1 == unassigned); the solver
    writes it."""
    def __init__(
        self,
        id: str,
        start: int,
        end: int,
        location: str = "",
        required_skill: str = "",
    ) -> None: ...
    id: str
    start: int
    """Start time in minutes from day 0 (see `minutes`); solving and scoring
    raise ValueError unless 0 <= start < end, with end at most about 100 years
    (36525 days) from day 0."""
    end: int
    location: str
    required_skill: str
    """Skill the assignee needs; "" = none. At most 64 distinct values across the
    shifts of a schedule, else solving and scoring raise ValueError."""
    employee_index: int
    """Index into `EmployeeSchedule.employees`; -1 == unassigned. Solving and
    scoring raise ValueError for a value outside that range."""
    pinned: bool
    """If True the shift keeps its employee_index fixed (scored but never moved)."""

class EmployeeSchedule:
    """Planning solution: employees (value range) + shifts (entities).
    Solving mutates the shifts' `employee_index` and sets `score` in place."""
    def __init__(self, employees: list[Employee], shifts: list[Shift]) -> None: ...
    employees: list[Employee]
    shifts: list[Shift]
    @property
    def score(self) -> Score: ...
    def assignment(self) -> list[int]:
        """The current employee index per shift (the solution vector)."""

def calculate_score(schedule: EmployeeSchedule) -> Score:
    """Full recompute with the eight DEFAULT constraints at weight 1 (also
    stores `schedule.score`). For a custom assembly use
    :meth:`DecomposedCalculator.score` instead."""

class ConstraintTotalDict(TypedDict):
    name: str
    cost: float
    """Weighted + shaped total; positive = penalizing, negative = rewarding."""
    is_hard: bool

class DecomposedCalculator:
    """Plugin-style cost assembly: pick constraints, tune weights, override
    hard/soft, shape superlinearly — then pass to
    ``Solver.solve(schedule, calc)`` or score fixed assignments directly.

    Assemble from constraint spec objects (``es_scheduling.constraints``)::

        DecomposedCalculator([RequiredSkill(), MinimumRest(0.25, hard=False)])

    or incrementally by name via :meth:`add`. All eight defaults at weight 1
    (see :func:`default_calculator`) reproduce :func:`calculate_score` exactly.
    """
    @overload
    def __init__(self) -> None: ...
    @overload
    def __init__(self, constraints: Iterable[ConstraintSpec]) -> None:
        """Duck-typed: each item only needs `.name`, `.weight`, `.hard`,
        `.exponent` (plus an optional `.params` dict) — the `Constraint`
        dataclasses and the pydantic rule models both qualify."""
    def add(
        self,
        name: str,
        weight: float = 1.0,
        hard: bool | None = None,
        exponent: float = 1.0,
        **params: object,
    ) -> None:
        """Add a built-in constraint (see `CONSTRAINT_NAMES`).

        weight:   linear multiplier on the constraint's cost.
        hard:     True/False overrides the constraint's default level
                  (e.g. ``add("undesired_day", hard=True)`` promotes it;
                  ``add("min_rest", hard=False, weight=0.25)`` demotes it).
                  None keeps the default.
        exponent: sign-preserving power shaping of the PER-EMPLOYEE cost:
                  contribution = weight * sign(c) * abs(c) ** exponent.
                  2.0 = squared, 3.0 = cubed — concentrated violations on one
                  employee become disproportionately expensive. Shaping is
                  applied on top of the incrementally maintained raw cost, so
                  it does not slow the search down.
        params:    constraint-specific options such as
                  ``pattern="Location A,OFF,Location B"`` or ``value=6``.

        Raises ValueError for an unknown name. Duplicates are allowed (their
        costs add up).
        """
    def score(self, schedule: EmployeeSchedule) -> Score:
        """Score `schedule`'s current assignment with this assembly (also
        stores `schedule.score`) and prime the calculator so
        :meth:`constraint_totals` explains the result."""
    def constraint_totals(self) -> list[ConstraintTotalDict]:
        """Weighted cost per constraint for the last scored/solved solution —
        the score explanation ("which constraint costs how much")."""
    @property
    def delta_group_count(self) -> int:
        """Structural delta groups compiled by the last score/solve reset."""

def default_calculator() -> DecomposedCalculator:
    """A DecomposedCalculator preloaded with all eight default constraints at
    weight 1 — equivalent to the built-in score."""

class SolverConfig:
    """Termination, seeding and search options for `Solver`."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget for solve() (default 30s)."""
    no_improvement_limit_seconds: float
    """Stop after this many seconds without a best-score improvement; 0 disables
    it. Alias of `unimproved_spent_limit_seconds`."""
    unimproved_spent_limit_seconds: float
    """Wall-clock time since last best-score improvement; 0 disables it."""
    step_count_limit: int
    """Local-search step budget; 0 disables it."""
    no_improvement_step_count_limit: int
    """Stop after this many steps without a best-score improvement; 0 disables
    it. Alias of `unimproved_step_count_limit`."""
    unimproved_step_count_limit: int
    """Steps since last best-score improvement; 0 disables it."""
    random_seed: int
    """Seed of the search; the same seed reproduces the same search."""
    guided_moves: bool
    """Violation/skill-guided move selection (default True). False = uniform
    random moves."""
    employee_greedy_construction: bool
    """Seed unassigned shifts with a day-aware greedy construction heuristic
    before local search (default True)."""
    greedy_repair_budget_fraction: float
    """Share of the time budget reserved for the post-local-search greedy
    hard-repair; local search gets the rest, so total wall time honors the
    limit. Default 0.0 (off). Range [0, 0.9)."""
    late_acceptance_size: int
    """Late-acceptance queue length (default 400)."""
    ruin_recreate: bool
    """Enable adaptive ruin-and-recreate kicks on stagnation."""
    ruin_recreate_unimproved_step_trigger: int
    """Minimum unimproved steps before a ruin-and-recreate kick."""
    ruin_recreate_min_ruin_fraction: float
    """Minimum fraction of entities to ruin per kick."""
    ruin_recreate_max_ruin_fraction: float
    """Maximum fraction of entities to ruin per kick."""
    ruin_recreate_max_ruin_count: int
    """Absolute cap on entities ruined per kick."""
    ruin_recreate_operator_decay: float
    """Adaptive operator weight decay; higher adapts more slowly."""
    ruin_recreate_rewards: list[float]
    """Rewards for NEW_BEST, BETTER, ACCEPTED and REJECTED outcomes."""
    ruin_recreate_polish_steps: int
    """Hill-climbing small-move polish steps per ruin-and-recreate kick."""
    ruin_recreate_as_move_weight: float
    """Probability that a step draws destroy-and-repair as an ordinary,
    acceptor-judged move rather than only as a stagnation kick. 0 disables it
    (the default for employee scheduling: measured, descent carries this domain
    and the two do not stack)."""
    ruin_recreate_as_move_max_ruin: int
    """Ruin size for the as-a-move form; much smaller than a kick's."""
    descent_weight: float
    """Probability that a step draws best-improvement descent over one nearby
    entity pair — every value of each entity plus their swap, repeated until the
    pair is locally optimal. One sweep costs ~2x the value-range in score
    evaluations, so the usable weight scales with how many employees there
    are (default 0.02)."""
    descent_max_sweeps: int
    """Give up on a pair after this many improving passes (default 4)."""
    change_move_weight: float
    """BASIC change move relative weight; 0 disables it."""
    swap_move_weight: float
    """BASIC swap move relative weight; 0 disables it."""
    nearby_shifts: bool
    """Temporal nearby selection (default True): bias swap partners, descent
    pairs and related-ruin clusters toward shifts whose start times are close,
    where the pairwise hard constraints (overlap, one-per-day, min-rest)
    actually interact. Neighbor-list size is auto-derived from the densest 24h
    start-time window of the instance."""
    nearby_shifts_probability: float
    """Probability that a swap partner is drawn from the nearby list instead of
    uniformly (default 0.65; measured best compromise across instance sizes —
    prefer 0.8 for large-only workloads of ~1000+ shifts). Range [0, 1]."""
    smart_change_moves: bool
    """Enable employee-scheduling smart change moves when guided_moves is True.
    The move samples several employees for one shift and keeps the best sampled
    reassignment as the local-search candidate."""
    smart_change_candidate_count: int
    """Employees sampled per smart change move (default 6)."""
    employee_day_repair_moves: bool
    """Enable repair moves for one-shift-per-day violations when guided_moves is
    True. The move relocates one duplicate-day assignment to an off employee."""
    employee_run_repair_moves: bool
    """Enable repair moves for overlong consecutive-work runs when guided_moves
    is True."""
    employee_run_repair_max_days: int
    """Worked-day run length that triggers run repair moves (default 3)."""
    employee_window_moves: bool
    """Enable employee-scheduling window moves when guided_moves is True. The
    move swaps two employees' assignments over a short day window."""
    employee_window_max_days: int
    """Maximum day-window length for employee window moves (default 3)."""
    custom_move_probability: float
    """Maximum probability of consulting the adaptive custom-move pool per
    ordinary search step (default 0.30). This keeps expensive employee window
    moves from monopolizing the search after a recent reward."""
    employee_ruin_moves: bool
    """Enable employee-aware ruin sets for adaptive ruin-and-recreate when
    guided_moves and ruin_recreate are True."""
    employee_unassign_moves: bool
    """Enable the unassign move (default False): empties an over-staffed slot by
    setting an assignment back to unassigned, so coverage slots are optional
    rather than always filled."""
    relocate_move_weight: float
    """LIST relocate move relative weight; unused by employee scheduling."""
    list_swap_move_weight: float
    """LIST swap move relative weight; unused by employee scheduling."""
    two_opt_move_weight: float
    """LIST 2-opt move relative weight; unused by employee scheduling."""
    or_opt_move_weight: float
    """LIST Or-opt move relative weight; unused by employee scheduling."""
    tail_swap_move_weight: float
    """LIST 2-opt* tail-swap relative weight; unused by employee scheduling."""
    best_score_limit: tuple[int, float]
    """Stop early once bestScore >= (hard, soft), compared lexicographically.
    (0, -1e18) stops at the first feasible solution. Default: unreachable."""
    on_new_best: Callable[[int, Score], None] | None
    """Callback fired on every new best solution: on_new_best(step, score).
    Runs on the solving thread and re-acquires the GIL, which solve() releases."""

class Solver:
    """Local-search solver (late acceptance + guided moves + adaptive
    ruin-and-recreate). Solves IN PLACE: writes `shift.employee_index` and
    `schedule.score`."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(
        self,
        schedule: EmployeeSchedule,
        calculator: DecomposedCalculator = ...,
    ) -> None:
        """Without `calculator`: the built-in default cost (fastest).
        With `calculator`: search and final score use that assembly — custom
        weights included; the search runs incrementally via each
        constraint's marginal-delta evaluation."""
