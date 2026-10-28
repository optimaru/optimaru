"""제약 조립 스펙 — 비용 함수를 선언으로 고르는 API.

기본 경로(`cs.solve(schedule)`)는 서른일곱 제약을 provider 가중치로 전부
켠다. 이 모듈은 그 옆의 선택 경로다: 제약을 골라 담고, 가중치를 바꾸고,
하드/소프트를 뒤집은 목록을 `solve(schedule, constraints=...)` 에 넘긴다.

가장 짧은 사용법은 :func:`get_constraints` 다. 기본 목록을 그대로 돌려주니
"기본에서 조금 바꾸는" 선언이 한 줄로 끝난다::

    import conference_scheduling as cs
    from conference_scheduling.constraints import (
        ContentConflict, get_constraints, room_conflict,
    )

    cs.solve(schedule, constraints=get_constraints())           # 기본 그대로
    cs.solve(schedule, constraints=get_constraints([            # 골라 담기
        room_conflict, ContentConflict(weight=500),
    ]))

모듈 상위에 제약마다 기본 인스턴스가 있으니 그중 몇 개만 골라 리스트로
넘겨도 된다. 목록에 넣은 제약만 채점된다 — 빼면 그 항은 점수에서 사라진다.

모든 스펙은 ``(weight=None, hard=None, exponent=1.0)`` 를 받는다:

- ``weight``   제약 비용의 선형 배수. ``None`` 은 컴포넌트 기본값.
- ``hard``     ``True``/``False`` 로 기본 레벨을 뒤집는다; ``None`` 은 유지.
- ``exponent`` 부호 보존 거듭제곱 셰이핑: ``weight * sign(c) * |c| **
               exponent``. 2.0 = 제곱이라 몰린 위반이 기하급수로 비싸진다.

세 항(`audience_type_diversity`, `audience_level_diversity`,
`language_diversity`)은 **보상**이라 비용이 음수다 — 가중치를 올리면 점수가
올라간다. 나머지는 전부 벌점이다.

이름(`name`)은 C++ 컴포넌트 이름을 snake_case 로 옮긴 것이다. 코어가
기계적으로 유도하니 손으로 관리하는 별칭 표는 없다 —
`cs.list_constraints()` 로 확인할 수 있다. 모듈 상위 인스턴스 이름도 같은
snake_case 라 `list_constraints()` 가 찍는 문자열과 그대로 대응한다.
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
    """제약 하나를 고르는 스펙: 가중치 / 레벨 / 셰이핑."""

    weight: float | None = None  # None = 컴포넌트 기본 가중치
    hard: bool | None = None  # None = 컴포넌트 기본 레벨
    exponent: float = 1.0
    name: ClassVar[str] = ""

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if not cls.name:
            raise TypeError(f"{cls.__name__} must set a registry `name`")


class TalkTypeCompatibility(Constraint):
    """발표 유형을 받지 않는 타임슬롯/회의실에 놓인 발표. 합성 제약이다 —
    Java quickstart 는 그런 배정을 애초에 만들지 못하게 하지만, 여기서는
    같은 금지를 비용으로 표현한다. 기본: 하드, 가중치 100'000."""

    name = "talk_type_compatibility"


class RoomUnavailableTimeslot(Constraint):
    """그 타임슬롯에 닫혀 있는 회의실에 놓인 발표. 기본: 하드, 가중치 100'000."""

    name = "room_unavailable_timeslot"


class RoomConflict(Constraint):
    """타임슬롯이 겹치면서 회의실을 공유하는 두 발표. 겹친 분으로 값매김한다. 기본: 하드, 가중치 1'000."""

    name = "room_conflict"


class SpeakerUnavailableTimeslot(Constraint):
    """발표자 중 한 명이 불가한 타임슬롯에 놓인 발표. 기본: 하드, 가중치 100."""

    name = "speaker_unavailable_timeslot"


class SpeakerConflict(Constraint):
    """발표자를 공유하는 겹친 두 발표. 공유 발표자마다 겹친 분으로
    값매김한다 — 한 사람이 동시에 두 곳에 있을 수는 없다. 기본: 하드, 가중치 10."""

    name = "speaker_conflict"


class TalkPrerequisiteTalks(Constraint):
    """선행으로 지정한 발표보다 먼저(또는 겹쳐) 잡힌 발표. 두 발표의 길이
    합으로 값매김한다. 기본: 하드, 가중치 10."""

    name = "talk_prerequisite_talks"


class TalkMutuallyExclusiveTalksTags(Constraint):
    """같은 상호배타 태그를 단 겹친 두 발표. 공유 태그 수 × 겹침으로
    값매김한다. 기본: 하드, 가중치 1."""

    name = "talk_mutually_exclusive_talks_tags"


class ConsecutiveTalksPause(Constraint):
    """한 발표자의 두 발표가 최소 휴식 없이 연달아 잡힌 경우. 두 발표의
    길이 합으로 값매김한다. 기본: 하드, 가중치 1."""

    name = "consecutive_talks_pause"


class CrowdControl(Constraint):
    """혼잡 위험이 있는 발표가 같은 타임슬롯에서 다른 위험 발표 둘 이상과
    겹치는 경우 — 복도가 넘치는 모양이다. 기본: 하드, 가중치 1."""

    name = "crowd_control"


class SpeakerRequiredTimeslotTags(Constraint):
    """발표자가 요구하는데 배정 타임슬롯에 없는 태그. 빠진 태그 수 × 길이로
    값매김한다. 기본: 하드, 가중치 1."""

    name = "speaker_required_timeslot_tags"


class SpeakerProhibitedTimeslotTags(Constraint):
    """발표자가 금지하는데 배정 타임슬롯이 달고 있는 태그. 기본: 하드, 가중치 1."""

    name = "speaker_prohibited_timeslot_tags"


class TalkRequiredTimeslotTags(Constraint):
    """발표가 요구하는데 배정 타임슬롯에 없는 태그. 기본: 하드, 가중치 1."""

    name = "talk_required_timeslot_tags"


class TalkProhibitedTimeslotTags(Constraint):
    """발표가 금지하는데 배정 타임슬롯이 달고 있는 태그. 기본: 하드, 가중치 1."""

    name = "talk_prohibited_timeslot_tags"


class SpeakerRequiredRoomTags(Constraint):
    """발표자가 요구하는데 배정 회의실에 없는 태그. 기본: 하드, 가중치 1."""

    name = "speaker_required_room_tags"


class SpeakerProhibitedRoomTags(Constraint):
    """발표자가 금지하는데 배정 회의실이 달고 있는 태그. 기본: 하드, 가중치 1."""

    name = "speaker_prohibited_room_tags"


class TalkRequiredRoomTags(Constraint):
    """발표가 요구하는데 배정 회의실에 없는 태그. 기본: 하드, 가중치 1."""

    name = "talk_required_room_tags"


class TalkProhibitedRoomTags(Constraint):
    """발표가 금지하는데 배정 회의실이 달고 있는 태그. 기본: 하드, 가중치 1."""

    name = "talk_prohibited_room_tags"


class ThemeTrackConflict(Constraint):
    """같은 테마 트랙의 겹친 두 발표. 공유 테마 수 × 겹침으로 값매김한다 —
    그 트랙을 따라다니는 참석자가 둘 중 하나를 포기해야 한다. 기본: 소프트, 가중치 10."""

    name = "theme_track_conflict"


class ThemeTrackRoomStability(Constraint):
    """한 테마 트랙의 두 발표가 같은 날 다른 회의실에 놓인 경우. 그 트랙을
    따라다니는 청중이 짐을 싸서 옮겨야 한다. 기본: 소프트, 가중치 10."""

    name = "theme_track_room_stability"


class SectorConflict(Constraint):
    """같은 섹터를 겨냥한 겹친 두 발표. 기본: 소프트, 가중치 10."""

    name = "sector_conflict"


class AudienceTypeThemeTrackConflict(Constraint):
    """테마 트랙과 청중 유형을 **둘 다** 공유하는 겹친 두 발표. 기본: 소프트, 가중치 1."""

    name = "audience_type_theme_track_conflict"


class ContentAudienceLevelFlowViolation(Constraint):
    """같은 콘텐츠의 초급 발표가 고급 발표보다 뒤에(또는 겹쳐) 잡힌 경우 —
    청중이 쉬운 것부터 따라갈 수 없다. 기본: 소프트, 가중치 10."""

    name = "content_audience_level_flow_violation"


class ContentConflict(Constraint):
    """같은 콘텐츠의 겹친 두 발표. 소프트 규칙 중 가장 비싸다 — 같은 청중이
    둘 다 원한다. 기본: 소프트, 가중치 100."""

    name = "content_conflict"


class SameDayTalks(Constraint):
    """콘텐츠나 테마 트랙을 공유하는 두 발표가 **다른** 날에 놓인 경우.
    참석자가 이틀을 와야 한다. 기본: 소프트, 가중치 10."""

    name = "same_day_talks"


class PopularTalks(Constraint):
    """덜 인기 있는 발표가 더 인기 있는 발표보다 큰 회의실을 받은 경우. 기본: 소프트, 가중치 10."""

    name = "popular_talks"


class SpeakerPreferredTimeslotTags(Constraint):
    """발표자가 선호하는데 배정 타임슬롯에 없는 태그. 기본: 소프트, 가중치 20."""

    name = "speaker_preferred_timeslot_tags"


class SpeakerUndesiredTimeslotTags(Constraint):
    """발표자가 피하고 싶은데 배정 타임슬롯이 달고 있는 태그. 기본: 소프트, 가중치 20."""

    name = "speaker_undesired_timeslot_tags"


class TalkPreferredTimeslotTags(Constraint):
    """발표가 선호하는데 배정 타임슬롯에 없는 태그. 기본: 소프트, 가중치 20."""

    name = "talk_preferred_timeslot_tags"


class TalkUndesiredTimeslotTags(Constraint):
    """발표가 피하고 싶은데 배정 타임슬롯이 달고 있는 태그. 기본: 소프트, 가중치 20."""

    name = "talk_undesired_timeslot_tags"


class SpeakerPreferredRoomTags(Constraint):
    """발표자가 선호하는데 배정 회의실에 없는 태그. 기본: 소프트, 가중치 20."""

    name = "speaker_preferred_room_tags"


class SpeakerUndesiredRoomTags(Constraint):
    """발표자가 피하고 싶은데 배정 회의실이 달고 있는 태그. 기본: 소프트, 가중치 20."""

    name = "speaker_undesired_room_tags"


class TalkPreferredRoomTags(Constraint):
    """발표가 선호하는데 배정 회의실에 없는 태그. 기본: 소프트, 가중치 20."""

    name = "talk_preferred_room_tags"


class TalkUndesiredRoomTags(Constraint):
    """발표가 피하고 싶은데 배정 회의실이 달고 있는 태그. 기본: 소프트, 가중치 20."""

    name = "talk_undesired_room_tags"


class SpeakerMakespan(Constraint):
    """발표가 이틀을 넘겨 흩어진 발표자. 초과 일수 × 근무일로 값매김한다 —
    발표자가 컨퍼런스 내내 붙잡혀 있지 않게 하는 항이다. 기본: 소프트, 가중치 20."""

    name = "speaker_makespan"


class AudienceTypeDiversity(Constraint):
    """**보상**: 한 타임슬롯이 여러 청중 유형을 동시에 챙기는 경우.
    청중 유형 수 × 슬롯 길이로 값매김한다 (비용이 음수라 점수를 올린다). 기본: 소프트, 가중치 1."""

    name = "audience_type_diversity"


class AudienceLevelDiversity(Constraint):
    """**보상**: 한 타임슬롯이 청중 레벨을 섞는 경우 — 초심자와 전문가가
    둘 다 갈 곳이 있다. 기본: 소프트, 가중치 1."""

    name = "audience_level_diversity"


class LanguageDiversity(Constraint):
    """**보상**: 한 타임슬롯이 서로 다른 언어의 발표를 함께 여는 경우. 기본: 소프트, 가중치 10."""

    name = "language_diversity"


#: 기본 가중치로 만들어 둔 인스턴스. 골라 담을 때 그대로 참조하면 된다.
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

#: 기본 세트 — `cs.list_constraints()` 와 같은 순서 (unit 먼저, global 나중).
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
    """비용 함수를 돌려준다. 인자가 없으면 기본 서른일곱 제약 전부.

    `constraints` 를 주면 그 목록을 그대로 쓴다 — 목록에 있는 제약만
    채점된다. 기본에서 하나만 바꾸고 싶으면 기본 인스턴스를 섞어 쓴다::

        get_constraints([room_conflict, ContentConflict(weight=500)])
    """
    if constraints is None:
        return list(_DEFAULTS)
    if not constraints:
        # C++ applyConstraints 와 같은 계약: 빈 목록은 "아무것도 채점하지
        # 않음" 이라 실수일 가능성이 높다. 필터 결과가 우연히 비었을 때
        # 조용히 "전부 채점" 으로 뒤집히면 안 된다.
        raise ValueError("constraints must not be empty (pass None for the defaults)")
    return list(constraints)


class Score(NamedTuple):
    """`score()` 가 돌려주는 (hard, soft). 둘 다 벌점이라 0 이하다."""

    hard: int
    soft: float


def _shape(cost: float, exponent: float) -> float:
    """부호 보존 거듭제곱. 보상(음수 cost)이 벌점으로 뒤집히지 않는다."""
    if exponent == 1.0 or cost == 0.0:
        return cost
    return -((-cost) ** exponent) if cost < 0.0 else cost**exponent


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """제약 원본 cost 에 스펙을 적용해 점수를 낸다 — 순수 파이썬.

    ``costs`` 는 `cs.constraint_costs(schedule)` 가 돌려준 것이다: 제약마다
    unit 별 **원본** cost (가중치도 exponent 도 레벨 반올림도 적용되기 전).
    한 번 받아두면 가중치를 바꿔가며 재채점 없이 계산할 수 있다::

        raw = cs.constraint_costs(schedule)
        score(raw)                     # 기본 세트
        score(raw, picked)             # 골라 담은 목록

    솔버도 탐색 알고리즘도 거치지 않는다. 계산 규칙은 엔진과 같다 —
    unit 마다 ``weight * shape(cost, exponent)`` 를 구하고, 하드는 unit
    기여분을 **각각 반올림**해서 더한다 (엔진의 applyRaw 와 같은 순서;
    합계를 뒤에 반올림하면 값이 달라진다).

    ``constraints`` 를 주면 그 목록의 제약만 채점된다 — `get_constraints`
    와 같은 의미론이다. 모르는 이름은 ValueError.
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
                # 엔진과 같이 unit 기여분마다 반올림한다 (llround 와 같은
                # half-away-from-zero; 파이썬 round 는 banker's rounding).
                hard += int(math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5))
            else:
                soft += value
    return Score(-hard, -soft if soft != 0.0 else 0.0)
