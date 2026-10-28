"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Timeslot/Room/Lesson/Timetable 과 SolverConfig 를 1:1 로 미러링한다.
canonical `TimetableProblem` JSON 은 `convert.py` 가 다루고, 여기 모델들은
엔진 형태 그대로를 저장/전송할 때(디버그 덤프, 회귀 스냅샷 등) 쓴다::

    from st_timetabling.models import TimetableModel, SolveConfig

    model = TimetableModel.from_core(table)     # 엔진 -> pydantic
    clone = TimetableModel.model_validate_json(model.model_dump_json())
    table2 = clone.to_core()                    # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install st-timetabling[models]`).
"""

from typing import Any

from pydantic import BaseModel, ConfigDict, Field, create_model, model_validator

from . import _core
from .constraints import soft_specs

__all__ = [
    "LessonModel",
    "ObjectiveWeightsModel",
    "RoomModel",
    "SolveConfig",
    "TimeslotModel",
    "TimetableModel",
]


class TimeslotModel(BaseModel):
    """하나의 수업 칸: (요일, 교시). day 0=월, period 는 1-base."""

    model_config = ConfigDict(extra="forbid")

    day: int = Field(ge=0)
    period: int = Field(ge=1)


class RoomModel(BaseModel):
    """교실. type 은 정보용(일반/과학실/...) — 엔진 hard 제약이 아니다."""

    model_config = ConfigDict(extra="forbid")

    name: str
    type: str = ""


class LessonModel(BaseModel):
    """planning entity. slot_room 은 timeslot*room 합성 변수(-1 = 미배정)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    subject: str
    teacher: str
    student_group: str
    slot_room: int = -1
    pinned: bool = False


class TimetableModel(BaseModel):
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

    model_config = ConfigDict(extra="forbid")

    timeslots: list[TimeslotModel]
    rooms: list[RoomModel]
    lessons: list[LessonModel]
    teacher_unavailable: dict[str, list[int]] = Field(default_factory=dict)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.Timetable":
        """pydantic -> 엔진 객체 (score 필드는 엔진이 다시 계산하므로 무시)."""
        return _core.Timetable(
            [_core.Timeslot(t.day, t.period) for t in self.timeslots],
            [_core.Room(r.name, r.type) for r in self.rooms],
            [self._lesson_to_core(l) for l in self.lessons],
            {name: set(slots) for name, slots in self.teacher_unavailable.items()},
        )

    @staticmethod
    def _lesson_to_core(l: LessonModel) -> "_core.Lesson":
        lesson = _core.Lesson(l.id, l.subject, l.teacher, l.student_group)
        lesson.slot_room = l.slot_room
        lesson.pinned = l.pinned
        return lesson

    @classmethod
    def from_core(cls, table: "_core.Timetable") -> "TimetableModel":
        """엔진 객체 -> pydantic (현재 배정과 score 포함)."""
        return cls(
            timeslots=[TimeslotModel(day=t.day, period=t.period) for t in table.timeslots],
            rooms=[RoomModel(name=r.name, type=r.type) for r in table.rooms],
            lessons=[
                LessonModel(
                    id=l.id,
                    subject=l.subject,
                    teacher=l.teacher,
                    student_group=l.student_group,
                    slot_room=l.slot_room,
                    pinned=l.pinned,
                )
                for l in table.lessons
            ],
            teacher_unavailable={name: sorted(slots) for name, slots in table.teacher_unavailable.items()},
            score_hard=table.score.hard,
            score_soft=table.score.soft,
        )


class _ObjectiveWeightsBase(BaseModel):
    """`ObjectiveWeightsModel` 의 몸통. 필드는 제약 레지스트리에서 생성된다."""

    model_config = ConfigDict(extra="forbid")

    def to_core(self) -> "_core.canon.ObjectiveWeights":
        # 모놀리식 fast path 의 ObjectiveWeights 는 고정 구조체라 그것이 아는
        # 항만 넘긴다. 조립 경로는 이 미러를 안 거치고 컴포넌트의 weight 를
        # 직접 쓰므로, 레지스트리에만 있는 새 소프트 항도 그쪽에서는 산다.
        known = set(_core.canon.ObjectiveWeights().__dir__())
        return _core.canon.ObjectiveWeights(**{k: v for k, v in self.model_dump().items() if k in known})


#: canonical soft 가중치 미러. **필드를 손으로 적지 않는다** — 제약 레지스트리의
#: 소프트 항에서 이름과 기본값을 그대로 가져온다 (플러그인 방식:
#: 목적함수는 등록된 제약들의 합이지 따로 관리하는 목록이 아니다). 소프트 제약을
#: 하나 추가하면 그 가중치 필드가 저절로 생기고, 두 목록이 갈라질 자리가 없다.
_objective_weight_fields: dict[str, Any | tuple[Any, Any]] = {
    spec.name: (
        int,
        Field(default=int(spec.default_weight), ge=0, description=(spec.__doc__ or "").strip().split("\n")[0]),
    )
    for spec in soft_specs()
}

_create_model: Any = create_model
ObjectiveWeightsModel = _create_model(
    "ObjectiveWeightsModel",
    __base__=_ObjectiveWeightsBase,
    __doc__=(
        "canonical soft 가중치 미러 (`_core.canon.ObjectiveWeights`). 필드는 "
        "`st_timetabling.constraints` 의 소프트 제약에서 생성된다."
    ),
    **_objective_weight_fields,
)


class SolveConfig(BaseModel):
    """`_core.SolverConfig` / `_core.canon.SolverConfig` 의 직렬화 가능한 미러.

    엔진 공통 필드는 두 모드가 같고, ``weights`` 는 canonical 모드
    (`to_canon()`)에만 반영된다 — quickstart config (`to_core()`)에는 해당
    필드가 없다."""

    model_config = ConfigDict(extra="forbid")

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    # 0 = 자동 (canonical: clamp(4 * 레슨 수, 400, 3200), quickstart:
    # clamp(8 * 레슨 수, 400, 3200)). 양수를 주면 그 값을 그대로 쓴다.
    late_acceptance_size: int = Field(default=0, ge=0)
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

    change_move_weight: float = Field(default=0.75, ge=0)
    swap_move_weight: float = Field(default=0.25, ge=0)

    weights: ObjectiveWeightsModel = Field(default_factory=ObjectiveWeightsModel)

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.ruin_recreate_min_ruin_fraction > self.ruin_recreate_max_ruin_fraction:
            raise ValueError("ruin_recreate_min_ruin_fraction must be <= ruin_recreate_max_ruin_fraction")
        return self

    def to_canon(self) -> "_core.canon.SolverConfig":
        """canonical 모드 config (weights 포함)."""
        cfg = self._apply_common(_core.canon.SolverConfig())
        cfg.weights = self.weights.to_core()
        return cfg

    def to_core(self) -> "_core.SolverConfig":
        """quickstart 모드 config (weights 는 quickstart 도메인에 없어 무시)."""
        return self._apply_common(_core.SolverConfig())

    def _apply_common(self, cfg):
        cfg.spent_limit_seconds = self.time_limit_seconds
        cfg.random_seed = self.random_seed
        cfg.parallel_solver_count = self.parallel_solver_count
        cfg.late_acceptance_size = self.late_acceptance_size  # 0 = 자동
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
        cfg.change_move_weight = self.change_move_weight
        cfg.swap_move_weight = self.swap_move_weight
        if self.stop_at_first_feasible:
            cfg.best_score_limit = (0, -1e18)
        if self.best_score_limit is not None:
            cfg.best_score_limit = self.best_score_limit
        return cfg
