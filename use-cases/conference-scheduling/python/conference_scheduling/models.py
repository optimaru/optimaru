"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Timeslot/Room/Speaker/Talk/ConferenceSchedule and SolverConfig
1:1. Use it to store or transfer the engine shape as-is (debug dumps, regression
snapshots, ...)::

    from conference_scheduling.models import ConferenceScheduleModel

    model = ConferenceScheduleModel.from_core(schedule)  # engine -> pydantic
    clone = ConferenceScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                          # pydantic -> engine

`pydantic` is an optional dependency (`pip install conference-scheduling[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field

from . import _core

__all__ = [
    "ConferenceScheduleModel",
    "RoomModel",
    "SolveConfig",
    "SpeakerModel",
    "TalkModel",
    "TimeslotModel",
    "with_initial_solution",
]


class TimeslotModel(BaseModel):
    """A time slot. In minutes (shared origin; midnight boundaries are multiples of 1440)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    start_minute: int = Field(ge=0)
    end_minute: int = Field(ge=0)
    talk_types: list[int]
    tags: list[str] = Field(default_factory=list)


class RoomModel(BaseModel):
    """A room. unavailable_timeslots are timeslot indices."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str = ""
    capacity: int = 0
    talk_types: list[int] = Field(default_factory=list)
    unavailable_timeslots: list[int] = Field(default_factory=list)
    tags: list[str] = Field(default_factory=list)


class SpeakerModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: str
    name: str = ""
    unavailable_timeslots: list[int] = Field(default_factory=list)
    required_timeslot_tags: list[str] = Field(default_factory=list)
    preferred_timeslot_tags: list[str] = Field(default_factory=list)
    prohibited_timeslot_tags: list[str] = Field(default_factory=list)
    undesired_timeslot_tags: list[str] = Field(default_factory=list)
    required_room_tags: list[str] = Field(default_factory=list)
    preferred_room_tags: list[str] = Field(default_factory=list)
    prohibited_room_tags: list[str] = Field(default_factory=list)
    undesired_room_tags: list[str] = Field(default_factory=list)


class TalkModel(BaseModel):
    """Planning entity. slot_room is the composite timeslot*room variable (-1 = unassigned)."""

    model_config = ConfigDict(extra="forbid")

    code: str
    title: str = ""
    talk_type: int = 0
    speakers: list[int] = Field(default_factory=list)
    theme_track_tags: list[str] = Field(default_factory=list)
    sector_tags: list[str] = Field(default_factory=list)
    audience_types: list[str] = Field(default_factory=list)
    audience_level: int = 0
    content_tags: list[str] = Field(default_factory=list)
    language: str = "en"
    required_timeslot_tags: list[str] = Field(default_factory=list)
    preferred_timeslot_tags: list[str] = Field(default_factory=list)
    prohibited_timeslot_tags: list[str] = Field(default_factory=list)
    undesired_timeslot_tags: list[str] = Field(default_factory=list)
    required_room_tags: list[str] = Field(default_factory=list)
    preferred_room_tags: list[str] = Field(default_factory=list)
    prohibited_room_tags: list[str] = Field(default_factory=list)
    undesired_room_tags: list[str] = Field(default_factory=list)
    mutually_exclusive_talks_tags: list[str] = Field(default_factory=list)
    prerequisite_talks: list[int] = Field(default_factory=list)
    favorite_count: int = 0
    crowd_control_risk: int = 0
    slot_room: int = -1
    pinned: bool = False


class ConferenceScheduleModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field."""

    model_config = ConfigDict(extra="forbid")

    name: str
    talk_types: list[str]
    timeslots: list[TimeslotModel]
    rooms: list[RoomModel]
    speakers: list[SpeakerModel] = Field(default_factory=list)
    talks: list[TalkModel] = Field(default_factory=list)
    minimum_consecutive_talks_pause_in_minutes: int = 30
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.ConferenceSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        return _core.ConferenceSchedule(
            name=self.name,
            talk_types=list(self.talk_types),
            timeslots=[_core.Timeslot(**t.model_dump()) for t in self.timeslots],
            rooms=[_core.Room(**r.model_dump()) for r in self.rooms],
            speakers=[_core.Speaker(**s.model_dump()) for s in self.speakers],
            talks=[_core.Talk(**t.model_dump()) for t in self.talks],
            minimum_consecutive_talks_pause_in_minutes=self.minimum_consecutive_talks_pause_in_minutes,
        )

    @classmethod
    def from_core(cls, schedule: "_core.ConferenceSchedule") -> "ConferenceScheduleModel":
        """engine object -> pydantic (including the current assignment and score)."""
        return cls(
            name=schedule.name,
            talk_types=list(schedule.talk_types),
            timeslots=[
                TimeslotModel(
                    id=t.id,
                    start_minute=t.start_minute,
                    end_minute=t.end_minute,
                    talk_types=list(t.talk_types),
                    tags=list(t.tags),
                )
                for t in schedule.timeslots
            ],
            rooms=[
                RoomModel(
                    id=r.id,
                    name=r.name,
                    capacity=r.capacity,
                    talk_types=list(r.talk_types),
                    unavailable_timeslots=list(r.unavailable_timeslots),
                    tags=list(r.tags),
                )
                for r in schedule.rooms
            ],
            speakers=[
                SpeakerModel(
                    id=s.id,
                    name=s.name,
                    unavailable_timeslots=list(s.unavailable_timeslots),
                    required_timeslot_tags=list(s.required_timeslot_tags),
                    preferred_timeslot_tags=list(s.preferred_timeslot_tags),
                    prohibited_timeslot_tags=list(s.prohibited_timeslot_tags),
                    undesired_timeslot_tags=list(s.undesired_timeslot_tags),
                    required_room_tags=list(s.required_room_tags),
                    preferred_room_tags=list(s.preferred_room_tags),
                    prohibited_room_tags=list(s.prohibited_room_tags),
                    undesired_room_tags=list(s.undesired_room_tags),
                )
                for s in schedule.speakers
            ],
            talks=[
                TalkModel(
                    code=t.code,
                    title=t.title,
                    talk_type=t.talk_type,
                    speakers=list(t.speakers),
                    theme_track_tags=list(t.theme_track_tags),
                    sector_tags=list(t.sector_tags),
                    audience_types=list(t.audience_types),
                    audience_level=t.audience_level,
                    content_tags=list(t.content_tags),
                    language=t.language,
                    required_timeslot_tags=list(t.required_timeslot_tags),
                    preferred_timeslot_tags=list(t.preferred_timeslot_tags),
                    prohibited_timeslot_tags=list(t.prohibited_timeslot_tags),
                    undesired_timeslot_tags=list(t.undesired_timeslot_tags),
                    required_room_tags=list(t.required_room_tags),
                    preferred_room_tags=list(t.preferred_room_tags),
                    prohibited_room_tags=list(t.prohibited_room_tags),
                    undesired_room_tags=list(t.undesired_room_tags),
                    mutually_exclusive_talks_tags=list(t.mutually_exclusive_talks_tags),
                    prerequisite_talks=list(t.prerequisite_talks),
                    favorite_count=t.favorite_count,
                    crowd_control_risk=t.crowd_control_risk,
                    slot_room=t.slot_room,
                    pinned=t.pinned,
                )
                for t in schedule.talks
            ],
            minimum_consecutive_talks_pause_in_minutes=schedule.minimum_consecutive_talks_pause_in_minutes,
            score_hard=schedule.score.hard,
            score_soft=schedule.score.soft,
        )


class SolveConfig(BaseModel):
    """Serializable mirror of `_core.SolverConfig`."""

    model_config = ConfigDict(extra="forbid")

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    # 0 = automatic (clamp(8 * talk count, 800, 3200)). A positive value is used as-is.
    late_acceptance_size: int = Field(default=0, ge=0)
    # Number of consecutive local search runs sharing one time limit
    # (each segment restarts from the previous segment's best; see cs/solver.hpp).
    restart_segments: int = Field(default=8, ge=1)
    stop_at_first_feasible: bool = False
    best_score_limit: tuple[int, float] | None = None
    unimproved_spent_limit_seconds: float = Field(default=0.0, ge=0)
    step_count_limit: int = Field(default=0, ge=0)
    unimproved_step_count_limit: int = Field(default=0, ge=0)
    full_assert: bool = False

    ruin_recreate: bool = True
    ruin_recreate_unimproved_step_trigger: int = Field(default=5000, ge=0)
    ruin_recreate_min_ruin_fraction: float = Field(default=0.10, ge=0)
    ruin_recreate_max_ruin_fraction: float = Field(default=0.30, ge=0)
    ruin_recreate_max_ruin_count: int = Field(default=60, ge=1)
    ruin_recreate_operator_decay: float = Field(default=0.8, ge=0, le=1)
    ruin_recreate_rewards: list[float] = Field(
        default_factory=lambda: [33.0, 9.0, 3.0, 0.0],
        min_length=4,
        max_length=4,
    )
    ruin_recreate_polish_steps: int = Field(default=5000, ge=0)
    # Probability of also using ruin & recreate as an ordinary move judged by the acceptor.
    # Negative = automatic (0.2 / slotRoomCount, the budget-share rule — see benchmarks/school-timetabling/RESULTS.md),
    # 0 = off.
    ruin_recreate_as_move_weight: float = Field(default=-1.0)
    ruin_recreate_as_move_max_ruin: int = Field(default=12, ge=1)
    # Probability of a best-improvement descent over one nearby pair (default 0 = off).
    descent_weight: float = Field(default=0.0, ge=0)
    # Probability of picking move targets among talks with hard violations (0 = off).
    # Measured to help only around scaled:120, so off by default — see the README.
    hot_talk_probability: float = Field(default=0.0, ge=0, le=1)

    change_move_weight: float = Field(default=0.75, ge=0)
    swap_move_weight: float = Field(default=0.25, ge=0)
    # Probability of steering change moves to type-compatible (timeslot, room) cells
    # (replaces Java's per-TalkType value range; see cs/solver.hpp).
    compatible_value_probability: float = Field(default=0.9, ge=0, le=1)

    def to_core(self) -> "_core.SolverConfig":
        cfg = _core.SolverConfig()
        cfg.spent_limit_seconds = self.time_limit_seconds
        cfg.random_seed = self.random_seed
        cfg.parallel_solver_count = self.parallel_solver_count
        cfg.late_acceptance_size = self.late_acceptance_size  # 0 = auto
        cfg.restart_segments = self.restart_segments
        cfg.unimproved_spent_limit_seconds = self.unimproved_spent_limit_seconds
        cfg.step_count_limit = self.step_count_limit
        cfg.unimproved_step_count_limit = self.unimproved_step_count_limit
        cfg.full_assert = self.full_assert
        cfg.ruin_recreate = self.ruin_recreate
        cfg.ruin_recreate_unimproved_step_trigger = self.ruin_recreate_unimproved_step_trigger
        cfg.ruin_recreate_min_ruin_fraction = self.ruin_recreate_min_ruin_fraction
        cfg.ruin_recreate_max_ruin_fraction = self.ruin_recreate_max_ruin_fraction
        cfg.ruin_recreate_max_ruin_count = self.ruin_recreate_max_ruin_count
        cfg.ruin_recreate_operator_decay = self.ruin_recreate_operator_decay
        cfg.ruin_recreate_rewards = list(self.ruin_recreate_rewards)
        cfg.ruin_recreate_polish_steps = self.ruin_recreate_polish_steps
        cfg.ruin_recreate_as_move_weight = self.ruin_recreate_as_move_weight
        cfg.ruin_recreate_as_move_max_ruin = self.ruin_recreate_as_move_max_ruin
        cfg.descent_weight = self.descent_weight
        cfg.hot_talk_probability = self.hot_talk_probability
        cfg.change_move_weight = self.change_move_weight
        cfg.swap_move_weight = self.swap_move_weight
        cfg.compatible_value_probability = self.compatible_value_probability
        if self.stop_at_first_feasible:
            cfg.best_score_limit = (0, -1e18)
        if self.best_score_limit is not None:
            cfg.best_score_limit = self.best_score_limit
        return cfg


def _solution_dict(solution) -> dict:
    if isinstance(solution, BaseModel):
        return solution.model_dump()
    if isinstance(solution, dict):
        return solution
    raise ValueError(f"solution must be a solved model or a dict of that shape: {type(solution)!r}")


def _entities_by_id(solved: dict, name: str, ids: list, key: str = "id") -> dict:
    """`solved[name]` by id. Rejects an id repeated on either side and an id outside `ids`."""
    entities = solved.get(name)
    if not isinstance(entities, list):
        raise ValueError(f"solution has no {name!r} list")
    known = set(ids)
    if len(known) != len(ids):
        raise ValueError(f"problem repeats an id in {name!r}: {next(i for i in ids if ids.count(i) > 1)!r}")
    by_id: dict = {}
    for entity in entities:
        entity_id = entity.get(key) if isinstance(entity, dict) else None
        if not isinstance(entity_id, (str, int)) or entity_id not in known:
            raise ValueError(f"solution has an unknown id in {name!r}: {entity_id!r}")
        if entity_id in by_id:
            raise ValueError(f"solution repeats an id in {name!r}: {entity_id!r}")
        by_id[entity_id] = entity
    return by_id


def _require_same_order(solved: dict, name: str, ids: list, key: str = "id") -> None:
    """A positional value only means the same thing over the same list. `solved` may omit the list."""
    values = solved.get(name)
    if values is None:
        return
    if not isinstance(values, list) or [v.get(key) if isinstance(v, dict) else None for v in values] != ids:
        raise ValueError(f"solution {name!r} list differs from the problem's (same entries, same order required)")


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns (`ConferenceScheduleModel.from_core`), or a plain
    dict of that shape (the JSON the API returned). Talks are matched by code. A talk
    left out becomes unassigned and the construction heuristic places it, and only
    it, so a complete solution starts the search exactly from it. `problem` is not
    mutated. Pinned talks keep their value on `problem`, whatever `solution` says.

    `slot_room` is the composite `timeslot_index * room_count + room_index`, so it is
    only valid for the same timeslots and rooms in the same order: ValueError if
    `solution` lists different ones.

    Raises ValueError naming the talk for an unknown or repeated code and for a
    `slot_room` outside -1..slot_room_count - 1 (the engine does not range-check).
    """
    if not isinstance(problem, ConferenceScheduleModel):
        raise TypeError(f"problem must be a ConferenceScheduleModel: {type(problem)!r}")
    solved = _solution_dict(solution)
    _require_same_order(solved, "timeslots", [t.id for t in problem.timeslots])
    _require_same_order(solved, "rooms", [r.id for r in problem.rooms])
    talks = _entities_by_id(solved, "talks", [t.code for t in problem.talks], key="code")
    value_count = len(problem.timeslots) * len(problem.rooms)
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for talk in seeded.talks:
        if not talk.pinned:
            talk.slot_room = talks.get(talk.code, {}).get("slot_room", -1)
        value = talk.slot_room
        if not isinstance(value, int) or isinstance(value, bool) or not -1 <= value < value_count:
            raise ValueError(f"talk {talk.code!r}: slot_room {value!r} is out of range -1..{value_count - 1}")
    return seeded
