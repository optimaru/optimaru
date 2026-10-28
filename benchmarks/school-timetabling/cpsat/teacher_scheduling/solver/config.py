"""솔버 설정과 목적함수 가중치."""

from pydantic import BaseModel, ConfigDict, Field, model_validator

from teacher_scheduling.config import env


class ObjectiveWeights(BaseModel):
    """소프트 제약 가중치. 0 으로 두면 해당 항목을 목적함수에서 제외한다.

    선언된 6개 항목(S1~S6) 외의 커스텀 SoftTerm 가중치는 항목 이름과 같은
    extra 필드로 넘긴다. 예: `ObjectiveWeights(first_period_load=2)`.
    지정하지 않은 커스텀 항목의 가중치는 1 로 본다(`weight_for` 참고).
    """

    model_config = ConfigDict(extra="allow")

    teacher_gap: int = Field(default=20, ge=0, description="교사 하루 일과 중간의 공강 1칸당 벌점")
    teacher_workday: int = Field(default=3, ge=0, description="교사 출근일 1일당 벌점(수업일 집중 배치 유도)")
    teacher_free_day: int = Field(default=30, ge=0, description="희망 공강일에 수업이 잡힐 때 벌점")
    class_gap: int = Field(default=10, ge=0, description="학급 하루 일과 중간의 공강 1칸당 벌점")
    avoided_period: int = Field(default=5, ge=0, description="과목 기피 교시에 배치될 때 벌점")
    non_preferred_period: int = Field(default=1, ge=0, description="과목 선호 교시를 벗어날 때 벌점")

    @model_validator(mode="after")
    def _check_extra_weights(self) -> ObjectiveWeights:
        """extra 로 들어온 커스텀 가중치도 선언 필드와 같은 규칙(0 이상 정수)을 따른다."""
        for name, value in (self.model_extra or {}).items():
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"커스텀 가중치 {name} 는 0 이상의 정수여야 합니다: {value!r}")
        return self

    def weight_for(self, name: str) -> int:
        """SoftTerm.name 에 해당하는 가중치. 선언/extra 어디에도 없는 이름은 1 로 본다."""
        return self.model_dump().get(name, 1)

    @property
    def is_empty(self) -> bool:
        return not any(self.model_dump().values())


class SolverConfig(BaseModel):
    """CP-SAT 실행 파라미터."""

    time_limit_seconds: float = Field(default_factory=lambda: env.SOLVER_TIME_LIMIT_SECONDS, gt=0)
    num_workers: int = Field(default_factory=lambda: env.SOLVER_NUM_WORKERS, ge=1)
    log_search_progress: bool = Field(default_factory=lambda: env.SOLVER_LOG_SEARCH_PROGRESS)
    random_seed: int = 0
    relative_gap_limit: float | None = Field(default=None, ge=0, description="이 상대 갭에 도달하면 조기 종료")
    weights: ObjectiveWeights = Field(default_factory=ObjectiveWeights)
