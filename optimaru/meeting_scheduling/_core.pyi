"""Type stubs for `optimaru.meeting_scheduling._core` — the meeting-scheduling C++ solver.

The compiled module wraps the C++ domain in `optimaru/meeting_scheduling/cpp`,
which imports the generic engine in `optimaru/cpp/optimaru`.

Constraints (hard / medium / soft levels) and the score mapping
(registry names; default weights per raw unit):
  hard 1. room_conflict_cost        -1 per overlapping grain of two meetings in one room
  hard 2. overtime_cost             -lastTimeGrainIndex when no TimeGrain exists at the
                                    last grain index
  hard 3. required_attendance_conflict_cost  -1 per overlapping grain of pairs sharing a
                                    required attendee
  hard 4. room_capacity_cost        -(shortfall) when needed seats (required+preferred)
                                    exceed the room capacity
  hard 5. day_crossing_cost         -1 when start and end days differ
  med  6. required_and_preferred_attendance_conflict_cost  -1 medium per overlapping
                                    grain of (required, preferred) pairs
  med  7. preferred_attendance_conflict_cost  -1 medium per overlap of pairs sharing a
                                    preferred attendee
  soft 8. meeting_end_time_cost     -lastTimeGrainIndex (per meeting)
  soft 9. consecutive_meetings_cost  -100 per adjacent meeting pair
  soft 10. meeting_overlap_cost     -10 per overlapping grain of distinct meetings
  soft 11. larger_room_capacity_cost  -(capacity difference) per larger room
  soft 12. room_change_cost         -1 when an attendee's consecutive (gap <= 2 grains)
                                    meetings change room

The engine score is Score(hard int, soft float), so medium is folded into
soft: ``soft = -(medium penalty * MEDIUM_SCALE + raw soft penalty)``
(MEDIUM_SCALE == 1e6; medium always dominates soft).

A meeting's starting time grain and room are one decision
variable, the composite encoding ``grain_room = time_grain_pos * room_count + room_index``
(-1 == unassigned). A meeting occupies duration_in_grains consecutive grain indices
from its start grain, and every overlap constraint is computed over that whole span.
"""

from collections.abc import Callable, Iterable, Sequence
from typing import Any, Literal, NamedTuple

MEDIUM_SCALE: float
GRAIN_LENGTH_IN_MINUTES: int

class Score:
    """Two-level score (hard, soft). `hard` is the integer violation sum (0 == feasible); `soft`
    is the folded medium/soft objective. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True if hard == 0."""

class Person:
    """An attendee (fixed input). Referenced by index into the people list."""
    def __init__(self, id: str, full_name: str = "") -> None: ...
    id: str
    full_name: str

class TimeGrain:
    """A 15-minute time grain (fixed input). grain_index is the unit of duration/overlap,
    and (day_of_year, starting_minute_of_day) is the calendar position."""
    def __init__(
        self,
        id: str,
        grain_index: int,
        day_of_year: int,
        starting_minute_of_day: int,
    ) -> None: ...
    id: str
    grain_index: int
    day_of_year: int
    starting_minute_of_day: int

class Room:
    """A meeting room, fixed input (capacity is used by room_capacity_cost / larger_room_capacity_cost)."""
    def __init__(self, id: str, name: str = "", capacity: int = 0) -> None: ...
    id: str
    name: str
    capacity: int

class Meeting:
    """A meeting (fixed input). required/preferred_attendees are lists of people
    indices — one element is one attendance."""
    def __init__(
        self,
        id: str,
        topic: str = "",
        duration_in_grains: int = 1,
        required_attendees: Sequence[int] = (),
        preferred_attendees: Sequence[int] = (),
        entire_group_meeting: bool = False,
    ) -> None: ...
    id: str
    topic: str
    duration_in_grains: int
    required_attendees: list[int]
    preferred_attendees: list[int]
    entire_group_meeting: bool
    def required_capacity(self) -> int:
        """Required + preferred attendee count."""

class MeetingAssignment:
    """The (start grain, room) assignment of one meeting. `grain_room` is
    the composite decision variable (-1 == unassigned); meeting is a meetings index, and
    meetings and assignments must be 1:1."""
    def __init__(
        self,
        id: str,
        meeting: int,
        grain_room: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: str
    meeting: int
    grain_room: int
    """time_grain_pos * room_count + room_index; -1 == unassigned."""
    pinned: bool
    """If True, grain_room is fixed (it counts in the score but never moves)."""

class MeetingSchedule:
    """The schedule: people + time_grains/rooms (value ranges) + meetings +
    meeting_assignments (entities). solve changes the assignments' `grain_room` and
    `score` in place.

    Note: list attributes such as `meeting_assignments` return copies, so use
    `set_assignment` to change assignments. After changing the structure
    (meetings/grains/rooms), build a new MeetingSchedule (derived tables are
    prepared once, at the first scoring)."""
    def __init__(
        self,
        people: Sequence[Person],
        time_grains: Sequence[TimeGrain],
        rooms: Sequence[Room],
        meetings: Sequence[Meeting],
        meeting_assignments: Sequence[MeetingAssignment],
    ) -> None: ...
    people: list[Person]
    time_grains: list[TimeGrain]
    rooms: list[Room]
    meetings: list[Meeting]
    meeting_assignments: list[MeetingAssignment]
    @property
    def score(self) -> Score: ...
    @property
    def prepared(self) -> bool: ...
    def time_grain_count(self) -> int: ...
    def room_count(self) -> int: ...
    def grain_room_count(self) -> int: ...
    def grain_room_of(self, time_grain_pos: int, room_index: int) -> int:
        """Composite value encoding: time_grain_pos * room_count + room_index."""

    def time_grain_pos_of(self, grain_room: int) -> int: ...
    def room_index_of(self, grain_room: int) -> int: ...
    def assignment(self) -> list[int]:
        """Current grain_room per assignment (-1 == unassigned)."""

    def set_assignment(self, assignment: Sequence[int]) -> None:
        """Bulk-write every grain_room (length must equal the number of assignments)."""

def calculate_score(schedule: MeetingSchedule) -> Score:
    """Full recalculation (also stored in schedule.score)."""

def demo_data() -> MeetingSchedule:
    """The demo instance (deterministic): 20 people, 160 time grains, 3 rooms, 24 meetings."""

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
    """0 = automatic: clamp(128 * assignments, 3200, 6400)."""
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
    """Per-step probability of also drawing destroy-repair as an ordinary move judged by
    the acceptor (0 disables; default 2e-4, tuned by the quality bench)."""
    ruin_recreate_as_move_max_ruin: int
    """Upper bound on assignments ruined by one as-move destroy-repair (default 12)."""
    change_move_weight: float
    swap_move_weight: float
    day_fit_value_probability: float
    """Probability that a change move draws only start grains that keep the meeting
    within one day (0 disables)."""
    best_score_limit: tuple[int, float]
    """Stop early once bestScore >= (hard, soft); (0, -inf) stops at the first
    feasible."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: MeetingSchedule) -> None:
        """Solves in place: writes the assignments' grain_room and schedule.score.
        Pinned assignments are kept."""

    def solve_with_constraints(self, schedule: MeetingSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see optimaru.meeting_scheduling.constraints). The given list replaces the default set; None
        keeps the default set. A name may be picked several times: each pick is its own
        instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case:
    the five per-meeting constraints, then the seven pair constraints).
    The same names optimaru.meeting_scheduling.constraints uses."""

def constraint_costs(
    schedule: MeetingSchedule, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/offset/level rounding.
    A unit is a (time grain, room) cell; a pair (global) constraint reports one cost.
    `optimaru.meeting_scheduling.constraints.score()` takes these and produces a score:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str,
    "medium_fold": {"kind": "into_soft", "unit": 1e6}}}. The medium fold belongs
    to the domain, so EVERY entry carries it. None = the default constraints; a pick
    list (as for solve) scores exactly those picks, each entry carrying its pick's
    knobs, the k-th pick of a name keyed "<name>#<k>" from the second on."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: the default level and
    weight of each constraint (required_and_preferred_attendance_conflict_cost and
    preferred_attendance_conflict_cost are medium at weight 1 in medium units;
    consecutive_meetings_cost soft at 100, meeting_overlap_cost soft at 10; every
    other constraint at weight 1). All flippable, none a reward, all enabled by
    default."""
