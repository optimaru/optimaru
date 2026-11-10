"""Type stubs for the compiled `optimaru.conference_scheduling._core` extension."""

from collections.abc import Callable, Iterable
from typing import Any, Literal, NamedTuple

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

def calculate_score(schedule: ConferenceSchedule) -> Score: ...
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
        (see optimaru.conference_scheduling.constraints). The given list replaces the default
        set; None keeps the default set. A name may be picked several times: each pick is
        its own instance with its own knobs, and the instances add up."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case):
    talk_type_incompatibility_cost, room_unavailability_cost, room_conflict_cost, ...,
    language_diversity_cost (37). The same names optimaru.conference_scheduling.constraints uses."""

def constraint_costs(
    schedule: ConferenceSchedule, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw cost of every constraint, before weight/exponent/offset/level: every
    constraint is global, so "costs" holds one total. {key: {"costs": [total],
    "weight": float, "exponent": float, "offset": float, "clip": bool,
    "global": True, "level": "hard" | "soft", "reward": bool, "level_flippable": bool,
    "name": str, "label": str, "default_weight": float, "default_level": str}}.
    None = the default constraints; a pick list (as for solve) scores exactly those
    picks, each entry carrying its pick's knobs, the k-th pick of a name keyed
    "<name>#<k>" from the second on. The three *_diversity_cost rewards report a
    total <= 0 (reward=True, level_flippable=False). optimaru.conference_scheduling.constraints.score()
    turns it into a score. The only per-constraint surface."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order: 17 hard and 20 soft
    constraints at their default weights (talk_type_incompatibility_cost hard
    100000, ..., language_diversity_cost soft 10). Every one is scored by default
    (default_enabled); the three *_diversity_cost rewards have reward=True and
    level_flippable=False, every other constraint flips both ways."""
