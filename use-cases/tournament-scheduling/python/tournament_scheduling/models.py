"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Team/Day/UnavailabilityPenalty/TeamAssignment/TournamentSchedule 을
1:1 로 미러링한다. 팀/일자는 리스트 인덱스로 참조한다 (Java 의 객체 참조 대신;
Team 의 @PlanningId == teams 인덱스, Day 의 dateIndex == days 인덱스가 관례).

점수 매핑 (Java HardMediumSoftBigDecimalScore -> 엔진 HardSoftScore):
``score_hard`` 는 Java hard 그대로, ``score_soft`` 는
``-(medium 불공정도 * 1e6 + soft 불공정도)`` — medium(팀별 배정 수 균형)이
soft(대결 수 균형)를 항상 지배하도록 1e6 오프셋으로 접었다::

    from tournament_scheduling.models import TournamentScheduleModel, demo_data

    model = demo_data()                          # Java DemoDataGenerator 재현
    clone = TournamentScheduleModel.model_validate_json(model.model_dump_json())
    schedule = clone.to_core()                   # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install tournament_scheduling[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field

from . import _core

__all__ = [
    "DayModel",
    "TeamAssignmentModel",
    "TeamModel",
    "TournamentScheduleModel",
    "UnavailabilityPenaltyModel",
    "demo_data",
]


class TeamModel(BaseModel):
    """참가 팀. 리스트 인덱스가 Java 의 @PlanningId 역할을 한다."""

    model_config = ConfigDict(extra="forbid")

    name: str


class DayModel(BaseModel):
    """경기 일자. date_index 는 정보용 (참조는 days 리스트 인덱스로)."""

    model_config = ConfigDict(extra="forbid")

    date_index: int = Field(ge=0)


class UnavailabilityPenaltyModel(BaseModel):
    """불가일: (team, day) 에 배정이 하나라도 있으면 하드 1점."""

    model_config = ConfigDict(extra="forbid")

    team: int = Field(ge=0)
    day: int = Field(ge=0)


class TeamAssignmentModel(BaseModel):
    """planning entity: 일자의 경기 슬롯 하나. team 이 planning variable
    (None == 미배정); id 는 유일해야 한다 (Java @PlanningId)."""

    model_config = ConfigDict(extra="forbid")

    id: int
    day: int = Field(ge=0)
    index_in_day: int = Field(default=0, ge=0)
    team: int | None = None
    pinned: bool = False


class TournamentScheduleModel(BaseModel):
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

    model_config = ConfigDict(extra="forbid")

    teams: list[TeamModel]
    days: list[DayModel]
    unavailability_penalties: list[UnavailabilityPenaltyModel] = Field(default_factory=list)
    team_assignments: list[TeamAssignmentModel] = Field(default_factory=list)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.TournamentSchedule":
        """pydantic -> 엔진 객체 (score 필드는 엔진이 다시 계산하므로 무시)."""
        return _core.TournamentSchedule(
            [_core.Team(t.name) for t in self.teams],
            [_core.Day(d.date_index) for d in self.days],
            [_core.UnavailabilityPenalty(p.team, p.day) for p in self.unavailability_penalties],
            [
                _core.TeamAssignment(
                    a.id,
                    a.day,
                    index_in_day=a.index_in_day,
                    team=-1 if a.team is None else a.team,
                    pinned=a.pinned,
                )
                for a in self.team_assignments
            ],
        )

    @classmethod
    def from_core(cls, schedule: "_core.TournamentSchedule") -> "TournamentScheduleModel":
        """엔진 객체 -> pydantic (현재 배정과 score 포함)."""
        return cls(
            teams=[TeamModel(name=t.name) for t in schedule.teams],
            days=[DayModel(date_index=d.date_index) for d in schedule.days],
            unavailability_penalties=[
                UnavailabilityPenaltyModel(team=p.team, day=p.day) for p in schedule.unavailability_penalties
            ],
            team_assignments=[
                TeamAssignmentModel(
                    id=a.id,
                    day=a.day,
                    index_in_day=a.index_in_day,
                    team=None if a.team < 0 else a.team,
                    pinned=a.pinned,
                )
                for a in schedule.team_assignments
            ],
            score_hard=schedule.score.hard,
            score_soft=schedule.score.soft,
        )


class _JavaRandom:
    """java.util.Random (48-bit LCG) 재현 — Java 데모 데이터와 동일한 난수."""

    _MULTIPLIER = 0x5DEECE66D
    _ADDEND = 0xB
    _MASK = (1 << 48) - 1

    def __init__(self, seed: int) -> None:
        self._seed = (seed ^ self._MULTIPLIER) & self._MASK

    def _next(self, bits: int) -> int:
        self._seed = (self._seed * self._MULTIPLIER + self._ADDEND) & self._MASK
        return self._seed >> (48 - bits)

    def next_int(self, bound: int) -> int:
        if bound & (bound - 1) == 0:  # power of two
            return (bound * self._next(31)) >> 31
        while True:
            bits = self._next(31)
            val = bits % bound
            if bits - val + (bound - 1) < 2**31:  # Java int overflow check
                return val


def demo_data() -> TournamentScheduleModel:
    """Java quickstart 의 DemoDataGenerator 재현: 팀 7, 18일 x 슬롯 4 = 배정
    72, 불가일 12 (java.util.Random(0) 과 같은 추첨)."""
    random = _JavaRandom(0)
    teams = ["Maarten", "Geoffrey", "Lukas", "Chris", "Fred", "Radek", "Maciej"]
    day_count = 18
    penalties: list[UnavailabilityPenaltyModel] = []
    while len(penalties) < 12:
        team = random.next_int(len(teams))
        day = random.next_int(day_count)
        if all(p.team != team or p.day != day for p in penalties):
            penalties.append(UnavailabilityPenaltyModel(team=team, day=day))
    assignments = [TeamAssignmentModel(id=d * 4 + i, day=d, index_in_day=i) for d in range(day_count) for i in range(4)]
    return TournamentScheduleModel(
        teams=[TeamModel(name=name) for name in teams],
        days=[DayModel(date_index=d) for d in range(day_count)],
        unavailability_penalties=penalties,
        team_assignments=assignments,
    )
