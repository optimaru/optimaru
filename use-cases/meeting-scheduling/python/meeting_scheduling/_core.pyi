"""Type stubs for `meeting_scheduling._core` — the meeting-scheduling C++ solver.

The compiled module wraps the C++ domain in `use-cases/meeting-scheduling/cpp`,
which imports the generic engine in `include/optimaru`.

Constraints of the Java quickstart (HardMediumSoftScore) and the score mapping:
  hard 1. roomConflict          -1 per overlapping grain of two meetings in one room
  hard 2. avoidOvertime         -lastTimeGrainIndex when no TimeGrain exists at the
                                last grain index
  hard 3. requiredAttendanceConflict  -1 per overlapping grain of pairs sharing a required attendee
  hard 4. requiredRoomCapacity  -(shortfall) when needed seats (required+preferred)
                                exceed the room capacity
  hard 5. startAndEndOnSameDay  -1 when start and end days differ
  med  6. requiredAndPreferredAttendanceConflict  -1 per overlapping grain of
                                (required, preferred) pairs
  med  7. preferredAttendanceConflict  -1 per overlap of pairs sharing a preferred attendee
  soft 8. doMeetingsAsSoonAsPossible  -lastTimeGrainIndex (per meeting)
  soft 9. oneBreakBetweenConsecutiveMeetings  -100 per adjacent meeting pair
  soft 10. overlappingMeetings  -10 per overlapping grain of distinct meetings
  soft 11. assignLargerRoomsFirst  -(capacity difference) per larger room
  soft 12. roomStability        -1 when an attendee's consecutive (gap <= 2 grains)
                                meetings change room

The engine score is HardSoftScore(hard int, soft float), so medium is folded into
soft: ``soft = -(medium penalty * MEDIUM_SCALE + raw soft penalty)``
(MEDIUM_SCALE == 1e6; medium always dominates soft).

The Java entity's two planning variables (startingTimeGrain, room) become one BASIC
variable via the composite encoding ``grain_room = time_grain_pos * room_count + room_index``
(-1 == unassigned). A meeting occupies duration_in_grains consecutive grain indices
from its start grain, and every overlap constraint is computed over that whole span.
"""

from collections.abc import Callable, Iterable, Sequence
from typing import Any

MEDIUM_SCALE: float
GRAIN_LENGTH_IN_MINUTES: int

class Score:
    """HardSoft score. `hard` is the integer violation sum (0 == feasible); `soft`
    is the folded medium/soft objective. Lexicographic comparison (hard first)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """True if hard == 0."""

class Person:
    """Problem fact: an attendee. Referenced by index into the people list."""
    def __init__(self, id: str, full_name: str = "") -> None: ...
    id: str
    full_name: str

class TimeGrain:
    """Problem fact: a 15-minute time grain. grain_index is the unit of duration/overlap,
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
    """Problem fact: a meeting room (capacity is used by hard 4 / soft 11)."""
    def __init__(self, id: str, name: str = "", capacity: int = 0) -> None: ...
    id: str
    name: str
    capacity: int

class Meeting:
    """Problem fact: a meeting. required/preferred_attendees are lists of people
    indices — one element is one Java Attendance fact."""
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
        """Required + preferred attendee count (Java getRequiredCapacity)."""

class MeetingAssignment:
    """Planning entity: the (start grain, room) assignment of one meeting. `grain_room` is
    the composite planning variable (-1 == unassigned); meeting is a meetings index, and
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
    """Planning solution: people + time_grains/rooms (value ranges) + meetings +
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
        """Bulk-write planning variables (length must equal the number of assignments)."""

class Breakdown:
    """Raw violation counts per constraint (before weights; exactly the amounts the
    Java ConstraintProvider penalizes)."""
    @property
    def room_conflict_grains(self) -> int: ...
    @property
    def overtime_sum(self) -> int: ...
    @property
    def required_conflict_grains(self) -> int: ...
    @property
    def room_capacity_shortage(self) -> int: ...
    @property
    def start_end_day_violations(self) -> int: ...
    @property
    def required_preferred_conflict_grains(self) -> int: ...
    @property
    def preferred_conflict_grains(self) -> int: ...
    @property
    def last_grain_index_sum(self) -> int: ...
    @property
    def consecutive_meeting_pairs(self) -> int: ...
    @property
    def overlapping_meeting_grains(self) -> int: ...
    @property
    def larger_room_capacity_sum(self) -> int: ...
    @property
    def room_stability_violations(self) -> int: ...
    def hard_total(self) -> int: ...
    def medium_total(self) -> int: ...
    def soft_total(self) -> int:
        """Sum with soft weights applied (x1/x100/x10)."""

def calculate_score(schedule: MeetingSchedule) -> Score:
    """Full recalculation (also stored in schedule.score)."""

def compute_breakdown(schedule: MeetingSchedule) -> Breakdown:
    """Full recalculation per constraint (raw violation counts)."""

def demo_data() -> MeetingSchedule:
    """Reproduces Java DemoDataGenerator (the same draws as java.util.Random(0)):
    20 people, 160 time grains, 3 rooms, 24 meetings."""

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
    """Stop early once bestScore >= (hard, soft); (0, -1e18) stops at the first
    feasible."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: MeetingSchedule) -> None:
        """Solves in place: writes the assignments' grain_room and schedule.score.
        Pinned assignments are kept."""

    def solve_with_constraints(self, schedule: MeetingSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see meeting_scheduling.constraints). The given list replaces the default set; None
        keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names meeting_scheduling.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `meeting_scheduling.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
