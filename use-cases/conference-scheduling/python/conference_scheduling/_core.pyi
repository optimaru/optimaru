"""Type stubs for the compiled `conference_scheduling._core` extension."""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool: ...

class Timeslot:
    id: str
    start_minute: int
    end_minute: int
    talk_types: list[int]
    tags: list[str]
    def __init__(
        self,
        id: str,
        start_minute: int,
        end_minute: int,
        talk_types: list[int],
        tags: list[str] = ...,
    ) -> None: ...
    def duration_in_minutes(self) -> int: ...

class Room:
    id: str
    name: str
    capacity: int
    talk_types: list[int]
    unavailable_timeslots: list[int]
    tags: list[str]
    def __init__(
        self,
        id: str,
        name: str = ...,
        capacity: int = ...,
        talk_types: list[int] = ...,
        unavailable_timeslots: list[int] = ...,
        tags: list[str] = ...,
    ) -> None: ...

class Speaker:
    id: str
    name: str
    unavailable_timeslots: list[int]
    required_timeslot_tags: list[str]
    preferred_timeslot_tags: list[str]
    prohibited_timeslot_tags: list[str]
    undesired_timeslot_tags: list[str]
    required_room_tags: list[str]
    preferred_room_tags: list[str]
    prohibited_room_tags: list[str]
    undesired_room_tags: list[str]
    def __init__(
        self,
        id: str,
        name: str = ...,
        unavailable_timeslots: list[int] = ...,
        required_timeslot_tags: list[str] = ...,
        preferred_timeslot_tags: list[str] = ...,
        prohibited_timeslot_tags: list[str] = ...,
        undesired_timeslot_tags: list[str] = ...,
        required_room_tags: list[str] = ...,
        preferred_room_tags: list[str] = ...,
        prohibited_room_tags: list[str] = ...,
        undesired_room_tags: list[str] = ...,
    ) -> None: ...

class Talk:
    code: str
    title: str
    talk_type: int
    speakers: list[int]
    theme_track_tags: list[str]
    sector_tags: list[str]
    audience_types: list[str]
    audience_level: int
    content_tags: list[str]
    language: str
    required_timeslot_tags: list[str]
    preferred_timeslot_tags: list[str]
    prohibited_timeslot_tags: list[str]
    undesired_timeslot_tags: list[str]
    required_room_tags: list[str]
    preferred_room_tags: list[str]
    prohibited_room_tags: list[str]
    undesired_room_tags: list[str]
    mutually_exclusive_talks_tags: list[str]
    prerequisite_talks: list[int]
    favorite_count: int
    crowd_control_risk: int
    slot_room: int
    pinned: bool
    def __init__(
        self,
        code: str,
        title: str = ...,
        talk_type: int = ...,
        speakers: list[int] = ...,
        theme_track_tags: list[str] = ...,
        sector_tags: list[str] = ...,
        audience_types: list[str] = ...,
        audience_level: int = ...,
        content_tags: list[str] = ...,
        language: str = ...,
        favorite_count: int = ...,
        crowd_control_risk: int = ...,
        prerequisite_talks: list[int] = ...,
        mutually_exclusive_talks_tags: list[str] = ...,
        required_timeslot_tags: list[str] = ...,
        preferred_timeslot_tags: list[str] = ...,
        prohibited_timeslot_tags: list[str] = ...,
        undesired_timeslot_tags: list[str] = ...,
        required_room_tags: list[str] = ...,
        preferred_room_tags: list[str] = ...,
        prohibited_room_tags: list[str] = ...,
        undesired_room_tags: list[str] = ...,
        slot_room: int = ...,
        pinned: bool = ...,
    ) -> None: ...

class ConferenceSchedule:
    name: str
    talk_types: list[str]
    timeslots: list[Timeslot]
    rooms: list[Room]
    speakers: list[Speaker]
    talks: list[Talk]
    minimum_consecutive_talks_pause_in_minutes: int
    @property
    def score(self) -> Score: ...
    @property
    def prepared(self) -> bool: ...
    def __init__(
        self,
        name: str,
        talk_types: list[str],
        timeslots: list[Timeslot],
        rooms: list[Room],
        speakers: list[Speaker],
        talks: list[Talk],
        minimum_consecutive_talks_pause_in_minutes: int = ...,
    ) -> None: ...
    def timeslot_count(self) -> int: ...
    def room_count(self) -> int: ...
    def talk_count(self) -> int: ...
    def slot_room_count(self) -> int: ...
    def slot_room_of(self, timeslot_index: int, room_index: int) -> int: ...
    def timeslot_index_of(self, slot_room: int) -> int: ...
    def room_index_of(self, slot_room: int) -> int: ...
    def assignment(self) -> list[int]: ...
    def set_assignment(self, assignment: list[int]) -> None: ...

class Breakdown:
    @property
    def talk_type_compatibility(self) -> int: ...
    @property
    def room_unavailable_timeslot(self) -> int: ...
    @property
    def room_conflict(self) -> int: ...
    @property
    def speaker_unavailable_timeslot(self) -> int: ...
    @property
    def speaker_conflict(self) -> int: ...
    @property
    def talk_prerequisite_talks(self) -> int: ...
    @property
    def talk_mutually_exclusive_talks_tags(self) -> int: ...
    @property
    def consecutive_talks_pause(self) -> int: ...
    @property
    def crowd_control(self) -> int: ...
    @property
    def speaker_required_timeslot_tags(self) -> int: ...
    @property
    def speaker_prohibited_timeslot_tags(self) -> int: ...
    @property
    def talk_required_timeslot_tags(self) -> int: ...
    @property
    def talk_prohibited_timeslot_tags(self) -> int: ...
    @property
    def speaker_required_room_tags(self) -> int: ...
    @property
    def speaker_prohibited_room_tags(self) -> int: ...
    @property
    def talk_required_room_tags(self) -> int: ...
    @property
    def talk_prohibited_room_tags(self) -> int: ...
    @property
    def theme_track_conflict(self) -> int: ...
    @property
    def theme_track_room_stability(self) -> int: ...
    @property
    def sector_conflict(self) -> int: ...
    @property
    def audience_type_theme_track_conflict(self) -> int: ...
    @property
    def content_audience_level_flow_violation(self) -> int: ...
    @property
    def content_conflict(self) -> int: ...
    @property
    def same_day_talks(self) -> int: ...
    @property
    def popular_talks(self) -> int: ...
    @property
    def speaker_preferred_timeslot_tags(self) -> int: ...
    @property
    def speaker_undesired_timeslot_tags(self) -> int: ...
    @property
    def talk_preferred_timeslot_tags(self) -> int: ...
    @property
    def talk_undesired_timeslot_tags(self) -> int: ...
    @property
    def speaker_preferred_room_tags(self) -> int: ...
    @property
    def speaker_undesired_room_tags(self) -> int: ...
    @property
    def talk_preferred_room_tags(self) -> int: ...
    @property
    def talk_undesired_room_tags(self) -> int: ...
    @property
    def speaker_makespan(self) -> int: ...
    @property
    def audience_type_diversity(self) -> int: ...
    @property
    def audience_level_diversity(self) -> int: ...
    @property
    def language_diversity(self) -> int: ...
    def hard_total(self) -> int: ...
    def soft_penalty_total(self) -> int: ...
    def soft_reward_total(self) -> int: ...
    def to_score(self) -> Score: ...

def calculate_score(schedule: ConferenceSchedule) -> Score: ...
def compute_breakdown(schedule: ConferenceSchedule) -> Breakdown: ...
def prepare(schedule: ConferenceSchedule) -> None: ...

class SolverConfig:
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    restart_segments: int
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
    ruin_recreate_as_move_max_ruin: int
    descent_weight: float
    hot_talk_probability: float
    change_move_weight: float
    swap_move_weight: float
    compatible_value_probability: float
    best_score_limit: tuple[int, float]
    on_new_best: Callable[[int, Score], None] | None
    def __init__(self) -> None: ...

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: ConferenceSchedule) -> None: ...
    def solve_with_constraints(self, schedule: ConferenceSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see conference_scheduling.constraints). The given list replaces the default
        set; None keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names conference_scheduling.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `conference_scheduling.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
