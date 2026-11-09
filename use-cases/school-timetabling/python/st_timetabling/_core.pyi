"""Type stubs for `st_timetabling._core` — the school-timetabling C++ solver.

The compiled module wraps the C++ domain in
`use-cases/school-timetabling/cpp`, which imports the generic engine in
`include/optimaru`. Two domains are exposed:

- module top level: the quickstart port (`Timetable`, slotRoom composite
  encoding, the 7 terms below) and the package's constraint catalog
  (`constraint_info`, `ConstraintInfo`, `list_constraints` — the canonical
  domain's; the quickstart model has no registry)
- `canon` submodule: the canonical full-parity domain (`st::canon`), which
  implements every hard and soft constraint of the CP-SAT reference package
  (benchmarks/school-timetabling/cpsat) with validate.py's semantics, under
  the same names.

The quickstart domain uses one assignment-shape int variable per entity (Lesson), so
the two variables (timeslot, room) are a single composite encoding::

    slot_room = timeslot_index * room_count + room_index   # -1 == unassigned

Score convention: `hard` counts constraint violations (0 == feasible),
`soft` is the objective. Both are maximised (less negative is better).

Quickstart terms (hard/soft, not selectable):
  hard 1. room_conflict_cost           -1 per pair of lessons in the same
                                       timeslot+room
  hard 2. teacher_conflict_cost        -1 per pair of same-teacher lessons in a
                                       timeslot
  hard 3. class_conflict_cost          -1 per pair of same-group lessons in a
                                       timeslot
  hard 4. teacher_unavailability_cost  -1 per lesson in a timeslot its teacher
                                       cannot take (extension)
  soft 5. teacher_room_change_cost     -1 per pair of same-teacher lessons in
                                       different rooms
  soft 6. teacher_time_efficiency_cost +1 per pair of same-teacher lessons in
                                       consecutive periods of a day (a reward)
  soft 7. class_subject_repeat_cost    -1 per pair of same-group, same-subject
                                       lessons in consecutive periods of a day
"""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple, Protocol, TypedDict, overload

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog of the canonical domain, in list_constraints()
    order: per constraint its Python name, default level, default weight (the
    C++ kDefaultWeight), whether a pick's level may change it (False
    for fixed_slots_cost / parallel_groups_cost), whether it is a reward, and
    whether it is scored by default."""

def list_constraints() -> list[str]:
    """The canonical constraint names, in catalog order (= canon.CONSTRAINT_NAMES)."""

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
    """Relative weight of the assignment-shape change move; 0 disables it."""
    swap_move_weight: float
    """Relative weight of the assignment-shape swap move; 0 disables it."""
    best_score_limit: tuple[int, float]
    """Stops early once bestScore >= (hard, soft) (lexicographic).
    (0, -inf) stops at the first feasible solution. Default: unreachable."""
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
    namespace class in the stub). Implements every constraint of the CP-SAT
    reference with validate.py's semantics (raw cost; 0 = no violation):

      hard: unassigned_lessons_cost (lessons without a slot),
            class_conflict_cost / teacher_conflict_cost (C(n,2) clashes),
            disallowed_slot_cost (placed outside allowed_slot),
            room_capacity_cost (demand over the special rooms per type and time),
            course_max_periods_per_day_cost (over a course's daily cap),
            block_adjacency_cost (broken blocks),
            teacher_max_periods_cost (daily + weekly caps),
            teacher_max_consecutive_periods_cost (consecutive-lessons cap),
            fixed_slots_cost / parallel_groups_cost (structural, always 0:
            pinned and merged lessons; not flippable)
      soft: teacher_gap_cost (20), teacher_workday_cost (3),
            teacher_free_day_cost (30, worked requested free days),
            class_gap_cost (10), avoided_period_cost (5),
            non_preferred_period_cost (1)

    Design limits: periods <= 16, weekday index < 8 (bitmasks; prepare rejects more).
    """

    class ObjectiveWeights:
        """Soft weights of the monolithic fast path (`SolverConfig.weights`),
        keyed by the soft constraint names; the defaults are the components'
        kDefaultWeight (20/3/30/10/5/1). 0 drops the term. Never part of the
        model and never read by the plugin path. The constructor and the
        field setters take a real number, finite and >= 0, like a constraint
        pick's weight knob (TypeError for a bool or a non-number, ValueError
        otherwise)."""
        def __init__(
            self,
            teacher_gap_cost: float = 20.0,
            teacher_workday_cost: float = 3.0,
            teacher_free_day_cost: float = 30.0,
            class_gap_cost: float = 10.0,
            avoided_period_cost: float = 5.0,
            non_preferred_period_cost: float = 1.0,
        ) -> None: ...
        teacher_gap_cost: float
        teacher_workday_cost: float
        teacher_free_day_cost: float
        class_gap_cost: float
        avoided_period_cost: float
        non_preferred_period_cost: float

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
        """Demand per special-room type (room_capacity_cost). Empty list = no demand."""
        allowed_slot: list[bool]
        """Per-slot allowance table (disallowed_slot_cost). Empty list = everything allowed."""
        preference_units: list[tuple[int, int]]
        """avoided_period_cost / non_preferred_period_cost units: one (avoided mask,
        preferred mask) pair per member course (bit = period - 1). Empty means
        derived from the course_key masks."""
        block_group: int
        """Block (consecutive periods) group id (block_adjacency_cost), -1 = none."""
        pinned: bool
        """True pins the slot (fixed slots: the engine never moves it)."""
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
        """Number of rooms per special-room type (room_capacity_cost)."""
        teacher_max_per_day: list[int]
        teacher_max_per_week: list[int]
        """-1 = unlimited (cpsat's None)."""
        teacher_max_consecutive: list[int]
        teacher_preferred_free_days: list[int]
        """Per-teacher requested-free-day bitmask (bit d = 0-based weekday)."""
        course_daily_limit: list[int]
        course_avoided_period_mask: list[int]
        course_preferred_period_mask: list[int]
        @property
        def score(self) -> Score: ...
        @property
        def prepared(self) -> bool: ...
        def slot_count(self) -> int: ...
        def assignment(self) -> list[int]:
            """Current slot per lesson (the solution vector)."""
        def set_assignment(self, assignment: list[int]) -> None:
            """Writes the slot of every lesson. ValueError if the length differs from the lesson count."""

    @overload
    @staticmethod
    def calculate_score(table: canon.Timetable) -> Score:
        """Full score recompute at the default weights (also stored in `table.score`)."""
    @overload
    @staticmethod
    def calculate_score(table: canon.Timetable, weights: canon.ObjectiveWeights) -> Score:
        """The same with the soft terms at `weights` (the fast path's weights)."""

    @staticmethod
    def prepare(table: canon.Timetable) -> None:
        """Normalises the instance (idempotent; calculate_score / constraint_costs /
        solve call it too). Invalid input raises ValueError."""

    @staticmethod
    def constraint_costs(
        table: canon.Timetable, constraints: Iterable[canon.ConstraintSpec] | None = None
    ) -> dict[str, dict[str, Any]]:
        """Raw per-unit cost of every default constraint (or of the picked ones),
        before weight/exponent/offset/level, in list_constraints() order:
        {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
        "clip": bool, "hard": bool, "global": bool, "level": "hard" | "medium" |
        "soft", "reward": bool, "level_flippable": bool, "name": str, "label": str,
        "default_weight": float, "default_level": str}}. "costs" holds one value per
        slot for the per-slot constraints, one total for the others. A name picked
        several times is keyed "<name>#<k>" from its second pick on, next to its
        first. `st_timetabling.constraints.score(costs, selection)` turns it into
        the engine score of any selection."""

    class ConstraintSpec(Protocol):
        """Constraint-pick duck type: the `Constraint` specs of
        `st_timetabling.constraints` and `Assembly` items qualify. `weight`
        None = the component default, `level` None = the default level
        (otherwise "hard" or "soft", or a `Level`; this use case has no
        medium level). `offset` / `clip` are optional (0.0 / True)."""

        @property
        def name(self) -> str: ...
        @property
        def weight(self) -> float | None: ...
        @property
        def level(self) -> str | None: ...
        @property
        def exponent(self) -> float: ...

    CONSTRAINT_NAMES: tuple[str, ...]
    """Every name a selection accepts, in catalog order: unassigned_lessons_cost,
    class_conflict_cost, teacher_conflict_cost, disallowed_slot_cost,
    room_capacity_cost, course_max_periods_per_day_cost, block_adjacency_cost,
    teacher_max_periods_cost, teacher_max_consecutive_periods_cost (hard, weight
    1), teacher_gap_cost (20), teacher_workday_cost (3), teacher_free_day_cost
    (30), class_gap_cost (10), avoided_period_cost (5), non_preferred_period_cost
    (1) (soft), fixed_slots_cost, parallel_groups_cost (hard, structural).
    `constraint_info()` reports the same defaults."""

    UNASSIGNED_LESSONS_SOFT_WEIGHT: float
    """The documented weight of unassigned_lessons_cost demoted to soft
    (partial timetabling): 10000 per unplaced period. A recommendation, not a
    default."""

    class ConstraintTotalDict(TypedDict):
        name: str
        label: str
        cost: float
        """Total after weighting; positive = penalty."""
        level: Literal["hard", "medium", "soft"]
        is_hard: bool

    class DecomposedCalculator:
        """Plugin-style cost assembly: pick constraints, change weights and flip
        hard/soft levels, then pass it to ``Solver.solve(table, calc)`` or score
        a fixed assignment directly.

        Assemble from constraint picks (an `Assembly` / spec list from
        `st_timetabling.constraints`) or add names one by one with :meth:`add`.
        The default set (:func:`default_calculator`) matches
        :func:`calculate_score` bit for bit."""

        @overload
        def __init__(self) -> None: ...
        @overload
        def __init__(self, constraints: Iterable[canon.ConstraintSpec] | None) -> None:
            """Duck-typed picks (`.name`/`.weight`/`.level`/`.exponent`, optionally
            `.offset`/`.clip`); None = the default set. The picks replace the
            defaults; a name may be picked several times (each pick its own
            instance with its own knobs, and the instances add up). An empty
            list raises ValueError, a wrong knob type TypeError."""

        def add(
            self,
            name: str,
            weight: float | None = None,
            level: str | None = None,
            exponent: float = 1.0,
            *,
            offset: float = 0.0,
            clip: bool = True,
        ) -> None:
            """Adds one canonical constraint by name (`CONSTRAINT_NAMES`).

            weight:   linear multiplier of the raw cost; None keeps the
                      component default.
            level:    None keeps the default level, otherwise "hard" or "soft"
                      (fixed_slots_cost / parallel_groups_cost refuse "soft").
            exponent: sign-preserving power shaping:
                      contribution = weight * sign(c)*|c|^exponent.
            offset:   (keyword) shifts that shaping (0.0 = none): with clip=True
                      (the default) weight * sign(c)*max(0, |c| - offset)^exponent,
                      with clip=False weight * (c - offset)^exponent (sign-
                      preserving; every slot of a per-slot constraint is charged),
                      refused with an offset on hard (ValueError: hard 0 must
                      mean no violation).
            Unknown names raise ValueError. Adding a name twice is allowed (the
            costs add up)."""

        def score(self, table: canon.Timetable) -> Score:
            """Scores `table`'s current assignment with this assembly (also stored
            in `table.score`). Afterwards :meth:`constraint_totals` explains it."""

        def constraint_totals(self) -> list[canon.ConstraintTotalDict]:
            """Weighted cost per constraint of the last score/solve (the score explanation),
            in list_constraints() order."""

        def constraint_costs(self, table: canon.Timetable) -> dict[str, dict[str, Any]]:
            """Raw per-unit costs of this assembly's constraints on `table` (the
            `canon.constraint_costs` shape; re-primes the calculator)."""

        @property
        def delta_group_count(self) -> int:
            """Number of structural delta groups compiled by the last reset/solve."""

    @staticmethod
    def default_calculator() -> canon.DecomposedCalculator:
        """A DecomposedCalculator with the default set, every component at its
        default weight and level (scores like `calculate_score`)."""

    class SolverConfig:
        """Configuration of `canon.Solver`. The common engine fields match the
        top-level `SolverConfig`; canonical-only additions are the fast path's
        `weights` and the guided-search knobs (`hot_lesson_probability`,
        `same_class_nearby*`, `smart_class_swap_*`)."""
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
        """The soft weights of the monolithic fast path, solve(table) only (the
        model carries none; the plugin path's components carry their own)."""
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

        Used only when the assembly demotes unassigned_lessons_cost to soft. Re-inserting into a
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
            use `calculator` (the weights come from the assembly — config.weights
            is ignored). Single start: parallel_solver_count is ignored.

            When the assembly demotes `unassigned_lessons_cost` to soft or drops
            it, the unassign moves switch on — only then can the search leave a
            lesson at slot -1 (the assignment-shape move mix's value range is [0,
            slot_count)). With it hard, every lesson takes a slot exactly as on
            the default path."""
