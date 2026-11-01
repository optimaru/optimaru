"""Class-based CP-SAT plugins for conference scheduling constraints.

One plugin per engine constraint, subclassing its spec
(``conference_scheduling.constraints``) and carrying only ``build()``: the
defaults (level, weight, reward, flippability) come from the engine's
``conference_scheduling.constraint_info()`` through cpsat-common. The registry
keeps the engine's ``list_constraints()`` order. The three ``*_diversity_cost``
rewards add a reward term and refuse ``hard=True``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, ClassVar

from conference_scheduling import constraints as domain
from conference_scheduling.models import ConferenceScheduleModel
from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model


@dataclass
class ConferenceContext:
    model: cp_model.CpModel
    problem: ConferenceScheduleModel
    x: dict[tuple[int, int, int], Any]
    by_slot: dict[tuple[int, int], Any]
    by_room: dict[tuple[int, int], Any]
    _and_cache: dict[tuple[Any, ...], Any] = field(default_factory=dict)

    def both(self, key: tuple[Any, ...], left: Any, right: Any) -> Any:
        cached = self._and_cache.get(key)
        if cached is not None:
            return cached
        literal = self.model.new_bool_var("_".join(map(str, key)))
        self.model.add(literal <= left)
        self.model.add(literal <= right)
        self.model.add(literal >= left + right - 1)
        self._and_cache[key] = literal
        return literal


def _overlap(left: Any, right: Any) -> int:
    return max(
        0,
        min(left.end_minute, right.end_minute)
        - max(left.start_minute, right.start_minute),
    )


def _missing(required: list[str], actual: list[str]) -> int:
    return len(set(required) - set(actual))


def _prohibited(prohibited: list[str], actual: list[str]) -> int:
    return len(set(prohibited) & set(actual))


class TalkTypeIncompatibilityCost(
    domain.TalkTypeIncompatibilityCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for talk, talk_data in enumerate(context.problem.talks):
            for slot, slot_data in enumerate(context.problem.timeslots):
                for room, room_data in enumerate(context.problem.rooms):
                    duration = slot_data.end_minute - slot_data.start_minute
                    incompatible = duration * (
                        int(talk_data.talk_type not in slot_data.talk_types)
                        + int(talk_data.talk_type not in room_data.talk_types)
                    )
                    objective.add(
                        self.name,
                        incompatible * context.x[talk, slot, room],
                        bound=incompatible,
                    )


class RoomUnavailabilityCost(
    domain.RoomUnavailabilityCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for talk in range(len(context.problem.talks)):
            for slot in range(len(context.problem.timeslots)):
                for room, room_data in enumerate(context.problem.rooms):
                    unavailable = int(slot in room_data.unavailable_timeslots)
                    objective.add(
                        self.name,
                        unavailable * context.x[talk, slot, room],
                        bound=unavailable,
                    )


class SpeakerUnavailabilityCost(
    domain.SpeakerUnavailabilityCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for talk, talk_data in enumerate(context.problem.talks):
            speakers = [context.problem.speakers[index] for index in talk_data.speakers]
            for slot, slot_data in enumerate(context.problem.timeslots):
                unavailable = (slot_data.end_minute - slot_data.start_minute) * sum(
                    slot in speaker.unavailable_timeslots for speaker in speakers
                )
                objective.add(
                    self.name,
                    unavailable * context.by_slot[talk, slot],
                    bound=unavailable,
                )


class _TimeslotTagPlugin(ConstraintPlugin[ConferenceContext]):
    owner_kind: ClassVar[str]
    attribute: ClassVar[str]
    comparison: ClassVar[str]

    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        compare = _missing if self.comparison == "missing" else _prohibited
        for talk, talk_data in enumerate(context.problem.talks):
            owners = (
                [talk_data]
                if self.owner_kind == "talk"
                else [context.problem.speakers[index] for index in talk_data.speakers]
            )
            for slot, slot_data in enumerate(context.problem.timeslots):
                duration = slot_data.end_minute - slot_data.start_minute
                for owner in owners:
                    amount = duration * compare(
                        getattr(owner, self.attribute), slot_data.tags
                    )
                    objective.add(
                        self.name,
                        amount * context.by_slot[talk, slot],
                        bound=amount,
                    )


class _RoomTagPlugin(ConstraintPlugin[ConferenceContext]):
    owner_kind: ClassVar[str]
    attribute: ClassVar[str]
    comparison: ClassVar[str]

    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        compare = _missing if self.comparison == "missing" else _prohibited
        for talk, talk_data in enumerate(context.problem.talks):
            owners = (
                [talk_data]
                if self.owner_kind == "talk"
                else [context.problem.speakers[index] for index in talk_data.speakers]
            )
            for room, room_data in enumerate(context.problem.rooms):
                for owner in owners:
                    tags = compare(getattr(owner, self.attribute), room_data.tags)
                    for slot, slot_data in enumerate(context.problem.timeslots):
                        duration = slot_data.end_minute - slot_data.start_minute
                        amount = tags * duration
                        objective.add(
                            self.name,
                            amount * context.x[talk, slot, room],
                            bound=amount,
                        )


class SpeakerRequiredTimeslotTagsCost(
    domain.SpeakerRequiredTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "speaker"
    attribute = "required_timeslot_tags"
    comparison = "missing"


class SpeakerProhibitedTimeslotTagsCost(
    domain.SpeakerProhibitedTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "speaker"
    attribute = "prohibited_timeslot_tags"
    comparison = "prohibited"


class TalkRequiredTimeslotTagsCost(
    domain.TalkRequiredTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "talk"
    attribute = "required_timeslot_tags"
    comparison = "missing"


class TalkProhibitedTimeslotTagsCost(
    domain.TalkProhibitedTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "talk"
    attribute = "prohibited_timeslot_tags"
    comparison = "prohibited"


class SpeakerPreferredTimeslotTagsCost(
    domain.SpeakerPreferredTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "speaker"
    attribute = "preferred_timeslot_tags"
    comparison = "missing"


class SpeakerUndesiredTimeslotTagsCost(
    domain.SpeakerUndesiredTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "speaker"
    attribute = "undesired_timeslot_tags"
    comparison = "prohibited"


class TalkPreferredTimeslotTagsCost(
    domain.TalkPreferredTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "talk"
    attribute = "preferred_timeslot_tags"
    comparison = "missing"


class TalkUndesiredTimeslotTagsCost(
    domain.TalkUndesiredTimeslotTagsCost, _TimeslotTagPlugin
):
    owner_kind = "talk"
    attribute = "undesired_timeslot_tags"
    comparison = "prohibited"


class SpeakerRequiredRoomTagsCost(domain.SpeakerRequiredRoomTagsCost, _RoomTagPlugin):
    owner_kind = "speaker"
    attribute = "required_room_tags"
    comparison = "missing"


class SpeakerProhibitedRoomTagsCost(
    domain.SpeakerProhibitedRoomTagsCost, _RoomTagPlugin
):
    owner_kind = "speaker"
    attribute = "prohibited_room_tags"
    comparison = "prohibited"


class TalkRequiredRoomTagsCost(domain.TalkRequiredRoomTagsCost, _RoomTagPlugin):
    owner_kind = "talk"
    attribute = "required_room_tags"
    comparison = "missing"


class TalkProhibitedRoomTagsCost(domain.TalkProhibitedRoomTagsCost, _RoomTagPlugin):
    owner_kind = "talk"
    attribute = "prohibited_room_tags"
    comparison = "prohibited"


class SpeakerPreferredRoomTagsCost(domain.SpeakerPreferredRoomTagsCost, _RoomTagPlugin):
    owner_kind = "speaker"
    attribute = "preferred_room_tags"
    comparison = "missing"


class SpeakerUndesiredRoomTagsCost(domain.SpeakerUndesiredRoomTagsCost, _RoomTagPlugin):
    owner_kind = "speaker"
    attribute = "undesired_room_tags"
    comparison = "prohibited"


class TalkPreferredRoomTagsCost(domain.TalkPreferredRoomTagsCost, _RoomTagPlugin):
    owner_kind = "talk"
    attribute = "preferred_room_tags"
    comparison = "missing"


class TalkUndesiredRoomTagsCost(domain.TalkUndesiredRoomTagsCost, _RoomTagPlugin):
    owner_kind = "talk"
    attribute = "undesired_room_tags"
    comparison = "prohibited"


class TalkPrerequisiteTalksCost(
    domain.TalkPrerequisiteTalksCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for talk, talk_data in enumerate(context.problem.talks):
            for prerequisite in talk_data.prerequisite_talks:
                for slot, slot_data in enumerate(context.problem.timeslots):
                    for earlier_slot, earlier_data in enumerate(
                        context.problem.timeslots
                    ):
                        if slot_data.start_minute >= earlier_data.end_minute:
                            continue
                        both = context.both(
                            ("prerequisite", talk, prerequisite, slot, earlier_slot),
                            context.by_slot[talk, slot],
                            context.by_slot[prerequisite, earlier_slot],
                        )
                        amount = (
                            slot_data.end_minute
                            - slot_data.start_minute
                            + earlier_data.end_minute
                            - earlier_data.start_minute
                        )
                        objective.add(self.name, amount * both, bound=amount)


class _OverlapSharedPlugin(ConstraintPlugin[ConferenceContext]):
    attribute: ClassVar[str]

    def coefficient(self, left: Any, right: Any) -> int:
        return len(
            set(getattr(left, self.attribute)) & set(getattr(right, self.attribute))
        )

    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.talks)):
            a = context.problem.talks[left]
            for right in range(left + 1, len(context.problem.talks)):
                b = context.problem.talks[right]
                shared = self.coefficient(a, b)
                if not shared:
                    continue
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        overlap = _overlap(left_data, right_data)
                        if not overlap:
                            continue
                        both = context.both(
                            ("pair", left, right, left_slot, right_slot),
                            context.by_slot[left, left_slot],
                            context.by_slot[right, right_slot],
                        )
                        objective.add(
                            self.name, shared * overlap * both, bound=shared * overlap
                        )


class SpeakerConflictCost(domain.SpeakerConflictCost, _OverlapSharedPlugin):
    attribute = "speakers"


class TalkMutuallyExclusiveTalksTagsCost(
    domain.TalkMutuallyExclusiveTalksTagsCost, _OverlapSharedPlugin
):
    attribute = "mutually_exclusive_talks_tags"


class ThemeTrackConflictCost(domain.ThemeTrackConflictCost, _OverlapSharedPlugin):
    attribute = "theme_track_tags"


class SectorConflictCost(domain.SectorConflictCost, _OverlapSharedPlugin):
    attribute = "sector_tags"


class ContentConflictCost(domain.ContentConflictCost, _OverlapSharedPlugin):
    attribute = "content_tags"


class AudienceTypeThemeTrackConflictCost(
    domain.AudienceTypeThemeTrackConflictCost, _OverlapSharedPlugin
):
    attribute = "audience_types"

    def coefficient(self, left: Any, right: Any) -> int:
        audience = super().coefficient(left, right)
        theme = len(set(left.theme_track_tags) & set(right.theme_track_tags))
        return audience * theme


class ConsecutiveTalksPauseCost(
    domain.ConsecutiveTalksPauseCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.talks)):
            a = context.problem.talks[left]
            for right in range(left + 1, len(context.problem.talks)):
                b = context.problem.talks[right]
                if not set(a.speakers) & set(b.speakers):
                    continue
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        gap = max(
                            left_data.start_minute, right_data.start_minute
                        ) - min(left_data.end_minute, right_data.end_minute)
                        if (
                            gap
                            >= context.problem.minimum_consecutive_talks_pause_in_minutes
                        ):
                            continue
                        both = context.both(
                            ("pair", left, right, left_slot, right_slot),
                            context.by_slot[left, left_slot],
                            context.by_slot[right, right_slot],
                        )
                        combined = (
                            left_data.end_minute
                            - left_data.start_minute
                            + right_data.end_minute
                            - right_data.start_minute
                        )
                        objective.add(self.name, combined * both, bound=combined)


class CrowdControlRiskCost(
    domain.CrowdControlRiskCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        risky = [
            talk
            for talk, data in enumerate(context.problem.talks)
            if data.crowd_control_risk > 0
        ]
        for slot, slot_data in enumerate(context.problem.timeslots):
            count = sum(context.by_slot[talk, slot] for talk in risky)
            crowded = context.model.new_bool_var(f"crowd_control_slot_{slot}")
            context.model.add(count >= 3).only_enforce_if(crowded)
            context.model.add(count <= 2).only_enforce_if(crowded.negated())
            duration = slot_data.end_minute - slot_data.start_minute
            for talk in risky:
                contributes = context.both(
                    ("crowd_control_risk", talk, slot),
                    context.by_slot[talk, slot],
                    crowded,
                )
                objective.add(self.name, duration * contributes, bound=duration)


class RoomConflictCost(domain.RoomConflictCost, ConstraintPlugin[ConferenceContext]):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.talks)):
            for right in range(left + 1, len(context.problem.talks)):
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        overlap = _overlap(left_data, right_data)
                        if not overlap:
                            continue
                        for room in range(len(context.problem.rooms)):
                            same_room = context.both(
                                ("room_pair", left, right, left_slot, right_slot, room),
                                context.x[left, left_slot, room],
                                context.x[right, right_slot, room],
                            )
                            objective.add(self.name, overlap * same_room, bound=overlap)


class ContentAudienceLevelFlowViolationCost(
    domain.ContentAudienceLevelFlowViolationCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.talks)):
            a = context.problem.talks[left]
            for right in range(left + 1, len(context.problem.talks)):
                b = context.problem.talks[right]
                shared = len(set(a.content_tags) & set(b.content_tags))
                if not shared or a.audience_level == b.audience_level:
                    continue
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        beginner_is_left = a.audience_level < b.audience_level
                        beginner_slot = left_data if beginner_is_left else right_data
                        advanced_slot = right_data if beginner_is_left else left_data
                        if beginner_slot.end_minute <= advanced_slot.start_minute:
                            continue
                        both = context.both(
                            ("pair", left, right, left_slot, right_slot),
                            context.by_slot[left, left_slot],
                            context.by_slot[right, right_slot],
                        )
                        combined = (
                            left_data.end_minute
                            - left_data.start_minute
                            + right_data.end_minute
                            - right_data.start_minute
                        )
                        objective.add(
                            self.name, shared * combined * both, bound=shared * combined
                        )


class DifferentDayTalksCost(
    domain.DifferentDayTalksCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.talks)):
            a = context.problem.talks[left]
            for right in range(left + 1, len(context.problem.talks)):
                b = context.problem.talks[right]
                shared = len(set(a.content_tags) & set(b.content_tags)) + len(
                    set(a.theme_track_tags) & set(b.theme_track_tags)
                )
                if not shared:
                    continue
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        if (
                            left_data.start_minute // 1440
                            == right_data.start_minute // 1440
                        ):
                            continue
                        both = context.both(
                            ("pair", left, right, left_slot, right_slot),
                            context.by_slot[left, left_slot],
                            context.by_slot[right, right_slot],
                        )
                        combined = (
                            left_data.end_minute
                            - left_data.start_minute
                            + right_data.end_minute
                            - right_data.start_minute
                        )
                        objective.add(
                            self.name, shared * combined * both, bound=shared * combined
                        )


class ThemeTrackRoomChangeCost(
    domain.ThemeTrackRoomChangeCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        room_count = len(context.problem.rooms)
        for left in range(len(context.problem.talks)):
            a = context.problem.talks[left]
            for right in range(left + 1, len(context.problem.talks)):
                b = context.problem.talks[right]
                shared = len(set(a.theme_track_tags) & set(b.theme_track_tags))
                if not shared:
                    continue
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        if (
                            left_data.start_minute // 1440
                            != right_data.start_minute // 1440
                        ):
                            continue
                        for left_room in range(room_count):
                            for right_room in range(room_count):
                                if left_room == right_room:
                                    continue
                                rooms_both = context.both(
                                    (
                                        "room_change",
                                        left,
                                        right,
                                        left_slot,
                                        right_slot,
                                        left_room,
                                        right_room,
                                    ),
                                    context.x[left, left_slot, left_room],
                                    context.x[right, right_slot, right_room],
                                )
                                combined = (
                                    left_data.end_minute
                                    - left_data.start_minute
                                    + right_data.end_minute
                                    - right_data.start_minute
                                )
                                objective.add(
                                    self.name,
                                    shared * combined * rooms_both,
                                    bound=shared * combined,
                                )


class PopularTalksRoomSizeCost(
    domain.PopularTalksRoomSizeCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.problem.talks)):
            a = context.problem.talks[left]
            for right in range(left + 1, len(context.problem.talks)):
                b = context.problem.talks[right]
                if a.favorite_count == b.favorite_count:
                    continue
                for left_slot, left_data in enumerate(context.problem.timeslots):
                    for right_slot, right_data in enumerate(context.problem.timeslots):
                        combined = (
                            left_data.end_minute
                            - left_data.start_minute
                            + right_data.end_minute
                            - right_data.start_minute
                        )
                        for left_room, left_room_data in enumerate(
                            context.problem.rooms
                        ):
                            for right_room, right_room_data in enumerate(
                                context.problem.rooms
                            ):
                                wrong = (
                                    a.favorite_count > b.favorite_count
                                    and left_room_data.capacity
                                    < right_room_data.capacity
                                ) or (
                                    b.favorite_count > a.favorite_count
                                    and right_room_data.capacity
                                    < left_room_data.capacity
                                )
                                if not wrong:
                                    continue
                                rooms_both = context.both(
                                    (
                                        "popular",
                                        left,
                                        right,
                                        left_slot,
                                        right_slot,
                                        left_room,
                                        right_room,
                                    ),
                                    context.x[left, left_slot, left_room],
                                    context.x[right, right_slot, right_room],
                                )
                                objective.add(
                                    self.name, combined * rooms_both, bound=combined
                                )


class _DiversityPlugin(ConstraintPlugin[ConferenceContext]):
    attribute: ClassVar[str]

    def values(self, talk: Any) -> list[Any]:
        value = getattr(talk, self.attribute)
        return value if isinstance(value, list) else [value]

    def pair_amount(self, left: set[Any], right: set[Any]) -> int:
        return len(left & right)

    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        for slot, slot_data in enumerate(context.problem.timeslots):
            duration = slot_data.end_minute - slot_data.start_minute
            for left in range(len(context.problem.talks)):
                left_values = set(self.values(context.problem.talks[left]))
                for right in range(left + 1, len(context.problem.talks)):
                    right_values = set(self.values(context.problem.talks[right]))
                    amount = self.pair_amount(left_values, right_values) * duration
                    if not amount:
                        continue
                    both = context.both(
                        ("diversity", self.name, slot, left, right),
                        context.by_slot[left, slot],
                        context.by_slot[right, slot],
                    )
                    objective.add_reward(self.name, amount * both, bound=amount)


class AudienceTypeDiversityCost(domain.AudienceTypeDiversityCost, _DiversityPlugin):
    attribute = "audience_types"


class AudienceLevelDiversityCost(domain.AudienceLevelDiversityCost, _DiversityPlugin):
    attribute = "audience_level"

    def pair_amount(self, left: set[Any], right: set[Any]) -> int:
        return int(left != right)


class LanguageDiversityCost(domain.LanguageDiversityCost, _DiversityPlugin):
    attribute = "language"

    def pair_amount(self, left: set[Any], right: set[Any]) -> int:
        return int(left != right)


class SpeakerMakespanCost(
    domain.SpeakerMakespanCost, ConstraintPlugin[ConferenceContext]
):
    def build(self, context: ConferenceContext, objective: ObjectiveBuilder) -> None:
        days = sorted({slot.start_minute // 1440 for slot in context.problem.timeslots})
        if not days:
            return
        first_day, last_day = days[0], days[-1]
        for speaker in range(len(context.problem.speakers)):
            active_days = []
            for day in days:
                members = [
                    context.by_slot[talk, slot]
                    for talk, talk_data in enumerate(context.problem.talks)
                    if speaker in talk_data.speakers
                    for slot, slot_data in enumerate(context.problem.timeslots)
                    if slot_data.start_minute // 1440 == day
                ]
                if members:
                    active = context.model.new_bool_var(f"speaker_{speaker}_day_{day}")
                    context.model.add_max_equality(active, members)
                    active_days.append(active)
                else:
                    active_days.append(context.model.new_constant(0))
            if active_days:
                min_candidates = []
                max_candidates = []
                for day, active in zip(days, active_days, strict=True):
                    min_candidate = context.model.new_int_var(
                        first_day,
                        last_day + 1,
                        f"speaker_{speaker}_min_candidate_{day}",
                    )
                    context.model.add(
                        min_candidate == day + (last_day + 1 - day) * (1 - active)
                    )
                    min_candidates.append(min_candidate)
                    max_candidates.append(day * active)
                min_day = context.model.new_int_var(
                    first_day, last_day + 1, f"speaker_{speaker}_min_day"
                )
                max_day = context.model.new_int_var(
                    0, last_day, f"speaker_{speaker}_max_day"
                )
                context.model.add_min_equality(min_day, min_candidates)
                context.model.add_max_equality(max_day, max_candidates)
                excess = context.model.new_int_var(
                    0,
                    max(0, last_day - first_day),
                    f"speaker_{speaker}_day_excess",
                )
                context.model.add_max_equality(excess, [0, max_day - min_day - 1])
                objective.add(
                    self.name,
                    8 * 60 * excess,
                    bound=8 * 60 * max(0, last_day - first_day),
                )


REGISTRY = ConstraintRegistry(
    [
        TalkTypeIncompatibilityCost(),
        RoomUnavailabilityCost(),
        RoomConflictCost(),
        SpeakerUnavailabilityCost(),
        SpeakerConflictCost(),
        TalkPrerequisiteTalksCost(),
        TalkMutuallyExclusiveTalksTagsCost(),
        ConsecutiveTalksPauseCost(),
        CrowdControlRiskCost(),
        SpeakerRequiredTimeslotTagsCost(),
        SpeakerProhibitedTimeslotTagsCost(),
        TalkRequiredTimeslotTagsCost(),
        TalkProhibitedTimeslotTagsCost(),
        SpeakerRequiredRoomTagsCost(),
        SpeakerProhibitedRoomTagsCost(),
        TalkRequiredRoomTagsCost(),
        TalkProhibitedRoomTagsCost(),
        ThemeTrackConflictCost(),
        ThemeTrackRoomChangeCost(),
        SectorConflictCost(),
        AudienceTypeThemeTrackConflictCost(),
        ContentAudienceLevelFlowViolationCost(),
        ContentConflictCost(),
        DifferentDayTalksCost(),
        PopularTalksRoomSizeCost(),
        SpeakerPreferredTimeslotTagsCost(),
        SpeakerUndesiredTimeslotTagsCost(),
        TalkPreferredTimeslotTagsCost(),
        TalkUndesiredTimeslotTagsCost(),
        SpeakerPreferredRoomTagsCost(),
        SpeakerUndesiredRoomTagsCost(),
        TalkPreferredRoomTagsCost(),
        TalkUndesiredRoomTagsCost(),
        SpeakerMakespanCost(),
        AudienceTypeDiversityCost(),
        AudienceLevelDiversityCost(),
        LanguageDiversityCost(),
    ]
)
