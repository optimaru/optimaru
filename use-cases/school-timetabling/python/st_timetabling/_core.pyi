"""Type stubs for `st_timetabling._core` — the school-timetabling C++ solver.

The compiled module wraps the C++ domain in
`use-cases/school-timetabling/cpp`, which imports the generic engine in
`include/optimaru`. Two domains are exposed:

- module top level: the quickstart port (`Timetable`, slotRoom composite
  encoding, the 7 constraints below)
- `canon` submodule: the canonical full-parity domain (`st::canon`), which
  implements hard H1–H10 / soft S1–S6 of the CP-SAT reference package
  (benchmarks/school-timetabling/cpsat) with validate.py's semantics.

The quickstart domain uses one BASIC int variable per entity (Lesson), so
the two variables (timeslot, room) are a single composite encoding::

    slot_room = timeslot_index * room_count + room_index   # -1 == unassigned

Score convention: `hard` counts constraint violations (0 == feasible),
`soft` is the objective. Both are maximised (less negative is better).

Quickstart constraints (HardSoftScore):
  hard 1. roomConflict          -1 per pair of lessons in the same timeslot+room
  hard 2. teacherConflict       -1 per pair of same-teacher lessons in a timeslot
  hard 3. studentGroupConflict  -1 per pair of same-group lessons in a timeslot
  hard 4. teacherUnavailable    -1 per lesson in a timeslot its teacher cannot
                                take (extension)
  soft 5. teacherRoomStability  -1 per pair of same-teacher lessons in
                                different rooms
  soft 6. teacherTimeEfficiency +1 per pair of same-teacher lessons in
                                consecutive periods of a day
  soft 7. studentGroupSubjectVariety  -1 per pair of same-group, same-subject
                                lessons in consecutive periods of a day
"""

from collections.abc import Callable, Iterable
from typing import Protocol, TypedDict, overload

class Score:
    """HardSoft score. `hard` is the integer violation sum (0 == feasible);
    `soft` is the objective. Compared lexicographically (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True when hard == 0."""

class Timeslot:
    """Problem fact: one (weekday, period) cell. day 0 = Monday; period is
    1-based. (day, period) pairs must be unique within a Timetable."""
    def __init__(self, day: int, period: int) -> None: ...
    day: int
    period: int

class Room:
    """Problem fact: a classroom or special room. `type` is informational — not a hard constraint."""
    def __init__(self, name: str, type: str = "") -> None: ...
    name: str
    type: str

class Lesson:
    """Planning entity: one lesson to place in the timetable. `slot_room` is
    the planning variable (-1 == unassigned); the solver writes it."""
    def __init__(
        self,
        id: str,
        subject: str,
        teacher: str,
        student_group: str,
    ) -> None: ...
    id: str
    subject: str
    teacher: str
    student_group: str
    slot_room: int
    """timeslot_index * room_count + room_index; -1 == unassigned."""
    pinned: bool
    """True pins slot_room (it is scored but never moved)."""

class Timetable:
    """Planning solution: the timeslots x rooms grid (value range) + lessons
    (entities). solve changes the lessons' `slot_room` and `score` in place.

    Note: list attributes such as `lessons` return copies, so change the
    assignment with `set_assignment`, or set it on the Lessons before building
    the Timetable."""
    def __init__(
        self,
        timeslots: list[Timeslot],
        rooms: list[Room],
        lessons: list[Lesson],
        teacher_unavailable: dict[str, set[int]] = ...,
    ) -> None: ...
    timeslots: list[Timeslot]
    rooms: list[Room]
    lessons: list[Lesson]
    teacher_unavailable: dict[str, set[int]]
    """Teacher id -> set of timeslot indices the teacher cannot take. Teachers not listed are always available."""
    @property
    def score(self) -> Score: ...
    def timeslot_count(self) -> int: ...
    def room_count(self) -> int: ...
    def slot_room_count(self) -> int:
        """timeslot_count() * room_count() — the size of the slot_room value range."""
    def slot_room_of(self, timeslot_index: int, room_index: int) -> int: ...
    def timeslot_index_of(self, slot_room: int) -> int: ...
    def room_index_of(self, slot_room: int) -> int: ...
    def assignment(self) -> list[int]:
        """Current slot_room per lesson (the solution vector)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """Writes slot_room for every lesson. ValueError if the length differs from the lesson count."""

def calculate_score(table: Timetable) -> Score:
    """Full recompute of all 7 constraints (also stored in `table.score`)."""

class SolverConfig:
    """Termination, seed and search options for `Solver`."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """Wall-clock budget of solve() (default 30 s)."""
    unimproved_spent_limit_seconds: float
    """Time limit since the last new best; 0 disables it."""
    step_count_limit: int
    """Local-search step budget; 0 disables it."""
    unimproved_step_count_limit: int
    """Step limit since the last new best; 0 disables it."""
    random_seed: int
    """0 is a non-deterministic seed; any other value is reproducible."""
    parallel_solver_count: int
    """Independent parallel starts (engine multi-start): worker i gets
    random_seed+i and the full budget; the best solution wins. Default 1."""
    late_acceptance_size: int
    """Late-acceptance queue length. 0 (default) = auto: clamp(8 * lesson count, 400,
    3200)."""
    full_assert: bool
    """True checks the incremental score against a full recompute after every
    move (debugging; very slow)."""
    ruin_recreate: bool
    """Enables adaptive ruin-and-recreate kicks on stagnation."""
    ruin_recreate_unimproved_step_trigger: int
    """Minimum unimproved steps before a kick."""
    ruin_recreate_min_ruin_fraction: float
    """Minimum fraction of entities ruined per kick."""
    ruin_recreate_max_ruin_fraction: float
    """Maximum fraction of entities ruined per kick."""
    ruin_recreate_max_ruin_count: int
    """Absolute cap on the entities ruined per kick."""
    ruin_recreate_operator_decay: float
    """Adaptive operator-weight decay; higher adapts more slowly."""
    ruin_recreate_rewards: list[float]
    """Rewards per outcome NEW_BEST / BETTER / ACCEPTED / REJECTED (exactly 4)."""
    ruin_recreate_polish_steps: int
    """Small hill-climbing polish steps per kick."""
    ruin_recreate_as_move_weight: float
    """Relative weight of destroy&repair mixed in as an ordinary move judged by
    the acceptor. Negative (default) = auto: 0.2 / slotRoomCount when
    slotRoomCount lies in the measured benefit band [900, 1300], else 0
    (benchmarks/school-timetabling/RESULTS.md). 0 disables it; a positive value
    is used as is. One as-move costs ruin count x slotRoomCount evaluations."""
    ruin_recreate_as_move_max_ruin: int
    """Cap on the lessons one as-move ruins (default 12)."""
    descent_weight: float
    """Relative weight of the best-improvement descent over nearby lesson pairs
    (default 0 = off; measured weaker than the as-move in this domain,
    benchmarks/school-timetabling/RESULTS.md)."""
    descent_max_sweeps: int
    """Maximum sweeps per descent (default 4)."""
    change_move_weight: float
    """Relative weight of the BASIC change move; 0 disables it."""
    swap_move_weight: float
    """Relative weight of the BASIC swap move; 0 disables it."""
    best_score_limit: tuple[int, float]
    """Stops early once bestScore >= (hard, soft) (lexicographic).
    (0, -1e18) stops at the first feasible solution. Default: unreachable."""
    on_new_best: Callable[[int, Score], None] | None
    """Called on every new best solution: on_new_best(step, score). The GIL is
    re-acquired on entry (safe from multi-start worker threads too)."""
    same_group_nearby: bool
    """Registers lessons of the same studentGroup as nearby neighbours (swap
    partner bias + related ruin). Default True."""
    same_group_nearby_probability: float
    """Probability of drawing the swap partner from the nearby (same group) list (default 0.5)."""

class Solver:
    """Local-search solver (construction heuristic + late acceptance + ALNS).
    Solves IN PLACE: writes `lesson.slot_room` and `table.score`.
    Pinned lessons keep their assignment."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, table: Timetable) -> None: ...

class canon:
    """Canonical full-parity domain submodule (`st::canon`).

    At runtime this is the `st_timetabling._core.canon` module (written as a
    namespace class in the stub). Implements hard H1–H10 / soft S1–S6 of the
    CP-SAT reference with validate.py's semantics:

      H1 unassigned  H2 fixed (pinned)  H3 class clash C(n,2)
      H4 teacher clash C(n,2)  H5 placed outside allowed_slot
      H6 demand over capacity per (special room type, time)
      H7 over the (course, weekday) daily cap  H8 broken block
      H9 teacher daily/weekly cap  H10 teacher consecutive-lessons cap
      S1 teacher gaps  S2 teacher working days  S3 requested free day
      S4 class gaps  S5 avoided periods  S6 off preferred periods

    Design limits: periods <= 16, weekday index < 8 (bitmasks; prepare rejects more).
    """

    class ObjectiveWeights:
        """S1–S6 weights (cpsat ObjectiveWeights defaults). 0 drops the term."""
        def __init__(
            self,
            teacher_gap: int = 20,
            teacher_workday: int = 3,
            teacher_free_day: int = 30,
            class_gap: int = 10,
            avoided_period: int = 5,
            non_preferred_period: int = 1,
        ) -> None: ...
        teacher_gap: int
        teacher_workday: int
        teacher_free_day: int
        class_gap: int
        avoided_period: int
        non_preferred_period: int

    class Lesson:
        """Planning entity: one weekly hour of a (possibly merged) course. `slot`
        is the planning variable (timeslot index, -1 == unassigned)."""
        def __init__(
            self,
            id: str,
            class_group_id: int,
            course_key: int,
            teachers: list[int],
            room_type_demand: list[int] = ...,
            block_group: int = -1,
            pinned: bool = False,
            slot: int = -1,
        ) -> None: ...
        id: str
        class_group_id: int
        course_key: int
        teachers: list[int]
        """Dense ids of every teacher teaching the lesson (team teaching >= 2,
        merged lessons take the union)."""
        room_type_demand: list[int]
        """Demand per special-room type (H6). Empty list = no demand."""
        allowed_slot: list[bool]
        """Per-slot allowance table (H5). Empty list = everything allowed."""
        preference_units: list[tuple[int, int]]
        """S5/S6 units: one (avoided mask, preferred mask) pair per member course
        (bit = period - 1). Empty means derived from the course_key masks."""
        block_group: int
        """Block (consecutive periods) group id (H8), -1 = none."""
        pinned: bool
        """True pins the slot (H2: the engine never moves it)."""
        slot: int

    class Timetable:
        """Planning solution. Slots are enumerated as (day_of_slot[s],
        period_of_slot[s]) — days may have different period counts; list only
        the cells that exist."""
        def __init__(
            self,
            day_of_slot: list[int],
            period_of_slot: list[int],
            lessons: list[canon.Lesson],
        ) -> None: ...
        day_of_slot: list[int]
        period_of_slot: list[int]
        lessons: list[canon.Lesson]
        teacher_count: int
        class_count: int
        room_capacity_by_type: list[int]
        """Number of rooms per special-room type (H6)."""
        teacher_max_per_day: list[int]
        teacher_max_per_week: list[int]
        """-1 = unlimited (cpsat's None)."""
        teacher_max_consecutive: list[int]
        teacher_preferred_free_days: list[int]
        """Per-teacher requested-free-day bitmask (bit d = 0-based weekday)."""
        course_daily_limit: list[int]
        course_avoided_period_mask: list[int]
        course_preferred_period_mask: list[int]
        weights: canon.ObjectiveWeights
        @property
        def score(self) -> Score: ...
        @property
        def prepared(self) -> bool: ...
        def slot_count(self) -> int: ...
        def assignment(self) -> list[int]:
            """Current slot per lesson (the solution vector)."""
        def set_assignment(self, assignment: list[int]) -> None:
            """Writes the slot of every lesson. ValueError if the length differs from the lesson count."""

    class Breakdown:
        """Violation counts per constraint (validate.py's hard checks +
        compute_penalties semantics). hard_total() is the absolute hard score,
        soft_penalty(weights) the absolute soft score."""
        @property
        def unassigned(self) -> int: ...
        @property
        def class_conflicts(self) -> int: ...
        @property
        def teacher_conflicts(self) -> int: ...
        @property
        def slot_not_allowed(self) -> int: ...
        @property
        def room_over_capacity(self) -> int: ...
        @property
        def course_daily_excess(self) -> int: ...
        @property
        def block_breaks(self) -> int: ...
        @property
        def teacher_daily_excess(self) -> int: ...
        @property
        def teacher_weekly_excess(self) -> int: ...
        @property
        def consecutive_excess(self) -> int: ...
        @property
        def teacher_gap(self) -> int: ...
        @property
        def teacher_workday(self) -> int: ...
        @property
        def teacher_free_day(self) -> int: ...
        @property
        def class_gap(self) -> int: ...
        @property
        def avoided_period(self) -> int: ...
        @property
        def non_preferred_period(self) -> int: ...
        def hard_total(self) -> int: ...
        def soft_penalty(self, weights: canon.ObjectiveWeights) -> int: ...
        def penalties(self) -> dict[str, int]:
            """Raw S1–S6 counts (same keys as cpsat Timetable.penalties)."""

    @staticmethod
    def calculate_score(table: canon.Timetable) -> Score:
        """Full HardSoftScore recompute (also stored in `table.score`)."""

    @staticmethod
    def compute_breakdown(table: canon.Timetable) -> canon.Breakdown:
        """Full per-constraint recompute (H1–H10 / S1–S6 counts)."""

    @staticmethod
    def prepare(table: canon.Timetable) -> None:
        """Normalises the instance (idempotent; compute_breakdown /
        calculate_score / solve call it too). Invalid input raises ValueError."""

    class ConstraintSpec(Protocol):
        """Constraint-spec duck type: the `Constraint` dataclasses of
        `st_timetabling.constraints` and `Assembly` items qualify. A `weight` of
        None means the component default (hard H* = 1, soft S* = cpsat default
        weight)."""

        @property
        def name(self) -> str: ...
        @property
        def weight(self) -> float | None: ...
        @property
        def hard(self) -> bool | None: ...
        @property
        def exponent(self) -> float: ...

    CONSTRAINT_NAMES: tuple[str, ...]
    """Every name `DecomposedCalculator.add` accepts (registry order):

    - hard (default weight 1): ``"unassigned_lesson"`` (H1),
      ``"class_conflict"`` (H3), ``"teacher_conflict"`` (H4),
      ``"slot_not_allowed"`` (H5), ``"room_capacity"`` (H6),
      ``"course_daily_limit"`` (H7), ``"block_adjacency"`` (H8),
      ``"teacher_load_limit"`` (H9), ``"teacher_consecutive_limit"`` (H10)
    - soft (default weight = the cpsat ObjectiveWeights defaults):
      ``"teacher_gap"`` (S1, 20), ``"teacher_workday"`` (S2, 3),
      ``"teacher_free_day"`` (S3, 30), ``"class_gap"`` (S4, 10),
      ``"avoided_period"`` (S5, 5), ``"non_preferred_period"`` (S6, 1)

    H2 (fixed time) is enforced by pinned lessons and is not scored.
    """

    class ConstraintTotalDict(TypedDict):
        name: str
        cost: float
        """Total after weighting; positive = penalty."""
        is_hard: bool

    class DecomposedCalculator:
        """Plugin-style cost assembly: pick constraints, change weights and flip
        hard/soft levels, then pass it to ``Solver.solve(table, calc)`` or score
        a fixed assignment directly.

        Assemble from an iterable of specs (an `Assembly` / spec list from
        `st_timetabling.constraints`) or add names one by one with :meth:`add`.
        The full default set of 15 (:func:`default_calculator`) matches
        :func:`calculate_score` bit for bit when `weights` equals
        `table.weights`."""

        @overload
        def __init__(self) -> None: ...
        @overload
        def __init__(self, constraints: Iterable[canon.ConstraintSpec]) -> None:
            """Duck-typed: items only need `.name`/`.weight`/`.hard`/`.exponent`
            (both `Assembly` and the `Constraint` dataclasses qualify)."""

        def add(
            self,
            name: str,
            weight: float | None = None,
            hard: bool | None = None,
            exponent: float = 1.0,
        ) -> None:
            """Adds a built-in canonical constraint by name (`CONSTRAINT_NAMES`).

            weight:   linear multiplier of the constraint cost; None keeps the
                      component default.
            hard:     True/False flips the default hard/soft level
                      (None = keep the default).
            exponent: sign-preserving power shaping:
                      contribution = weight * sign(c)*|c|^exponent.
            Unknown names raise ValueError. Adding a name twice is allowed (the
            costs add up)."""

        def score(self, table: canon.Timetable) -> Score:
            """Scores `table`'s current assignment with this assembly (also stored
            in `table.score`). Afterwards :meth:`constraint_totals` explains it."""

        def constraint_totals(self) -> list[canon.ConstraintTotalDict]:
            """Weighted cost per constraint of the last score/solve (the score explanation)."""

        @property
        def delta_group_count(self) -> int:
            """Number of structural delta groups compiled by the last reset/solve."""

    @staticmethod
    def default_calculator(weights: canon.ObjectiveWeights = ...) -> canon.DecomposedCalculator:
        """A DecomposedCalculator with the full canonical set: hard H1–H10 at
        weight 1, soft S1–S6 at `weights` (default: the cpsat default weights)."""

    class SolverConfig:
        """Configuration of `canon.Solver`. The common engine fields match the
        top-level `SolverConfig`; canonical-only additions are `weights` and the
        guided-search knobs (`hot_lesson_probability`, `same_class_nearby*`,
        `smart_class_swap_*`)."""
        def __init__(self) -> None: ...
        spent_limit_seconds: float
        unimproved_spent_limit_seconds: float
        step_count_limit: int
        unimproved_step_count_limit: int
        random_seed: int
        parallel_solver_count: int
        late_acceptance_size: int
        """0 = auto: clamp(4 * lesson count, 400, 3200). A positive value is used as is."""
        full_assert: bool
        ruin_recreate: bool
        ruin_recreate_unimproved_step_trigger: int
        ruin_recreate_min_ruin_fraction: float
        ruin_recreate_max_ruin_fraction: float
        ruin_recreate_max_ruin_count: int
        ruin_recreate_operator_decay: float
        ruin_recreate_rewards: list[float]
        ruin_recreate_polish_steps: int
        ruin_recreate_as_move_weight: float
        """Relative weight of the destroy&repair as-move (default 0 = off — unlike
        the quickstart side there is no auto rule; negative also means off)."""
        ruin_recreate_as_move_max_ruin: int
        descent_weight: float
        descent_max_sweeps: int
        change_move_weight: float
        swap_move_weight: float
        best_score_limit: tuple[int, float]
        on_new_best: Callable[[int, Score], None] | None
        weights: canon.ObjectiveWeights
        """Copied into Timetable.weights when solve() starts."""
        hot_lesson_probability: float
        """Probability of picking a lesson involved in a hard violation as the move target (0 = off)."""
        same_class_nearby: bool
        """Registers lessons of the same class as nearby neighbours (swap partner / related ruin)."""
        same_class_nearby_probability: float
        smart_class_swap_moves: bool
        """Best-of-k same-class swap custom move (k evaluations, soft phase only)."""
        smart_class_swap_candidates: int
        reassign_candidates: int
        """Placements tried when putting an unassigned lesson back (partial timetabling only).

        Used only when the assembly demotes H1 to soft. Re-inserting into a
        tight timetable needs a slot that clashes with nobody, which a single
        random cell almost never finds — looking wide is the point of this move,
        hence a larger k than the swap's. Track-3 sweep (partial_M, 10 s):
        8 -> 325 cells, 16 -> 328, 32 -> 342, 64 -> 341."""

    class Solver:
        """Canonical-domain local-search solver (allowed_slot-based value
        recommender + FFD construction order + ALNS). Solves IN PLACE."""
        def __init__(self, config: canon.SolverConfig = ...) -> None: ...
        @overload
        def solve(self, table: canon.Timetable) -> None:
            """Fast path through the monolithic incremental calculator (default)."""
        @overload
        def solve(self, table: canon.Timetable, calculator: canon.DecomposedCalculator) -> None:
            """Plugin-assembly path: both the search and the final `table.score`
            use `calculator` (soft weights come from the assembly — config.weights
            is ignored). Single start: parallel_solver_count is ignored.

            When the assembly demotes H1 (`unassigned_lesson`) to soft or drops it,
            the unassign moves switch on — only then can the search leave a lesson
            at slot -1 (the BASIC move mix's value range is [0, slot_count)). With
            H1 hard, every lesson takes a slot exactly as on the default path."""
