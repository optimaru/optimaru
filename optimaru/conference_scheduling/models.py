"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Timeslot/Room/Speaker/Talk/ConferenceSchedule and SolverConfig
1:1. Use it to store or transfer the engine shape as-is (debug dumps, regression
snapshots, ...)::

    from optimaru.conference_scheduling.models import ConferenceScheduleModel

    model = ConferenceScheduleModel.from_core(schedule)  # engine -> pydantic
    clone = ConferenceScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                          # pydantic -> engine
"""

from pydantic import Field, model_validator

from .._constraint import ParameterUse, is_set
from .._models import (
    AcceptanceGroup,
    SolveConfigBase,
    SolveRequest,
    StrictModel,
    entities_by_id,
    require_same_order,
    solution_dict,
    solve_model_request,
    valid_positions,
)
from . import _core

__all__ = [
    "ConferenceScheduleModel",
    "ConferenceScheduleRequest",
    "RoomModel",
    "SolveConfig",
    "SpeakerModel",
    "TalkModel",
    "TimeslotModel",
    "parameter_uses",
    "solve_request",
    "with_initial_solution",
]


class TimeslotModel(StrictModel):
    """A time slot. In minutes (shared origin; midnight boundaries are multiples of 1440)."""

    id: str
    start_minute: int = Field(ge=0)
    end_minute: int = Field(ge=0)
    talk_types: list[int]
    tags: list[str] = Field(default_factory=list)


class RoomModel(StrictModel):
    """A room. unavailable_timeslots are timeslot indices."""

    id: str
    name: str = ""
    capacity: int = 0
    talk_types: list[int] = Field(default_factory=list)
    unavailable_timeslots: list[int] = Field(default_factory=list)
    tags: list[str] = Field(default_factory=list)


class SpeakerModel(StrictModel):
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


class TalkModel(StrictModel):
    """Slot_room is the composite timeslot*room variable (-1 = unassigned)."""

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


class ConferenceScheduleModel(StrictModel):
    """Solution mirror. score is a read-only result, kept as a separate field."""

    name: str
    talk_types: list[str]
    timeslots: list[TimeslotModel]
    rooms: list[RoomModel]
    speakers: list[SpeakerModel] = Field(default_factory=list)
    talks: list[TalkModel] = Field(default_factory=list)
    minimum_consecutive_talks_pause_in_minutes: int = 30
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def _check_indices(self):
        """Raw positions the engine reads without a range check. The positions listed on
        timeslots, rooms and speakers and in `prerequisite_talks` it skips when out of range."""
        type_count = len(self.talk_types)
        speaker_count = len(self.speakers)
        value_count = len(self.timeslots) * len(self.rooms)
        for talk in self.talks:
            if not 0 <= talk.talk_type < type_count:
                raise ValueError(
                    f"talk {talk.code!r}: talk_type {talk.talk_type!r} is out of range {valid_positions(type_count)}"
                )
            for speaker in talk.speakers:
                if not 0 <= speaker < speaker_count:
                    raise ValueError(
                        f"talk {talk.code!r}: speaker {speaker!r} is out of range {valid_positions(speaker_count)}"
                    )
            if not -1 <= talk.slot_room < value_count:
                raise ValueError(
                    f"talk {talk.code!r}: slot_room {talk.slot_room!r} is out of range -1..{value_count - 1}"
                )
        return self

    def to_core(self) -> "_core.ConferenceSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        self._check_indices()  # the fields stay mutable after validation
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


class _ConferenceAcceptance(AcceptanceGroup):
    # 0 = automatic (clamp(8 * talk count, 800, 3200)). A positive value is used as-is.
    # Consecutive local search runs sharing one time limit (each segment restarts from the
    # previous segment's best; see cs/solver.hpp).
    restart_segments: int = Field(default=8, ge=1)


class _ConferenceStrategies(StrictModel):
    # Ruin & recreate also as an ordinary move judged by the acceptor. Negative = automatic
    # (0.2 / slotRoomCount, the budget-share rule; see benchmarks/school-timetabling/RESULTS.md), 0 = off.
    ruin_recreate_weight: float = -1.0
    ruin_recreate_max_ruin: int = Field(default=12, ge=1)
    # A best-improvement descent over one nearby pair (default 0 = off).
    descent_weight: float = Field(default=0.0, ge=0)


class _ConferenceSelection(StrictModel):
    # Steer change moves to type-compatible (timeslot, room) cells (see cs/solver.hpp).
    compatible_value_probability: float = Field(default=0.9, ge=0, le=1)
    # Pick move targets among talks with hard violations (0 = off). Measured to help only around
    # scaled:120 (a 120-talk instance of the quality bench's former generator), so off by default.
    hot_talk_probability: float = Field(default=0.0, ge=0, le=1)


class SolveConfig(SolveConfigBase):
    """Serializable mirror of `_core.SolverConfig`: the shared groups, with conference scheduling's own settings in them."""

    acceptance: _ConferenceAcceptance = Field(default_factory=_ConferenceAcceptance)
    strategies: _ConferenceStrategies = Field(default_factory=_ConferenceStrategies)
    selection: _ConferenceSelection = Field(default_factory=_ConferenceSelection)

    _first_feasible: tuple = (0, float("-inf"))

    def to_core(self) -> "_core.SolverConfig":
        return self._apply_to(_core.SolverConfig())


_TAG_KINDS = ("required", "preferred", "prohibited", "undesired")
# The fields that act only through a cost: (field, default, the costs that read it).
_TALK_COSTS = (
    (
        "speakers",
        [],
        (
            "speaker_conflict_cost",
            "speaker_unavailability_cost",
            "consecutive_talks_pause_cost",
            "speaker_makespan_cost",
        )
        + tuple(f"speaker_{kind}_{place}_tags_cost" for kind in _TAG_KINDS for place in ("timeslot", "room")),
    ),
    (
        "theme_track_tags",
        [],
        (
            "theme_track_conflict_cost",
            "theme_track_room_change_cost",
            "audience_type_theme_track_conflict_cost",
            "different_day_talks_cost",
        ),
    ),
    ("sector_tags", [], ("sector_conflict_cost",)),
    ("audience_types", [], ("audience_type_theme_track_conflict_cost", "audience_type_diversity_cost")),
    ("audience_level", 0, ("content_audience_level_flow_violation_cost", "audience_level_diversity_cost")),
    (
        "content_tags",
        [],
        ("content_conflict_cost", "content_audience_level_flow_violation_cost", "different_day_talks_cost"),
    ),
    ("language", "en", ("language_diversity_cost",)),
    ("mutually_exclusive_talks_tags", [], ("talk_mutually_exclusive_talks_tags_cost",)),
    ("prerequisite_talks", [], ("talk_prerequisite_talks_cost",)),
    ("favorite_count", 0, ("popular_talks_room_size_cost",)),
    ("crowd_control_risk", 0, ("crowd_control_risk_cost",)),
) + tuple(
    (f"{kind}_{place}_tags", [], (f"talk_{kind}_{place}_tags_cost",))
    for kind in _TAG_KINDS
    for place in ("timeslot", "room")
)
_SPEAKER_COSTS = (("unavailable_timeslots", [], ("speaker_unavailability_cost",)),) + tuple(
    (f"{kind}_{place}_tags", [], (f"speaker_{kind}_{place}_tags_cost",))
    for kind in _TAG_KINDS
    for place in ("timeslot", "room")
)
_ROOM_COSTS = (
    ("capacity", 0, ("popular_talks_room_size_cost",)),
    ("talk_types", [], ("talk_type_incompatibility_cost",)),
    ("unavailable_timeslots", [], ("room_unavailability_cost",)),
    ("tags", [], tuple(f"{who}_{kind}_room_tags_cost" for who in ("talk", "speaker") for kind in _TAG_KINDS)),
)
_TIMESLOT_COSTS = (
    ("tags", [], tuple(f"{who}_{kind}_timeslot_tags_cost" for who in ("talk", "speaker") for kind in _TAG_KINDS)),
)


def parameter_uses(schedule: ConferenceScheduleModel) -> list[ParameterUse]:
    """The parameters `schedule` sets away from their defaults that act only through a cost.

    `solve` refuses a model whose selection holds none of a parameter's costs
    (`optimaru._constraint.require_costs`); the table is "Parameters and their costs" in
    docs/use-cases/README.md. The timeslots' `talk_types`, their times and a talk's
    `talk_type` describe the problem itself and are not checked; neither are pins."""
    uses = []
    for kind, items, key, table in (
        ("talk", schedule.talks, "code", _TALK_COSTS),
        ("speaker", schedule.speakers, "id", _SPEAKER_COSTS),
        ("room", schedule.rooms, "id", _ROOM_COSTS),
        ("timeslot", schedule.timeslots, "id", _TIMESLOT_COSTS),
    ):
        for item in items:
            for field, default, costs in table:
                if is_set(getattr(item, field), default):
                    uses.append(ParameterUse(f"{kind} {getattr(item, key)}", field, default, costs))
    # 0 asks for no pause at all, so it needs no cost either.
    if schedule.minimum_consecutive_talks_pause_in_minutes not in (30, 0):
        uses.append(
            ParameterUse(
                "the schedule", "minimum_consecutive_talks_pause_in_minutes", 30, ("consecutive_talks_pause_cost",)
            )
        )
    return uses


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
    solved = solution_dict(solution)
    require_same_order(solved, "timeslots", [t.id for t in problem.timeslots])
    require_same_order(solved, "rooms", [r.id for r in problem.rooms])
    talks = entities_by_id(solved, "talks", [t.code for t in problem.talks], key="code")
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


class ConferenceScheduleRequest(SolveRequest):
    """The conference schedule with the settings of one solve (see `SolveRequest` for those)."""

    model_input: ConferenceScheduleModel


def solve_request(request: ConferenceScheduleRequest, *, constraints=None) -> ConferenceScheduleModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)
