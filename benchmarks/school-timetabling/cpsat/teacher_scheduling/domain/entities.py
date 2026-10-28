"""시간표 편성 문제를 구성하는 기본 엔티티.

용어는 국내 중고등학교 시간표 편성 관행(컴시간알리미 등)을 따른다.
- 학급(ClassGroup): 1학년 3반 처럼 함께 움직이는 학생 집단
- 강좌(Course): "1학년 3반 / 수학 / 김철수 / 주 4시간" 처럼 배정 단위가 되는 수업
- 블록타임(block): 2교시 연속으로 붙여야 하는 연강
- 분반(parallel_group): 한 학급을 나눠 같은 시간에 서로 다른 교사가 진행하는 수업
"""

from pydantic import BaseModel, ConfigDict, Field, model_validator

from teacher_scheduling.domain.enums import Weekday


class TimeSlot(BaseModel):
    """요일 + 교시로 표현되는 하나의 수업 칸."""

    model_config = ConfigDict(frozen=True)

    day: Weekday
    period: int = Field(ge=1, description="1교시부터 시작하는 교시 번호")

    def __str__(self) -> str:
        return f"{self.day.label}{self.period}"


class Subject(BaseModel):
    """과목. 시간대 선호는 소프트 제약으로만 반영된다."""

    id: str
    name: str
    required_room_type: str | None = Field(default=None, description="필요한 특별실 종류. None 이면 자기 교실 사용")
    preferred_periods: frozenset[int] = Field(default=frozenset(), description="선호 교시. 비어 있으면 선호 없음")
    avoided_periods: frozenset[int] = Field(default=frozenset(), description="기피 교시 (예: 체육 1교시)")


class Teacher(BaseModel):
    """교사. 개인 사정(출장/겸임/육아시간)은 `unavailable` 로 막는다."""

    id: str
    name: str
    subject_ids: frozenset[str] = Field(default=frozenset(), description="담당 가능 과목. 비어 있으면 제한 없음")
    unavailable: frozenset[TimeSlot] = Field(default=frozenset(), description="수업 배정 불가 시간")
    max_periods_per_week: int | None = Field(default=None, ge=0)
    max_periods_per_day: int = Field(default=6, ge=1)
    max_consecutive_periods: int = Field(default=4, ge=1, description="쉬는 시간 없이 이어지는 최대 시수")
    preferred_free_days: frozenset[Weekday] = Field(default=frozenset(), description="공강일 희망 요일 (소프트)")
    homeroom_class_id: str | None = Field(default=None, description="담임을 맡은 학급 id")


class ClassGroup(BaseModel):
    """학급."""

    id: str
    name: str
    grade: int = Field(ge=1)
    unavailable: frozenset[TimeSlot] = Field(default=frozenset(), description="학급 자체가 수업 불가한 시간")


class Room(BaseModel):
    """교실 / 특별실."""

    id: str
    name: str
    room_type: str


class Course(BaseModel):
    """배정 단위 수업. 주당 시수만큼의 칸을 시간표에 꽂는 것이 목표다."""

    id: str
    class_group_id: str
    subject_id: str
    teacher_ids: tuple[str, ...] = Field(min_length=1, description="2명 이상이면 동시수업(팀티칭)")
    periods_per_week: int = Field(ge=1)
    block_periods: int = Field(default=0, ge=0, description="연강으로 붙일 시수. 0 또는 1이면 블록 없음")
    max_periods_per_day: int = Field(default=1, ge=1, description="같은 강좌를 하루에 배치할 수 있는 최대 시수")
    fixed_slots: frozenset[TimeSlot] = Field(default=frozenset(), description="반드시 이 시간에 넣어야 하는 칸")
    forbidden_slots: frozenset[TimeSlot] = Field(default=frozenset(), description="이 강좌만 따로 막을 시간")
    parallel_group: str | None = Field(default=None, description="같은 값을 가진 강좌끼리 동일 시간에 동시 진행(분반)")

    @model_validator(mode="after")
    def _check_consistency(self) -> Course:
        if self.block_periods > self.periods_per_week:
            raise ValueError(f"[{self.id}] block_periods 는 periods_per_week 를 넘을 수 없습니다")
        if len(self.fixed_slots) > self.periods_per_week:
            raise ValueError(f"[{self.id}] fixed_slots 가 periods_per_week 보다 많습니다")
        if overlap := self.fixed_slots & self.forbidden_slots:
            raise ValueError(f"[{self.id}] fixed 와 forbidden 이 겹칩니다: {sorted(map(str, overlap))}")
        return self

    @property
    def has_block(self) -> bool:
        return self.block_periods >= 2

    @property
    def effective_max_periods_per_day(self) -> int:
        """블록타임이 있으면 그 날은 최소한 블록 크기만큼은 허용해야 한다."""
        return max(self.max_periods_per_day, self.block_periods)

    @property
    def occupancy_key(self) -> str:
        """학급 점유 관점에서 하나로 묶이는 단위. 분반이면 그룹 전체가 한 칸을 쓴다."""
        return self.parallel_group or self.id
