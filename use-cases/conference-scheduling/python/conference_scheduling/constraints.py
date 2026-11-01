"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`cs.solve(schedule)`) enables all thirty-seven constraints at their
default weights (the Java provider's). This module is the alternative path next to it:
pick constraints, change weights, change levels, shape the cost, and pass the list to
`solve(schedule, constraints=...)`.

A constraint list reads one way: one class call per constraint::

    import conference_scheduling as cs
    from conference_scheduling.constraints import (
        ContentConflictCost, RoomConflictCost, get_constraints,
    )

    cs.solve(schedule, constraints=get_constraints())           # the defaults as-is
    cs.solve(schedule, constraints=[                             # picked
        RoomConflictCost(), ContentConflictCost(weight=500),
    ])

Only listed constraints are scored — leave one out and its term disappears from the
score. (The module also carries ready-made default instances: ``room_conflict_cost``
is ``RoomConflictCost()``, and so on.)

Every spec takes five knobs:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (the Java provider's weight; ``cs.constraint_info()`` reports
               every default level and weight).
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level: ``level="medium"`` raises ValueError.
- ``exponent`` sign-preserving power shaping: ``weight * sign(c) * |c| ** exponent``.
               Every constraint here is global, so it shapes the one total. 2.0 =
               squared, so concentrated violations grow much costlier.
- ``offset``   (keyword) shifts that shaping of the total; 0.0 = none.
- ``clip``     (keyword) ``True`` (the default) counts only what lies beyond the
               offset, ``sign(c) * max(0, |c| - offset) ** exponent``; ``False``
               shapes ``c - offset`` as it is, so a total below the offset (a 0
               included) lowers the penalty.

The same constraint may be picked several times; each pick is scored on its own with
its own knobs (its level included) and the picks add up. The content-conflict minutes
plus their square beyond one hour::

    cs.solve(schedule, constraints=[
        RoomConflictCost(),
        ContentConflictCost(),
        ContentConflictCost(weight=1, exponent=2, offset=60),
    ])

``cs.constraint_costs(schedule, picks)`` keys the second pick
``"content_conflict_cost#2"``.

Three terms (`audience_type_diversity_cost`, `audience_level_diversity_cost`,
`language_diversity_cost`) are **rewards**: their raw cost is <= 0, so raising their
weight raises the score. A reward is never hard: ``level="hard"`` raises ValueError (here,
in `score()` and in the C++ core). Everything else is a penalty that flips both ways.

Names (`name`) are the C++ component names in snake_case
(``RoomConflictCost`` -> ``room_conflict_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `cs.list_constraints()`.
Module-level instance names use the same snake_case, so they match the strings
`list_constraints()` prints. 0.2.0 renamed every constraint (``room_conflict`` ->
``room_conflict_cost``, ``same_day_talks`` -> ``different_day_talks_cost``, ...); the
old names are unknown now.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "TalkTypeIncompatibilityCost",
    "RoomUnavailabilityCost",
    "RoomConflictCost",
    "SpeakerUnavailabilityCost",
    "SpeakerConflictCost",
    "TalkPrerequisiteTalksCost",
    "TalkMutuallyExclusiveTalksTagsCost",
    "ConsecutiveTalksPauseCost",
    "CrowdControlRiskCost",
    "SpeakerRequiredTimeslotTagsCost",
    "SpeakerProhibitedTimeslotTagsCost",
    "TalkRequiredTimeslotTagsCost",
    "TalkProhibitedTimeslotTagsCost",
    "SpeakerRequiredRoomTagsCost",
    "SpeakerProhibitedRoomTagsCost",
    "TalkRequiredRoomTagsCost",
    "TalkProhibitedRoomTagsCost",
    "ThemeTrackConflictCost",
    "ThemeTrackRoomChangeCost",
    "SectorConflictCost",
    "AudienceTypeThemeTrackConflictCost",
    "ContentAudienceLevelFlowViolationCost",
    "ContentConflictCost",
    "DifferentDayTalksCost",
    "PopularTalksRoomSizeCost",
    "SpeakerPreferredTimeslotTagsCost",
    "SpeakerUndesiredTimeslotTagsCost",
    "TalkPreferredTimeslotTagsCost",
    "TalkUndesiredTimeslotTagsCost",
    "SpeakerPreferredRoomTagsCost",
    "SpeakerUndesiredRoomTagsCost",
    "TalkPreferredRoomTagsCost",
    "TalkUndesiredRoomTagsCost",
    "SpeakerMakespanCost",
    "AudienceTypeDiversityCost",
    "AudienceLevelDiversityCost",
    "LanguageDiversityCost",
    "talk_type_incompatibility_cost",
    "room_unavailability_cost",
    "room_conflict_cost",
    "speaker_unavailability_cost",
    "speaker_conflict_cost",
    "talk_prerequisite_talks_cost",
    "talk_mutually_exclusive_talks_tags_cost",
    "consecutive_talks_pause_cost",
    "crowd_control_risk_cost",
    "speaker_required_timeslot_tags_cost",
    "speaker_prohibited_timeslot_tags_cost",
    "talk_required_timeslot_tags_cost",
    "talk_prohibited_timeslot_tags_cost",
    "speaker_required_room_tags_cost",
    "speaker_prohibited_room_tags_cost",
    "talk_required_room_tags_cost",
    "talk_prohibited_room_tags_cost",
    "theme_track_conflict_cost",
    "theme_track_room_change_cost",
    "sector_conflict_cost",
    "audience_type_theme_track_conflict_cost",
    "content_audience_level_flow_violation_cost",
    "content_conflict_cost",
    "different_day_talks_cost",
    "popular_talks_room_size_cost",
    "speaker_preferred_timeslot_tags_cost",
    "speaker_undesired_timeslot_tags_cost",
    "talk_preferred_timeslot_tags_cost",
    "talk_undesired_timeslot_tags_cost",
    "speaker_preferred_room_tags_cost",
    "speaker_undesired_room_tags_cost",
    "talk_preferred_room_tags_cost",
    "talk_undesired_room_tags_cost",
    "speaker_makespan_cost",
    "audience_type_diversity_cost",
    "audience_level_diversity_cost",
    "language_diversity_cost",
    "get_constraints",
]


class TalkTypeIncompatibilityCost(Constraint):
    """Talks placed in a timeslot or room that does not accept their talk type. A synthetic
    constraint — the Java quickstart never builds such assignments, while here the
    same prohibition is expressed as a cost. Default: hard, weight 100'000."""

    name = "talk_type_incompatibility_cost"
    label_en = "Talk type incompatibility"
    label_ko = "발표 유형 불일치"
    description_en = (
        "For each talk whose timeslot does not accept its talk type, the timeslot's duration in minutes (at least 1), "
        "and the same again when its room does not. 0 means every talk sits in a timeslot and a room that accept its "
        "type."
    )
    description_ko = (
        "발표 유형을 허용하지 않는 시간대에 배정된 발표마다 그 시간대의 길이(분, 최소 1)를 더하고, 배정된 방이 그 "
        "유형을 허용하지 않을 때도 같은 값을 더합니다. 0이면 모든 발표가 자기 유형을 허용하는 시간대와 방에 배정되어 "
        "있습니다."
    )


class RoomUnavailabilityCost(Constraint):
    """Talks placed in a room that is unavailable in that timeslot. Default: hard, weight 100'000."""

    name = "room_unavailability_cost"
    label_en = "Room unavailability"
    label_ko = "방 사용 불가"
    description_en = (
        "The timeslot's duration in minutes for each talk placed in a room that is unavailable during that timeslot. 0 "
        "means no talk uses an unavailable room."
    )
    description_ko = (
        "그 시간대에 사용할 수 없는 방에 배정된 발표마다 시간대의 길이(분)를 더합니다. 0이면 사용할 수 없는 방에 "
        "배정된 발표가 없습니다."
    )


class RoomConflictCost(Constraint):
    """Two talks in overlapping timeslots sharing a room. Valued by overlapping minutes. Default: hard, weight 1'000."""

    name = "room_conflict_cost"
    label_en = "Room conflict"
    label_ko = "방 중복 사용"
    description_en = (
        "The overlapping minutes of every pair of talks that share a room in overlapping timeslots. 0 means no room "
        "hosts two talks at once."
    )
    description_ko = (
        "같은 방에서 시간이 겹치는 발표 쌍마다 겹치는 시간(분)을 더합니다. 0이면 한 방에서 두 발표가 동시에 열리지 "
        "않습니다."
    )


class SpeakerUnavailabilityCost(Constraint):
    """Talks placed in a timeslot where one of their speakers is unavailable. Valued per
    unavailable speaker. Default: hard, weight 100."""

    name = "speaker_unavailability_cost"
    label_en = "Speaker unavailability"
    label_ko = "발표자 참석 불가"
    description_en = (
        "The timeslot's duration in minutes for each speaker of a talk who is unavailable in that timeslot "
        "(speaker-minutes). 0 means every speaker is available for their talks."
    )
    description_ko = (
        "발표 시간대에 참석할 수 없는 발표자마다 그 시간대의 길이(분)를 더합니다(발표자 × 분). 0이면 모든 발표자가 "
        "자기 발표 시간에 참석할 수 있습니다."
    )


class SpeakerConflictCost(Constraint):
    """Two overlapping talks sharing a speaker. Valued by overlapping minutes per
    shared speaker — one person cannot be in two places at once. Default: hard, weight 10."""

    name = "speaker_conflict_cost"
    label_en = "Speaker conflict"
    label_ko = "발표자 일정 충돌"
    description_en = (
        "For every pair of overlapping talks, the speakers they share times the overlapping minutes. 0 means no "
        "speaker has to be in two rooms at once."
    )
    description_ko = (
        "시간이 겹치는 발표 쌍마다 공통 발표자 수에 겹치는 시간(분)을 곱해 더합니다. 0이면 동시에 두 방에 있어야 하는 "
        "발표자가 없습니다."
    )


class TalkPrerequisiteTalksCost(Constraint):
    """A talk scheduled before (or overlapping) a talk marked as its prerequisite. Valued
    by the sum of both talks' durations. Default: hard, weight 10."""

    name = "talk_prerequisite_talks_cost"
    label_en = "Prerequisite talks"
    label_ko = "선행 발표 순서"
    description_en = (
        "For each prerequisite that ends after the dependent talk starts, the combined duration of both talks in "
        "minutes. 0 means every prerequisite ends by the time the talk that requires it starts."
    )
    description_ko = (
        "선행 발표가 뒤따를 발표의 시작 시각보다 늦게 끝날 때마다 두 발표의 길이(분)를 합쳐 더합니다. 0이면 모든 "
        "선행 발표가 뒤따를 발표의 시작 시각까지 끝납니다."
    )


class TalkMutuallyExclusiveTalksTagsCost(Constraint):
    """Two overlapping talks with the same mutually exclusive tag. Valued by shared
    tags × overlap. Default: hard, weight 1."""

    name = "talk_mutually_exclusive_talks_tags_cost"
    label_en = "Mutually exclusive talks"
    label_ko = "동시 진행 금지 발표"
    description_en = (
        "For every pair of overlapping talks, the mutually-exclusive tags they share times the overlapping minutes. 0 "
        "means no two talks with such a common tag overlap."
    )
    description_ko = (
        "시간이 겹치는 발표 쌍마다 함께 가진 상호 배타 태그 수에 겹치는 시간(분)을 곱해 더합니다. 0이면 같은 상호 배타 "
        "태그를 가진 발표끼리 겹치지 않습니다."
    )


class ConsecutiveTalksPauseCost(Constraint):
    """Two talks of one speaker back to back without the minimum pause. Valued by
    the sum of both durations. Default: hard, weight 1."""

    name = "consecutive_talks_pause_cost"
    label_en = "Pause between a speaker's talks"
    label_ko = "발표자 발표 간 휴식"
    description_en = (
        "For every pair of talks sharing a speaker without the minimum pause between them (overlapping, or too close "
        "on the same day), the combined duration of both talks in minutes. 0 means every speaker gets the pause."
    )
    description_ko = (
        "공통 발표자가 있는 두 발표 사이에 최소 휴식 시간이 없을 때마다(시간이 겹치거나 같은 날 너무 가까울 때) 두 "
        "발표의 길이(분)를 합쳐 더합니다. 0이면 모든 발표자가 발표 사이에 최소 휴식 시간을 가집니다."
    )


class CrowdControlRiskCost(Constraint):
    """A crowd-risk talk in a timeslot that holds two or more other risky talks — the
    hallway overflows. Default: hard, weight 1."""

    name = "crowd_control_risk_cost"
    label_en = "Crowd control risk"
    label_ko = "군중 관리 위험"
    description_en = (
        "The timeslot's duration in minutes for each talk with a crowd-control risk in a timeslot that holds three or "
        "more such talks. 0 means no timeslot has more than two risky talks."
    )
    description_ko = (
        "군중 관리 위험이 있는 발표가 세 개 이상 몰린 시간대에서 그런 발표마다 시간대의 길이(분)를 더합니다. 0이면 "
        "어느 시간대에도 위험 발표가 두 개를 넘지 않습니다."
    )


class SpeakerRequiredTimeslotTagsCost(Constraint):
    """Tags a speaker requires that the assigned timeslot lacks. Valued by tags × duration. Default: hard, weight 1."""

    name = "speaker_required_timeslot_tags_cost"
    label_en = "Speaker required timeslot tags"
    label_ko = "발표자 필수 시간대 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's required timeslot tags the assigned timeslot "
        "lacks, times the timeslot's duration in minutes. 0 means every speaker's required timeslot tags are present."
    )
    description_ko = (
        "발표마다 각 발표자의 필수 시간대 태그 가운데 배정된 시간대에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 모든 발표자의 필수 시간대 태그가 갖춰져 있습니다."
    )


class SpeakerProhibitedTimeslotTagsCost(Constraint):
    """Tags a speaker prohibits that the assigned timeslot carries. Valued by tags × duration.
    Default: hard, weight 1."""

    name = "speaker_prohibited_timeslot_tags_cost"
    label_en = "Speaker prohibited timeslot tags"
    label_ko = "발표자 금지 시간대 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's prohibited timeslot tags the assigned timeslot "
        "carries, times the timeslot's duration in minutes. 0 means no speaker sits in a timeslot with a tag they "
        "prohibit."
    )
    description_ko = (
        "발표마다 각 발표자의 금지 시간대 태그 가운데 배정된 시간대에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 어느 발표자도 자신이 금지한 태그가 붙은 시간대에 발표하지 않습니다."
    )


class TalkRequiredTimeslotTagsCost(Constraint):
    """Tags a talk requires that the assigned timeslot lacks. Valued by tags × duration. Default: hard, weight 1."""

    name = "talk_required_timeslot_tags_cost"
    label_en = "Talk required timeslot tags"
    label_ko = "발표 필수 시간대 태그"
    description_en = (
        "For each talk, the number of its required timeslot tags the assigned timeslot lacks, times the timeslot's "
        "duration in minutes. 0 means every talk's required timeslot tags are present."
    )
    description_ko = (
        "발표마다 그 발표의 필수 시간대 태그 가운데 배정된 시간대에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 모든 발표의 필수 시간대 태그가 갖춰져 있습니다."
    )


class TalkProhibitedTimeslotTagsCost(Constraint):
    """Tags a talk prohibits that the assigned timeslot carries. Valued by tags × duration. Default: hard, weight 1."""

    name = "talk_prohibited_timeslot_tags_cost"
    label_en = "Talk prohibited timeslot tags"
    label_ko = "발표 금지 시간대 태그"
    description_en = (
        "For each talk, the number of its prohibited timeslot tags the assigned timeslot carries, times the timeslot's "
        "duration in minutes. 0 means no talk sits in a timeslot with a tag it prohibits."
    )
    description_ko = (
        "발표마다 그 발표의 금지 시간대 태그 가운데 배정된 시간대에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 금지 태그가 붙은 시간대에 배정된 발표가 없습니다."
    )


class SpeakerRequiredRoomTagsCost(Constraint):
    """Tags a speaker requires that the assigned room lacks. Valued by tags × duration. Default: hard, weight 1."""

    name = "speaker_required_room_tags_cost"
    label_en = "Speaker required room tags"
    label_ko = "발표자 필수 방 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's required room tags the assigned room lacks, times "
        "the timeslot's duration in minutes. 0 means every speaker's required room tags are present."
    )
    description_ko = (
        "발표마다 각 발표자의 필수 방 태그 가운데 배정된 방에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 "
        "모든 발표자의 필수 방 태그가 갖춰져 있습니다."
    )


class SpeakerProhibitedRoomTagsCost(Constraint):
    """Tags a speaker prohibits that the assigned room carries. Valued by tags × duration. Default: hard, weight 1."""

    name = "speaker_prohibited_room_tags_cost"
    label_en = "Speaker prohibited room tags"
    label_ko = "발표자 금지 방 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's prohibited room tags the assigned room carries, "
        "times the timeslot's duration in minutes. 0 means no speaker sits in a room with a tag they prohibit."
    )
    description_ko = (
        "발표마다 각 발표자의 금지 방 태그 가운데 배정된 방에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 "
        "어느 발표자도 자신이 금지한 태그가 붙은 방에서 발표하지 않습니다."
    )


class TalkRequiredRoomTagsCost(Constraint):
    """Tags a talk requires that the assigned room lacks. Valued by tags × duration. Default: hard, weight 1."""

    name = "talk_required_room_tags_cost"
    label_en = "Talk required room tags"
    label_ko = "발표 필수 방 태그"
    description_en = (
        "For each talk, the number of its required room tags the assigned room lacks, times the timeslot's duration in "
        "minutes. 0 means every talk's required room tags are present."
    )
    description_ko = (
        "발표마다 그 발표의 필수 방 태그 가운데 배정된 방에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 모든 "
        "발표의 필수 방 태그가 갖춰져 있습니다."
    )


class TalkProhibitedRoomTagsCost(Constraint):
    """Tags a talk prohibits that the assigned room carries. Valued by tags × duration. Default: hard, weight 1."""

    name = "talk_prohibited_room_tags_cost"
    label_en = "Talk prohibited room tags"
    label_ko = "발표 금지 방 태그"
    description_en = (
        "For each talk, the number of its prohibited room tags the assigned room carries, times the timeslot's "
        "duration in minutes. 0 means no talk sits in a room with a tag it prohibits."
    )
    description_ko = (
        "발표마다 그 발표의 금지 방 태그 가운데 배정된 방에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 금지 "
        "태그가 붙은 방에 배정된 발표가 없습니다."
    )


class ThemeTrackConflictCost(Constraint):
    """Two overlapping talks of the same theme track. Valued by shared themes × overlap —
    attendees following that track must give one up. Default: soft, weight 10."""

    name = "theme_track_conflict_cost"
    label_en = "Theme track conflict"
    label_ko = "테마 트랙 충돌"
    description_en = (
        "For every pair of overlapping talks, the theme tracks they share times the overlapping minutes. 0 means no "
        "two talks of one theme track overlap."
    )
    description_ko = (
        "시간이 겹치는 발표 쌍마다 공통 테마 트랙 수에 겹치는 시간(분)을 곱해 더합니다. 0이면 같은 테마 트랙의 "
        "발표끼리 겹치지 않습니다."
    )


class ThemeTrackRoomChangeCost(Constraint):
    """Two talks of one theme track on the same day in different rooms. The audience
    following that track has to pack up and move. Default: soft, weight 10."""

    name = "theme_track_room_change_cost"
    label_en = "Theme track room change"
    label_ko = "테마 트랙 방 이동"
    description_en = (
        "For every same-day pair of talks in different rooms, the theme tracks they share times their combined "
        "duration in minutes. 0 means each theme track stays in one room per day."
    )
    description_ko = (
        "같은 날 서로 다른 방에 있는 발표 쌍마다 공통 테마 트랙 수에 두 발표 길이(분)의 합을 곱해 더합니다. 0이면 각 "
        "테마 트랙이 하루 동안 한 방에 머뭅니다."
    )


class SectorConflictCost(Constraint):
    """Two overlapping talks targeting the same sector. Valued by shared sectors × overlap.
    Default: soft, weight 10."""

    name = "sector_conflict_cost"
    label_en = "Sector conflict"
    label_ko = "산업 분야 충돌"
    description_en = (
        "For every pair of overlapping talks, the sectors they share times the overlapping minutes. 0 means no two "
        "talks for the same sector overlap."
    )
    description_ko = (
        "시간이 겹치는 발표 쌍마다 공통 산업 분야 수에 겹치는 시간(분)을 곱해 더합니다. 0이면 같은 분야를 겨냥한 "
        "발표끼리 겹치지 않습니다."
    )


class AudienceTypeThemeTrackConflictCost(Constraint):
    """Two overlapping talks sharing **both** a theme track and an audience type. Default: soft, weight 1."""

    name = "audience_type_theme_track_conflict_cost"
    label_en = "Audience type and theme track conflict"
    label_ko = "청중 유형·테마 트랙 충돌"
    description_en = (
        "For every pair of overlapping talks, the shared theme tracks times the shared audience types times the "
        "overlapping minutes. 0 means no overlapping talks share both a theme track and an audience type."
    )
    description_ko = (
        "시간이 겹치는 발표 쌍마다 공통 테마 트랙 수 × 공통 청중 유형 수 × 겹치는 시간(분)을 더합니다. 0이면 테마 "
        "트랙과 청중 유형을 모두 공유하면서 겹치는 발표가 없습니다."
    )


class ContentAudienceLevelFlowViolationCost(Constraint):
    """An introductory talk scheduled after (or overlapping) an advanced talk of the same
    content — the audience cannot follow from easy to hard. Default: soft, weight 10."""

    name = "content_audience_level_flow_violation_cost"
    label_en = "Audience level flow violation"
    label_ko = "청중 수준 흐름 위반"
    description_en = (
        "For every pair of talks sharing content where the lower-level talk ends after the higher-level talk starts, "
        "the shared content tags times their combined duration in minutes. 0 means every content goes from the lower "
        "audience level to the higher one."
    )
    description_ko = (
        "같은 내용을 다루는 발표 쌍에서 낮은 수준의 발표가 높은 수준의 발표 시작 시각보다 늦게 끝날 때마다 공통 내용 "
        "태그 수에 두 발표 길이(분)의 합을 곱해 더합니다. 0이면 모든 내용이 낮은 수준에서 높은 수준 순으로 진행됩니다."
    )


class ContentConflictCost(Constraint):
    """Two overlapping talks of the same content. The most expensive soft rule — the same
    audience wants both. Default: soft, weight 100."""

    name = "content_conflict_cost"
    label_en = "Content conflict"
    label_ko = "내용 충돌"
    description_en = (
        "For every pair of overlapping talks, the content tags they share times the overlapping minutes. 0 means no "
        "two talks on the same content overlap."
    )
    description_ko = (
        "시간이 겹치는 발표 쌍마다 공통 내용 태그 수에 겹치는 시간(분)을 곱해 더합니다. 0이면 같은 내용의 발표끼리 "
        "겹치지 않습니다."
    )


class DifferentDayTalksCost(Constraint):
    """Two talks sharing content or a theme track placed on **different** days.
    Attendees have to come twice. Default: soft, weight 10."""

    name = "different_day_talks_cost"
    label_en = "Related talks on different days"
    label_ko = "다른 날에 열리는 관련 발표"
    description_en = (
        "For every pair of talks on different days, the shared content tags plus the shared theme tracks, times their "
        "combined duration in minutes. 0 means related talks are on the same day."
    )
    description_ko = (
        "서로 다른 날에 열리는 발표 쌍마다 (공통 내용 태그 수 + 공통 테마 트랙 수)에 두 발표 길이(분)의 합을 곱해 "
        "더합니다. 0이면 관련 발표가 모두 같은 날에 열립니다."
    )


class PopularTalksRoomSizeCost(Constraint):
    """A less popular talk getting a bigger room than a more popular one. Valued by the sum
    of both durations. Default: soft, weight 10."""

    name = "popular_talks_room_size_cost"
    label_en = "Popular talks in small rooms"
    label_ko = "작은 방에 배정된 인기 발표"
    description_en = (
        "For every pair of talks where the talk with fewer favorites has the larger room, their combined duration in "
        "minutes. 0 means room sizes follow popularity."
    )
    description_ko = (
        "즐겨찾기가 더 적은 발표가 더 큰 방을 쓰는 발표 쌍마다 두 발표 길이(분)의 합을 더합니다. 0이면 방 크기가 인기 "
        "순서를 따릅니다."
    )


class SpeakerPreferredTimeslotTagsCost(Constraint):
    """Tags a speaker prefers that the assigned timeslot lacks. Valued by tags × duration. Default: soft, weight 20."""

    name = "speaker_preferred_timeslot_tags_cost"
    label_en = "Speaker preferred timeslot tags"
    label_ko = "발표자 선호 시간대 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's preferred timeslot tags the assigned timeslot "
        "lacks, times the timeslot's duration in minutes. 0 means every speaker's preferred timeslot tags are present."
    )
    description_ko = (
        "발표마다 각 발표자의 선호 시간대 태그 가운데 배정된 시간대에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 모든 발표자의 선호 시간대 태그가 갖춰져 있습니다."
    )


class SpeakerUndesiredTimeslotTagsCost(Constraint):
    """Tags a speaker would rather avoid that the assigned timeslot carries. Valued by tags × duration.
    Default: soft, weight 20."""

    name = "speaker_undesired_timeslot_tags_cost"
    label_en = "Speaker undesired timeslot tags"
    label_ko = "발표자 기피 시간대 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's undesired timeslot tags the assigned timeslot "
        "carries, times the timeslot's duration in minutes. 0 means no speaker sits in a timeslot with a tag they "
        "would rather avoid."
    )
    description_ko = (
        "발표마다 각 발표자의 기피 시간대 태그 가운데 배정된 시간대에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 어느 발표자도 피하고 싶은 태그가 붙은 시간대에 발표하지 않습니다."
    )


class TalkPreferredTimeslotTagsCost(Constraint):
    """Tags a talk prefers that the assigned timeslot lacks. Valued by tags × duration. Default: soft, weight 20."""

    name = "talk_preferred_timeslot_tags_cost"
    label_en = "Talk preferred timeslot tags"
    label_ko = "발표 선호 시간대 태그"
    description_en = (
        "For each talk, the number of its preferred timeslot tags the assigned timeslot lacks, times the timeslot's "
        "duration in minutes. 0 means every talk's preferred timeslot tags are present."
    )
    description_ko = (
        "발표마다 그 발표의 선호 시간대 태그 가운데 배정된 시간대에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 모든 발표의 선호 시간대 태그가 갖춰져 있습니다."
    )


class TalkUndesiredTimeslotTagsCost(Constraint):
    """Tags a talk would rather avoid that the assigned timeslot carries. Valued by tags × duration.
    Default: soft, weight 20."""

    name = "talk_undesired_timeslot_tags_cost"
    label_en = "Talk undesired timeslot tags"
    label_ko = "발표 기피 시간대 태그"
    description_en = (
        "For each talk, the number of its undesired timeslot tags the assigned timeslot carries, times the timeslot's "
        "duration in minutes. 0 means no talk sits in a timeslot with a tag it would rather avoid."
    )
    description_ko = (
        "발표마다 그 발표의 기피 시간대 태그 가운데 배정된 시간대에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. "
        "0이면 피하고 싶은 태그가 붙은 시간대에 배정된 발표가 없습니다."
    )


class SpeakerPreferredRoomTagsCost(Constraint):
    """Tags a speaker prefers that the assigned room lacks. Valued by tags × duration. Default: soft, weight 20."""

    name = "speaker_preferred_room_tags_cost"
    label_en = "Speaker preferred room tags"
    label_ko = "발표자 선호 방 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's preferred room tags the assigned room lacks, times "
        "the timeslot's duration in minutes. 0 means every speaker's preferred room tags are present."
    )
    description_ko = (
        "발표마다 각 발표자의 선호 방 태그 가운데 배정된 방에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 "
        "모든 발표자의 선호 방 태그가 갖춰져 있습니다."
    )


class SpeakerUndesiredRoomTagsCost(Constraint):
    """Tags a speaker would rather avoid that the assigned room carries. Valued by tags × duration.
    Default: soft, weight 20."""

    name = "speaker_undesired_room_tags_cost"
    label_en = "Speaker undesired room tags"
    label_ko = "발표자 기피 방 태그"
    description_en = (
        "For each speaker of each talk, the number of the speaker's undesired room tags the assigned room carries, "
        "times the timeslot's duration in minutes. 0 means no speaker sits in a room with a tag they would rather "
        "avoid."
    )
    description_ko = (
        "발표마다 각 발표자의 기피 방 태그 가운데 배정된 방에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 "
        "어느 발표자도 피하고 싶은 태그가 붙은 방에서 발표하지 않습니다."
    )


class TalkPreferredRoomTagsCost(Constraint):
    """Tags a talk prefers that the assigned room lacks. Valued by tags × duration. Default: soft, weight 20."""

    name = "talk_preferred_room_tags_cost"
    label_en = "Talk preferred room tags"
    label_ko = "발표 선호 방 태그"
    description_en = (
        "For each talk, the number of its preferred room tags the assigned room lacks, times the timeslot's duration "
        "in minutes. 0 means every talk's preferred room tags are present."
    )
    description_ko = (
        "발표마다 그 발표의 선호 방 태그 가운데 배정된 방에 없는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 모든 "
        "발표의 선호 방 태그가 갖춰져 있습니다."
    )


class TalkUndesiredRoomTagsCost(Constraint):
    """Tags a talk would rather avoid that the assigned room carries. Valued by tags × duration.
    Default: soft, weight 20."""

    name = "talk_undesired_room_tags_cost"
    label_en = "Talk undesired room tags"
    label_ko = "발표 기피 방 태그"
    description_en = (
        "For each talk, the number of its undesired room tags the assigned room carries, times the timeslot's duration "
        "in minutes. 0 means no talk sits in a room with a tag it would rather avoid."
    )
    description_ko = (
        "발표마다 그 발표의 기피 방 태그 가운데 배정된 방에 있는 태그 수에 시간대 길이(분)를 곱해 더합니다. 0이면 "
        "피하고 싶은 태그가 붙은 방에 배정된 발표가 없습니다."
    )


class SpeakerMakespanCost(Constraint):
    """Speakers whose talks spread over more than two days. Valued by extra days × a working
    day (480 minutes) — keeps speakers from being held for the whole conference.
    Default: soft, weight 20."""

    name = "speaker_makespan_cost"
    label_en = "Speaker makespan"
    label_ko = "발표자 참석 기간"
    description_en = (
        "For each speaker whose talks span more than two consecutive days, (last day - first day - 1) times 480 "
        "minutes. 0 means every speaker's talks fit within two consecutive days."
    )
    description_ko = (
        "발표 일정이 연속된 이틀을 넘기는 발표자마다 (마지막 날 - 첫날 - 1) × 480분을 더합니다. 0이면 모든 발표자의 "
        "발표가 연속된 이틀 안에 모여 있습니다."
    )


class AudienceTypeDiversityCost(Constraint):
    """**Reward**: talks in one timeslot that share an audience type. Valued by shared
    audience types × slot duration (a negative cost, so it raises the score). Despite the
    name it rewards SHARED audience types; 0.2.0 keeps the 0.1.x arithmetic (rename only).
    Default: soft, weight 1; a reward, so never hard."""

    name = "audience_type_diversity_cost"
    label_en = "Audience type diversity (reward)"
    label_ko = "청중 유형 다양성(보상)"
    description_en = (
        "A reward: for every pair of talks in the same timeslot, minus the audience types they share times the slot's "
        "duration in minutes, so the raw cost is <= 0. Despite the name it rewards pairs that share an audience type "
        "(kept from 0.1.x). 0 means no reward is earned."
    )
    description_ko = (
        "보상 항목입니다. 같은 시간대의 발표 쌍마다 공통 청중 유형 수에 시간대 길이(분)를 곱한 값을 빼므로 원시 비용은 "
        "0 이하입니다. 이름과 달리 청중 유형을 공유하는 쌍에 보상을 줍니다(0.1.x 동작 유지). 0이면 받은 보상이 "
        "없습니다."
    )


class AudienceLevelDiversityCost(Constraint):
    """**Reward**: a timeslot mixing audience levels — beginners and experts both
    have somewhere to go. Default: soft, weight 1; a reward, so never hard."""

    name = "audience_level_diversity_cost"
    label_en = "Audience level diversity (reward)"
    label_ko = "청중 수준 다양성(보상)"
    description_en = (
        "A reward: for every pair of talks in the same timeslot with different audience levels, minus the slot's "
        "duration in minutes, so the raw cost is <= 0. 0 means no timeslot mixes audience levels."
    )
    description_ko = (
        "보상 항목입니다. 같은 시간대에서 청중 수준이 다른 발표 쌍마다 시간대 길이(분)를 빼므로 원시 비용은 0 "
        "이하입니다. 0이면 청중 수준을 섞은 시간대가 없습니다."
    )


class LanguageDiversityCost(Constraint):
    """**Reward**: a timeslot running talks in different languages together.
    Default: soft, weight 10; a reward, so never hard."""

    name = "language_diversity_cost"
    label_en = "Language diversity (reward)"
    label_ko = "언어 다양성(보상)"
    description_en = (
        "A reward: for every pair of talks in the same timeslot with different languages, minus the slot's duration in "
        "minutes, so the raw cost is <= 0. 0 means no timeslot mixes languages."
    )
    description_ko = (
        "보상 항목입니다. 같은 시간대에서 언어가 다른 발표 쌍마다 시간대 길이(분)를 빼므로 원시 비용은 0 이하입니다. "
        "0이면 여러 언어를 섞은 시간대가 없습니다."
    )


#: Ready-made default instances (``room_conflict_cost`` is ``RoomConflictCost()``).
talk_type_incompatibility_cost = TalkTypeIncompatibilityCost()
room_unavailability_cost = RoomUnavailabilityCost()
room_conflict_cost = RoomConflictCost()
speaker_unavailability_cost = SpeakerUnavailabilityCost()
speaker_conflict_cost = SpeakerConflictCost()
talk_prerequisite_talks_cost = TalkPrerequisiteTalksCost()
talk_mutually_exclusive_talks_tags_cost = TalkMutuallyExclusiveTalksTagsCost()
consecutive_talks_pause_cost = ConsecutiveTalksPauseCost()
crowd_control_risk_cost = CrowdControlRiskCost()
speaker_required_timeslot_tags_cost = SpeakerRequiredTimeslotTagsCost()
speaker_prohibited_timeslot_tags_cost = SpeakerProhibitedTimeslotTagsCost()
talk_required_timeslot_tags_cost = TalkRequiredTimeslotTagsCost()
talk_prohibited_timeslot_tags_cost = TalkProhibitedTimeslotTagsCost()
speaker_required_room_tags_cost = SpeakerRequiredRoomTagsCost()
speaker_prohibited_room_tags_cost = SpeakerProhibitedRoomTagsCost()
talk_required_room_tags_cost = TalkRequiredRoomTagsCost()
talk_prohibited_room_tags_cost = TalkProhibitedRoomTagsCost()
theme_track_conflict_cost = ThemeTrackConflictCost()
theme_track_room_change_cost = ThemeTrackRoomChangeCost()
sector_conflict_cost = SectorConflictCost()
audience_type_theme_track_conflict_cost = AudienceTypeThemeTrackConflictCost()
content_audience_level_flow_violation_cost = ContentAudienceLevelFlowViolationCost()
content_conflict_cost = ContentConflictCost()
different_day_talks_cost = DifferentDayTalksCost()
popular_talks_room_size_cost = PopularTalksRoomSizeCost()
speaker_preferred_timeslot_tags_cost = SpeakerPreferredTimeslotTagsCost()
speaker_undesired_timeslot_tags_cost = SpeakerUndesiredTimeslotTagsCost()
talk_preferred_timeslot_tags_cost = TalkPreferredTimeslotTagsCost()
talk_undesired_timeslot_tags_cost = TalkUndesiredTimeslotTagsCost()
speaker_preferred_room_tags_cost = SpeakerPreferredRoomTagsCost()
speaker_undesired_room_tags_cost = SpeakerUndesiredRoomTagsCost()
talk_preferred_room_tags_cost = TalkPreferredRoomTagsCost()
talk_undesired_room_tags_cost = TalkUndesiredRoomTagsCost()
speaker_makespan_cost = SpeakerMakespanCost()
audience_type_diversity_cost = AudienceTypeDiversityCost()
audience_level_diversity_cost = AudienceLevelDiversityCost()
language_diversity_cost = LanguageDiversityCost()

#: The default set, in list_constraints() order (every constraint is global).
_DEFAULTS = (
    talk_type_incompatibility_cost,
    room_unavailability_cost,
    room_conflict_cost,
    speaker_unavailability_cost,
    speaker_conflict_cost,
    talk_prerequisite_talks_cost,
    talk_mutually_exclusive_talks_tags_cost,
    consecutive_talks_pause_cost,
    crowd_control_risk_cost,
    speaker_required_timeslot_tags_cost,
    speaker_prohibited_timeslot_tags_cost,
    talk_required_timeslot_tags_cost,
    talk_prohibited_timeslot_tags_cost,
    speaker_required_room_tags_cost,
    speaker_prohibited_room_tags_cost,
    talk_required_room_tags_cost,
    talk_prohibited_room_tags_cost,
    theme_track_conflict_cost,
    theme_track_room_change_cost,
    sector_conflict_cost,
    audience_type_theme_track_conflict_cost,
    content_audience_level_flow_violation_cost,
    content_conflict_cost,
    different_day_talks_cost,
    popular_talks_room_size_cost,
    speaker_preferred_timeslot_tags_cost,
    speaker_undesired_timeslot_tags_cost,
    talk_preferred_timeslot_tags_cost,
    talk_undesired_timeslot_tags_cost,
    speaker_preferred_room_tags_cost,
    speaker_undesired_room_tags_cost,
    talk_preferred_room_tags_cost,
    talk_undesired_room_tags_cost,
    speaker_makespan_cost,
    audience_type_diversity_cost,
    audience_level_diversity_cost,
    language_diversity_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all thirty-seven default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored::

        get_constraints([RoomConflictCost(), ContentConflictCost(weight=500)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) raises ValueError; a constraint named several times is
    scored once per pick, each with its own knobs.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `cs.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost (one global total each; the three rewards are <= 0), before weight,
    exponent, offset and level rounding. Fetch it once and rescore with different
    weights without re-solving::

        raw = cs.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, [RoomConflictCost(), ContentConflictCost(weight=500)])
        score(raw, [ContentConflictCost(), ContentConflictCost(exponent=2, offset=60)])

    No solver and no search are involved. The arithmetic matches the engine —
    each total contributes ``weight * shape(cost, exponent, offset, clip)`` and hard
    contributions are rounded before summing (the same order as the engine). A
    second pick of a constraint reads the same raw cost with its own knobs.

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError, and so does
    ``level="hard"`` on a reward.
    """
    return score_costs(costs, get_constraints(constraints))
