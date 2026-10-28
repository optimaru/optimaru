"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`cs.solve(schedule)`) enables all thirty-seven constraints at the
provider weights. This module is the alternative path next to it: pick constraints,
change weights, flip hard/soft, and pass the list to `solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import conference_scheduling as cs
    from conference_scheduling.constraints import (
        ContentConflict, get_constraints, room_conflict,
    )

    cs.solve(schedule, constraints=get_constraints())           # defaults as-is
    cs.solve(schedule, constraints=get_constraints([            # picked
        room_conflict, ContentConflict(weight=500),
    ]))

Each constraint has a default instance at module level, so you can also pass a
list of just a few. Only listed constraints are scored — leave one out and its
term disappears from the score.

Every spec takes ``(weight=None, hard=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = component default.
- ``hard``     ``True``/``False`` flips the default level; ``None`` keeps it.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| **
               exponent``. 2.0 = squared, so concentrated violations grow much costlier.

Three terms (`audience_type_diversity`, `audience_level_diversity`,
`language_diversity`) are **rewards** with negative cost — raising their weight
raises the score. Everything else is a penalty.

Names (`name`) are the C++ component names in snake_case. The core derives them
mechanically, so there is no hand-maintained alias table — check them with
`cs.list_constraints()`. Module-level instance names use the same snake_case,
so they match the strings `list_constraints()` prints.
"""

import math
from dataclasses import dataclass
from typing import ClassVar, NamedTuple

__all__ = [
    "Constraint",
    "Score",
    "score",
    "TalkTypeCompatibility",
    "RoomUnavailableTimeslot",
    "RoomConflict",
    "SpeakerUnavailableTimeslot",
    "SpeakerConflict",
    "TalkPrerequisiteTalks",
    "TalkMutuallyExclusiveTalksTags",
    "ConsecutiveTalksPause",
    "CrowdControl",
    "SpeakerRequiredTimeslotTags",
    "SpeakerProhibitedTimeslotTags",
    "TalkRequiredTimeslotTags",
    "TalkProhibitedTimeslotTags",
    "SpeakerRequiredRoomTags",
    "SpeakerProhibitedRoomTags",
    "TalkRequiredRoomTags",
    "TalkProhibitedRoomTags",
    "ThemeTrackConflict",
    "ThemeTrackRoomStability",
    "SectorConflict",
    "AudienceTypeThemeTrackConflict",
    "ContentAudienceLevelFlowViolation",
    "ContentConflict",
    "SameDayTalks",
    "PopularTalks",
    "SpeakerPreferredTimeslotTags",
    "SpeakerUndesiredTimeslotTags",
    "TalkPreferredTimeslotTags",
    "TalkUndesiredTimeslotTags",
    "SpeakerPreferredRoomTags",
    "SpeakerUndesiredRoomTags",
    "TalkPreferredRoomTags",
    "TalkUndesiredRoomTags",
    "SpeakerMakespan",
    "AudienceTypeDiversity",
    "AudienceLevelDiversity",
    "LanguageDiversity",
    "talk_type_compatibility",
    "room_unavailable_timeslot",
    "room_conflict",
    "speaker_unavailable_timeslot",
    "speaker_conflict",
    "talk_prerequisite_talks",
    "talk_mutually_exclusive_talks_tags",
    "consecutive_talks_pause",
    "crowd_control",
    "speaker_required_timeslot_tags",
    "speaker_prohibited_timeslot_tags",
    "talk_required_timeslot_tags",
    "talk_prohibited_timeslot_tags",
    "speaker_required_room_tags",
    "speaker_prohibited_room_tags",
    "talk_required_room_tags",
    "talk_prohibited_room_tags",
    "theme_track_conflict",
    "theme_track_room_stability",
    "sector_conflict",
    "audience_type_theme_track_conflict",
    "content_audience_level_flow_violation",
    "content_conflict",
    "same_day_talks",
    "popular_talks",
    "speaker_preferred_timeslot_tags",
    "speaker_undesired_timeslot_tags",
    "talk_preferred_timeslot_tags",
    "talk_undesired_timeslot_tags",
    "speaker_preferred_room_tags",
    "speaker_undesired_room_tags",
    "talk_preferred_room_tags",
    "talk_undesired_room_tags",
    "speaker_makespan",
    "audience_type_diversity",
    "audience_level_diversity",
    "language_diversity",
    "get_constraints",
]


@dataclass(frozen=True)
class Constraint:
    """Spec selecting one constraint: weight / level / shaping."""

    weight: float | None = None  # None = component default weight
    hard: bool | None = None  # None = component default level
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class TalkTypeCompatibility(Constraint):
    """Talks placed in a timeslot/room that does not accept their talk type. A synthetic
    constraint — the Java quickstart never builds such assignments, while here the
    same prohibition is expressed as a cost. Default: hard, weight 100'000."""

    name = "talk_type_compatibility"


class RoomUnavailableTimeslot(Constraint):
    """Talks placed in a room that is closed in that timeslot. Default: hard, weight 100'000."""

    name = "room_unavailable_timeslot"


class RoomConflict(Constraint):
    """Two talks in overlapping timeslots sharing a room. Valued by overlapping minutes. Default: hard, weight 1'000."""

    name = "room_conflict"


class SpeakerUnavailableTimeslot(Constraint):
    """Talks placed in a timeslot where one of their speakers is unavailable. Default: hard, weight 100."""

    name = "speaker_unavailable_timeslot"


class SpeakerConflict(Constraint):
    """Two overlapping talks sharing a speaker. Valued by overlapping minutes per
    shared speaker — one person cannot be in two places at once. Default: hard, weight 10."""

    name = "speaker_conflict"


class TalkPrerequisiteTalks(Constraint):
    """A talk scheduled before (or overlapping) a talk marked as its prerequisite. Valued
    by the sum of both talks' durations. Default: hard, weight 10."""

    name = "talk_prerequisite_talks"


class TalkMutuallyExclusiveTalksTags(Constraint):
    """Two overlapping talks with the same mutually exclusive tag. Valued by shared
    tags × overlap. Default: hard, weight 1."""

    name = "talk_mutually_exclusive_talks_tags"


class ConsecutiveTalksPause(Constraint):
    """Two talks of one speaker back to back without the minimum pause. Valued by
    the sum of both durations. Default: hard, weight 1."""

    name = "consecutive_talks_pause"


class CrowdControl(Constraint):
    """A crowd-risk talk overlapping two or more other risky talks in the same
    timeslot — the hallway overflows. Default: hard, weight 1."""

    name = "crowd_control"


class SpeakerRequiredTimeslotTags(Constraint):
    """Tags a speaker requires that the assigned timeslot lacks. Valued by missing
    tags × duration. Default: hard, weight 1."""

    name = "speaker_required_timeslot_tags"


class SpeakerProhibitedTimeslotTags(Constraint):
    """Tags a speaker prohibits that the assigned timeslot carries. Default: hard, weight 1."""

    name = "speaker_prohibited_timeslot_tags"


class TalkRequiredTimeslotTags(Constraint):
    """Tags a talk requires that the assigned timeslot lacks. Default: hard, weight 1."""

    name = "talk_required_timeslot_tags"


class TalkProhibitedTimeslotTags(Constraint):
    """Tags a talk prohibits that the assigned timeslot carries. Default: hard, weight 1."""

    name = "talk_prohibited_timeslot_tags"


class SpeakerRequiredRoomTags(Constraint):
    """Tags a speaker requires that the assigned room lacks. Default: hard, weight 1."""

    name = "speaker_required_room_tags"


class SpeakerProhibitedRoomTags(Constraint):
    """Tags a speaker prohibits that the assigned room carries. Default: hard, weight 1."""

    name = "speaker_prohibited_room_tags"


class TalkRequiredRoomTags(Constraint):
    """Tags a talk requires that the assigned room lacks. Default: hard, weight 1."""

    name = "talk_required_room_tags"


class TalkProhibitedRoomTags(Constraint):
    """Tags a talk prohibits that the assigned room carries. Default: hard, weight 1."""

    name = "talk_prohibited_room_tags"


class ThemeTrackConflict(Constraint):
    """Two overlapping talks of the same theme track. Valued by shared themes × overlap —
    attendees following that track must give one up. Default: soft, weight 10."""

    name = "theme_track_conflict"


class ThemeTrackRoomStability(Constraint):
    """Two talks of one theme track on the same day in different rooms. The audience
    following that track has to pack up and move. Default: soft, weight 10."""

    name = "theme_track_room_stability"


class SectorConflict(Constraint):
    """Two overlapping talks targeting the same sector. Default: soft, weight 10."""

    name = "sector_conflict"


class AudienceTypeThemeTrackConflict(Constraint):
    """Two overlapping talks sharing **both** a theme track and an audience type. Default: soft, weight 1."""

    name = "audience_type_theme_track_conflict"


class ContentAudienceLevelFlowViolation(Constraint):
    """An introductory talk scheduled after (or overlapping) an advanced talk of the same
    content — the audience cannot follow from easy to hard. Default: soft, weight 10."""

    name = "content_audience_level_flow_violation"


class ContentConflict(Constraint):
    """Two overlapping talks of the same content. The most expensive soft rule — the same
    audience wants both. Default: soft, weight 100."""

    name = "content_conflict"


class SameDayTalks(Constraint):
    """Two talks sharing content or a theme track placed on **different** days.
    Attendees have to come twice. Default: soft, weight 10."""

    name = "same_day_talks"


class PopularTalks(Constraint):
    """A less popular talk getting a bigger room than a more popular one. Default: soft, weight 10."""

    name = "popular_talks"


class SpeakerPreferredTimeslotTags(Constraint):
    """Tags a speaker prefers that the assigned timeslot lacks. Default: soft, weight 20."""

    name = "speaker_preferred_timeslot_tags"


class SpeakerUndesiredTimeslotTags(Constraint):
    """Tags a speaker would rather avoid that the assigned timeslot carries. Default: soft, weight 20."""

    name = "speaker_undesired_timeslot_tags"


class TalkPreferredTimeslotTags(Constraint):
    """Tags a talk prefers that the assigned timeslot lacks. Default: soft, weight 20."""

    name = "talk_preferred_timeslot_tags"


class TalkUndesiredTimeslotTags(Constraint):
    """Tags a talk would rather avoid that the assigned timeslot carries. Default: soft, weight 20."""

    name = "talk_undesired_timeslot_tags"


class SpeakerPreferredRoomTags(Constraint):
    """Tags a speaker prefers that the assigned room lacks. Default: soft, weight 20."""

    name = "speaker_preferred_room_tags"


class SpeakerUndesiredRoomTags(Constraint):
    """Tags a speaker would rather avoid that the assigned room carries. Default: soft, weight 20."""

    name = "speaker_undesired_room_tags"


class TalkPreferredRoomTags(Constraint):
    """Tags a talk prefers that the assigned room lacks. Default: soft, weight 20."""

    name = "talk_preferred_room_tags"


class TalkUndesiredRoomTags(Constraint):
    """Tags a talk would rather avoid that the assigned room carries. Default: soft, weight 20."""

    name = "talk_undesired_room_tags"


class SpeakerMakespan(Constraint):
    """Speakers whose talks spread over more than two days. Valued by extra days × working
    days — keeps speakers from being held for the whole conference. Default: soft, weight 20."""

    name = "speaker_makespan"


class AudienceTypeDiversity(Constraint):
    """**Reward**: a timeslot serving several audience types at once. Valued by audience
    types × slot duration (negative cost, so it raises the score). Default: soft, weight 1."""

    name = "audience_type_diversity"


class AudienceLevelDiversity(Constraint):
    """**Reward**: a timeslot mixing audience levels — beginners and experts both
    have somewhere to go. Default: soft, weight 1."""

    name = "audience_level_diversity"


class LanguageDiversity(Constraint):
    """**Reward**: a timeslot running talks in different languages together. Default: soft, weight 10."""

    name = "language_diversity"


#: Instances at the default weights. Reference them directly when picking.
talk_type_compatibility = TalkTypeCompatibility()
room_unavailable_timeslot = RoomUnavailableTimeslot()
room_conflict = RoomConflict()
speaker_unavailable_timeslot = SpeakerUnavailableTimeslot()
speaker_conflict = SpeakerConflict()
talk_prerequisite_talks = TalkPrerequisiteTalks()
talk_mutually_exclusive_talks_tags = TalkMutuallyExclusiveTalksTags()
consecutive_talks_pause = ConsecutiveTalksPause()
crowd_control = CrowdControl()
speaker_required_timeslot_tags = SpeakerRequiredTimeslotTags()
speaker_prohibited_timeslot_tags = SpeakerProhibitedTimeslotTags()
talk_required_timeslot_tags = TalkRequiredTimeslotTags()
talk_prohibited_timeslot_tags = TalkProhibitedTimeslotTags()
speaker_required_room_tags = SpeakerRequiredRoomTags()
speaker_prohibited_room_tags = SpeakerProhibitedRoomTags()
talk_required_room_tags = TalkRequiredRoomTags()
talk_prohibited_room_tags = TalkProhibitedRoomTags()
theme_track_conflict = ThemeTrackConflict()
theme_track_room_stability = ThemeTrackRoomStability()
sector_conflict = SectorConflict()
audience_type_theme_track_conflict = AudienceTypeThemeTrackConflict()
content_audience_level_flow_violation = ContentAudienceLevelFlowViolation()
content_conflict = ContentConflict()
same_day_talks = SameDayTalks()
popular_talks = PopularTalks()
speaker_preferred_timeslot_tags = SpeakerPreferredTimeslotTags()
speaker_undesired_timeslot_tags = SpeakerUndesiredTimeslotTags()
talk_preferred_timeslot_tags = TalkPreferredTimeslotTags()
talk_undesired_timeslot_tags = TalkUndesiredTimeslotTags()
speaker_preferred_room_tags = SpeakerPreferredRoomTags()
speaker_undesired_room_tags = SpeakerUndesiredRoomTags()
talk_preferred_room_tags = TalkPreferredRoomTags()
talk_undesired_room_tags = TalkUndesiredRoomTags()
speaker_makespan = SpeakerMakespan()
audience_type_diversity = AudienceTypeDiversity()
audience_level_diversity = AudienceLevelDiversity()
language_diversity = LanguageDiversity()

#: Default set — same order as `cs.list_constraints()` (unit first, global after).
_DEFAULTS: list[Constraint] = [
    talk_type_compatibility,
    room_unavailable_timeslot,
    room_conflict,
    speaker_unavailable_timeslot,
    speaker_conflict,
    talk_prerequisite_talks,
    talk_mutually_exclusive_talks_tags,
    consecutive_talks_pause,
    crowd_control,
    speaker_required_timeslot_tags,
    speaker_prohibited_timeslot_tags,
    talk_required_timeslot_tags,
    talk_prohibited_timeslot_tags,
    speaker_required_room_tags,
    speaker_prohibited_room_tags,
    talk_required_room_tags,
    talk_prohibited_room_tags,
    theme_track_conflict,
    theme_track_room_stability,
    sector_conflict,
    audience_type_theme_track_conflict,
    content_audience_level_flow_violation,
    content_conflict,
    same_day_talks,
    popular_talks,
    speaker_preferred_timeslot_tags,
    speaker_undesired_timeslot_tags,
    talk_preferred_timeslot_tags,
    talk_undesired_timeslot_tags,
    speaker_preferred_room_tags,
    speaker_undesired_room_tags,
    talk_preferred_room_tags,
    talk_undesired_room_tags,
    speaker_makespan,
    audience_type_diversity,
    audience_level_diversity,
    language_diversity,
]


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all thirty-seven default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([room_conflict, ContentConflict(weight=500)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # Same contract as C++ applyConstraints: an empty list means "score
        # nothing", which is most likely a mistake. A filter that happens to come
        # out empty must not silently flip to "score everything".
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """(hard, soft) returned by `score()`. Both are penalties, so <= 0."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """Sign-preserving power. Rewards (negative costs) never flip into penalties."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `cs.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each unit (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = cs.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each unit contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per unit** before summing (the same order as the
    engine's applyRaw; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    hard = 0
    soft = 0.0
    for spec in get_constraints(constraints):
        entry = costs.get(spec.name)
        if entry is None:
            raise ValueError(
                f"unknown constraint {spec.name!r}; available: " + ", ".join(repr(name) for name in sorted(costs))
            )
        weight = entry["weight"] if spec.weight is None else spec.weight
        is_hard = entry["hard"] if spec.hard is None else spec.hard
        exponent = spec.exponent
        for cost in entry["costs"]:
            value = weight * _shape(cost, exponent)
            if is_hard:
                # Round per unit contribution like the engine (half-away-from-zero
                # like llround; Python's round is banker's rounding).
                hard += int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
