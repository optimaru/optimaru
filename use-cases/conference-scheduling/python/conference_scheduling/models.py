"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Timeslot/Room/Speaker/Talk/ConferenceSchedule 과 SolverConfig 를
1:1 로 미러링한다. 엔진 형태 그대로를 저장/전송할 때(디버그 덤프, 회귀
스냅샷 등) 쓴다::

    from conference_scheduling.models import ConferenceScheduleModel

    model = ConferenceScheduleModel.from_core(schedule)  # 엔진 -> pydantic
    clone = ConferenceScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                          # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install conference-scheduling[models]`).
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
]


class TimeslotModel(BaseModel):
    """시간 칸. 분 단위(공통 기준점, 자정 경계는 1440 의 배수)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    start_minute: int = Field(ge=0)
    end_minute: int = Field(ge=0)
    talk_types: list[int]
    tags: list[str] = Field(default_factory=list)


class RoomModel(BaseModel):
    """발표장. unavailable_timeslots 는 timeslot 인덱스."""

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
    """planning entity. slot_room 은 timeslot*room 합성 변수(-1 = 미배정)."""

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
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

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
        """pydantic -> 엔진 객체 (score 필드는 엔진이 다시 계산하므로 무시)."""
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
        """엔진 객체 -> pydantic (현재 배정과 score 포함)."""
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
    """`_core.SolverConfig` 의 직렬화 가능한 미러."""

    model_config = ConfigDict(extra="forbid")

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    # 0 = 자동 (clamp(8 * talk 수, 800, 3200)). 양수면 그 값을 그대로 쓴다.
    late_acceptance_size: int = Field(default=0, ge=0)
    # 하나의 time limit 을 나눠 쓰는 연속 local search 실행 횟수
    # (각 구간은 직전 구간의 best 에서 재시작; cs/solver.hpp 참고).
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
    # 파괴-재구성을 acceptor 판정을 받는 평범한 무브로도 쓰는 확률.
    # 음수 = 자동 (0.2 / slotRoomCount, 예산 점유율 규칙 — benchmarks/school-timetabling/RESULTS.md 참고),
    # 0 = 끔.
    ruin_recreate_as_move_weight: float = Field(default=-1.0)
    ruin_recreate_as_move_max_ruin: int = Field(default=12, ge=1)
    # nearby 짝 하나에 대한 best-improvement 하강 확률 (기본 0 = 끔).
    descent_weight: float = Field(default=0.0, ge=0)
    # 하드 위반을 지닌 talk 을 겨냥해 무브 대상을 뽑는 확률 (0 = 끔).
    # 측정상 scaled:120 급에서만 이득이라 기본은 끔 — README 참고.
    hot_talk_probability: float = Field(default=0.0, ge=0, le=1)

    change_move_weight: float = Field(default=0.75, ge=0)
    swap_move_weight: float = Field(default=0.25, ge=0)
    # 타입 호환 (timeslot, room) 셀로 change move 를 몰아주는 확률
    # (Java 의 TalkType별 value range 대체; cs/solver.hpp 참고).
    compatible_value_probability: float = Field(default=0.9, ge=0, le=1)

    def to_core(self) -> "_core.SolverConfig":
        cfg = _core.SolverConfig()
        cfg.spent_limit_seconds = self.time_limit_seconds
        cfg.random_seed = self.random_seed
        cfg.parallel_solver_count = self.parallel_solver_count
        cfg.late_acceptance_size = self.late_acceptance_size  # 0 = 자동
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
