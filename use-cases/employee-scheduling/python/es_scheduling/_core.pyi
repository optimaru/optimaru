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

2. Assembled (plugin): build a :class:`DecomposedCalculator` from constraint
   specs (``es_scheduling.constraints``, one class per constraint) or by name
   with per-constraint ``weight`` / ``level`` / ``exponent`` and domain
   parameters, then ``Solver(cfg).solve(schedule, calc)``. Every built-in
   constraint carries its own incremental (marginal-delta) evaluation and
   precomputes lookup tables at reset, so any assembly — custom weights,
   level overrides, shaped ``exponent != 1`` included — searches
   incrementally out of the box, at ~0.9x the built-in step rate (effectively
   parity).

The defaults (level, weight, reward) live in the C++ components and are
reported by :func:`constraint_info`; the rule factory accepts the 30 names of
``CONSTRAINT_NAMES``.
"""

from typing import Any, Callable, Iterable, Literal, NamedTuple, Protocol, TypedDict, overload

class ConstraintSpec(Protocol):
    """Anything with the spec attributes: the `Constraint` dataclasses
    (es_scheduling.constraints) and the pydantic rule models
    (es_scheduling.models) both qualify. ``params()`` (optional) returns the
    domain parameters."""

    @property
    def name(self) -> str: ...
    @property
    def weight(self) -> float | None: ...
    @property
    def level(self) -> str | None: ...
    @property
    def exponent(self) -> float: ...

MINUTES_PER_DAY: int
"""1440 — minutes in a day; day index of a time t is `t // MINUTES_PER_DAY`."""

MAX_DAY: int
"""The last day a shift or a day range may reach."""
CONSTRAINT_NAMES: tuple[str, ...]
"""Every name the rule factory accepts (:meth:`DecomposedCalculator.add`, the
spec classes), in factory order. Default level and weight 1 each (the eight
defaults first in :func:`constraint_info`):

- ``"required_skill_cost"``          hard; 1 per assigned shift whose required
                                     skill the employee lacks.  [default]
- ``"employee_unavailability_cost"`` hard; minutes of assigned shifts on the
                                     employee's unavailable days.  [default]
- ``"shift_overlap_cost"``           hard; overlapping minutes between two shifts
                                     of the same employee.  [default]
- ``"max_one_shift_per_day_cost"``   hard; 1 per pair of same-employee shifts
                                     starting on the same day.  [default]
- ``"min_rest_cost"``                hard; shortfall minutes below
                                     ``min_gap_minutes`` (600) of rest between two
                                     same-employee shifts.  [default]
- ``"undesired_day_cost"``           soft; minutes on the employee's undesired
                                     days (``per_day``: 1 per shift).  [default]
- ``"desired_day_cost"``             soft REWARD (raw <= 0, never hard); minutes on
                                     the employee's desired days (``per_day``: 1 per
                                     shift).  [default]
- ``"desired_shift_cost"``           soft REWARD (raw <= 0, never hard); 1 per
                                     assigned shift matching a desired (day,
                                     location).
- ``"shift_count_imbalance_cost"``   soft; load-balancing "unfairness" statistic
                                     over per-employee shift counts (global).
                                     [default]
- ``"max_consecutive_days_cost"``    hard; days beyond ``value`` in any run of
                                     consecutive worked days.
- ``"min_consecutive_days_cost"``    soft; shortfall below ``value`` of any run.
- ``"max_consecutive_days_off_cost"`` soft; interior off runs beyond ``value``.
- ``"rest_after_consecutive_work_cost"`` hard; after >= ``work_days`` worked days in
                                     a row, shortfall of the following rest gap below
                                     ``rest_days`` (end-of-horizon runs excluded).
- ``"max_weekly_minutes_cost"``      hard; assigned minutes beyond ``minutes`` per
                                     calendar week (``week_anchor``).
- ``"min_weekly_minutes_cost"``      soft; shortfall below ``minutes`` in every
                                     week (guaranteed hours; ``week_anchor``).
- ``"min_weekly_rest_days_cost"``    hard; worked days beyond ``7 - value`` per
                                     calendar week (>= ``value`` rest days per week).
- ``"assignment_cost"``              soft; money: ``shift_costs[name][id]`` override,
                                     else ``cost_per_hour``/``cost_per_shift``.
- ``"unassigned_shifts_cost"``       hard; 1 per shift left unassigned (global).
- ``"shift_count_gap_cost"``         soft; busiest-vs-least-busy gap beyond
                                     ``tolerance`` (global).
- ``"weekly_minutes_gap_cost"``      soft; per calendar week, busiest-vs-least-busy
                                     gap of assigned minutes beyond
                                     ``tolerance_minutes``, in hours (global).
- ``"shift_coverage_cost"``          soft; per (day, location) group, count of
                                     assignees holding ``worker_skill`` outside
                                     [``min_count``, ``max_count``] (global).
- ``"location_run_length_cost"``     soft; maximal runs of days at ``location``
                                     outside [``min_run``, ``max_run``].
- ``"rest_after_location_run_cost"`` hard; worked days in the ``rest_days`` after a
                                     run at ``location``.
- ``"location_run_gap_cost"``        soft; shortfall below ``min_gap`` days between
                                     two runs at ``location``.
- ``"worker_location_count_cost"``   SOFT; per worker holding ``worker_skill``,
                                     count of ``location`` shifts outside
                                     [``min_count``, ``max_count``].
- ``"day_pattern_cost"``             soft; 1 per window matching ``pattern``, a
                                     comma-separated list of locations, ``OFF`` (a
                                     day with no work), ``*`` (any day), ``WORK``
                                     (any worked day) or ``!x`` (a worked day not at
                                     location x).
- ``"target_days_off_cost"``         soft; shortfall below ``min_count`` days off
                                     among ``target_days``.
- ``"worker_day_count_in_range_cost"`` SOFT; distinct worked days in [``lo``,
                                     ``hi``] outside [``min_count``, ``max_count``].
- ``"location_imbalance_cost"``      soft; fairness of a per-worker count across
                                     workers holding ``worker_skill`` (global).
- ``"worker_pairing_cost"``          soft; 1 per (day, location) bucket where the
                                     pairing of ``anchor`` and ``paired`` fails
                                     (raw >= 0; global).

Unit constraints take an optional scope ``worker_indices`` / ``shift_indices``;
``shift_coverage_cost`` scopes natively; the other globals take none.
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
    """(day, location) location-specific work preferences (see the desired_shift_cost rule)."""

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
    """If True the shift keeps its employee_index fixed (scored but never moved).
    Solving and scoring raise ValueError if it has none."""

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
    """Full recompute with the eight DEFAULT constraints at their default
    weights (also stores `schedule.score`). For a custom assembly use
    :meth:`DecomposedCalculator.score` instead."""

class ConstraintTotalDict(TypedDict):
    name: str
    """The constraint's Python name, e.g. ``"day_pattern_cost"``."""
    label: str
    """Display text of this instance of a parametrized rule, e.g.
    ``"Day pattern[Location A,OFF,Location B]"``; "" for one without."""
    cost: float
    """Weighted + shaped total; positive = penalizing, negative = rewarding."""
    level: Literal["hard", "medium", "soft"]
    is_hard: bool

class DecomposedCalculator:
    """Plugin-style cost assembly: pick constraints, tune weights, override
    hard/soft, shape superlinearly — then pass to
    ``Solver.solve(schedule, calc)`` or score fixed assignments directly.

    Assemble from constraint spec objects (``es_scheduling.constraints``)::

        DecomposedCalculator([RequiredSkillCost(), MinRestCost(weight=0.25, level="soft")])

    or incrementally by name via :meth:`add`. The eight defaults at their
    default weights (see :func:`default_calculator`) reproduce
    :func:`calculate_score` exactly. A constraint may be added several times
    (other parameters, scope or knobs, or the same ones); each instance is
    scored on its own with its own knobs, and the instances add up.
    """
    @overload
    def __init__(self) -> None: ...
    @overload
    def __init__(self, constraints: Iterable[ConstraintSpec]) -> None:
        """Duck-typed: each item only needs `.name`, `.weight`, `.level`,
        `.exponent` (plus an optional `.offset` / `.clip` and `params()`) —
        the `Constraint` dataclasses and the pydantic rule models both
        qualify. A name may be picked several times: each pick is its own
        instance with its own knobs, and the instances add up. Raises
        ValueError for an empty list (use ``DecomposedCalculator()`` and
        ``add()`` to start empty), an unknown name, a reward made hard, a
        scope on a global rule other than shift_coverage_cost, or an invalid
        knob value, and TypeError for a knob of the wrong type."""
    def add(
        self,
        name: str,
        weight: float | None = None,
        level: str | None = None,
        exponent: float = 1.0,
        *,
        offset: float = 0.0,
        clip: bool = True,
        **params: object,
    ) -> None:
        """Add a built-in constraint (see `CONSTRAINT_NAMES`).

        weight:   None = the component's default weight; otherwise a linear
                  multiplier (finite, >= 0) on the constraint's cost.
        level:    None keeps the constraint's default level, otherwise "hard"
                  or "soft", or a ``Level`` (e.g. ``add("undesired_day_cost",
                  level="hard")`` promotes it; ``add("min_rest_cost",
                  level="soft", weight=0.25)`` demotes it). The rewards
                  (desired_day_cost, desired_shift_cost) refuse "hard"; "medium"
                  is refused (no medium level here), and the removed ``hard=``
                  keyword raises TypeError.
        exponent: sign-preserving power shaping of the PER-EMPLOYEE cost:
                  contribution = weight * sign(c) * abs(c) ** exponent.
                  2.0 = squared, 3.0 = cubed — concentrated violations on one
                  employee become disproportionately expensive. Shaping is
                  applied on top of the incrementally maintained raw cost, so
                  it does not slow the search down.
        offset:   (keyword) shifts that shaping, any finite number (0.0 =
                  none). With ``clip=True`` (the default) only what lies
                  beyond it counts: ``sign(c) * max(0, abs(c) - offset) **
                  exponent``; with ``clip=False`` the literal ``(c - offset)
                  ** exponent`` (sign-preserving), which charges every
                  employee the rule evaluates, a raw 0 included (a worker
                  scope evaluates its employees only), and rewards below the
                  offset; with an offset it is refused on hard (ValueError),
                  because hard 0 must mean no violation.
        params:    constraint-specific options such as
                  ``pattern="Location A,OFF,Location B"`` or ``value=6``, and the
                  scope ``worker_indices`` / ``shift_indices``.

        The same checks as a spec pick: ValueError for an unknown name, a
        refused level or an invalid knob value, TypeError for a knob of the
        wrong type. Duplicates are allowed (each call is its own instance;
        their costs add up).
        """
    def score(self, schedule: EmployeeSchedule) -> Score:
        """Score `schedule`'s current assignment with this assembly (also
        stores `schedule.score`) and prime the calculator so
        :meth:`constraint_totals` explains the result."""
    def constraint_totals(self) -> list[ConstraintTotalDict]:
        """Weighted cost per constraint instance for the last scored/solved
        solution — the score explanation ("which constraint costs how much")."""
    @property
    def delta_group_count(self) -> int:
        """Structural delta groups compiled by the last score/solve reset."""

def default_calculator() -> DecomposedCalculator:
    """A DecomposedCalculator preloaded with the eight default constraints at
    their default weights — equivalent to the built-in score."""

def constraint_costs(
    schedule: EmployeeSchedule, constraints: Iterable[ConstraintSpec] | None = None
) -> dict[str, dict[str, Any]]:
    """Raw per-employee cost of every constraint instance, before
    weight/exponent/offset/level: {key: {"costs": [...], "weight": float,
    "exponent": float, "offset": float, "clip": bool, "hard": bool, "global":
    bool, "level": "hard" | "medium" | "soft", "reward": bool,
    "level_flippable": bool, "name": str, "label": str, "default_weight":
    float, "default_level": str[, "units": [...]]}}.

    ``constraints`` None = the eight defaults; otherwise the picks, built by
    the same factory as :class:`DecomposedCalculator` (an empty list raises
    ValueError). The key is the Python name, ``"<name>#<k>"`` for the k-th
    instance of a name picked more than once; ``name`` is always the plain
    name and ``label`` the instance's display text. A global constraint
    reports its single total as a one-element list. A worker-scoped rule
    carries ``"units"``, the employee indices it evaluates (ascending, in
    range, each once): the literal offset formula charges those and no
    other. Feed the result to ``es_scheduling.constraints.score`` with the
    same list to re-price it."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default and catalog assemblies."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog: one row per name the rule factory accepts (30).
    The eight defaults come first (``default_enabled=True``, in
    ``get_constraints()`` order), then the opt-in rules
    (``default_enabled=False``; unit constraints before global ones). Every
    default weight is 1; ``desired_day_cost`` / ``desired_shift_cost`` are
    rewards (``reward=True``, ``level_flippable=False``)."""

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
    """Assignment-shape change move relative weight; 0 disables it."""
    swap_move_weight: float
    """Assignment-shape swap move relative weight; 0 disables it."""
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
    """Sequence-shape relocate move relative weight; unused by employee scheduling."""
    list_swap_move_weight: float
    """Sequence-shape swap move relative weight; unused by employee scheduling."""
    two_opt_move_weight: float
    """Sequence-shape 2-opt move relative weight; unused by employee scheduling."""
    or_opt_move_weight: float
    """Sequence-shape Or-opt move relative weight; unused by employee scheduling."""
    tail_swap_move_weight: float
    """Sequence-shape 2-opt* tail-swap relative weight; unused by employee scheduling."""
    best_score_limit: tuple[int, float]
    """Stop early once bestScore >= (hard, soft), compared lexicographically.
    (0, -inf) stops at the first feasible solution. Default: unreachable."""
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
