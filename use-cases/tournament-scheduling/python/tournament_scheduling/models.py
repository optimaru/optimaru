"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Team/Day/UnavailabilityPenalty/TeamAssignment/TournamentSchedule
1:1. Teams/days are referenced by list index (instead of Java's object references; by
convention Team's @PlanningId == teams index and Day's dateIndex == days index).

Score mapping (Java HardMediumSoftBigDecimalScore -> engine HardSoftScore):
``score_hard`` is the Java hard as-is, ``score_soft`` is
``-(medium unfairness * 1e6 + soft unfairness)`` — folded with a 1e6 offset so medium
(per-team assignment balance) always dominates soft (head-to-head balance)::

    from tournament_scheduling.models import TournamentScheduleModel, demo_data

    model = demo_data()                          # reproduces Java DemoDataGenerator
    clone = TournamentScheduleModel.model_validate_json(model.model_dump_json())
    schedule = clone.to_core()                   # pydantic -> engine

`pydantic` is an optional dependency (`pip install tournament_scheduling[models]`).
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
    """A participating team. The list index plays the role of Java's @PlanningId."""

    model_config = ConfigDict(extra="forbid")

    name: str


class DayModel(BaseModel):
    """A match day. date_index is informational (references use the days list index)."""

    model_config = ConfigDict(extra="forbid")

    date_index: int = Field(ge=0)


class UnavailabilityPenaltyModel(BaseModel):
    """Unavailability: one hard point when (team, day) has at least one assignment."""

    model_config = ConfigDict(extra="forbid")

    team: int = Field(ge=0)
    day: int = Field(ge=0)


class TeamAssignmentModel(BaseModel):
    """Planning entity: one match slot of a day. team is the planning variable
    (None == unassigned); ids must be unique (Java @PlanningId)."""

    model_config = ConfigDict(extra="forbid")

    id: int
    day: int = Field(ge=0)
    index_in_day: int = Field(default=0, ge=0)
    team: int | None = None
    pinned: bool = False


class TournamentScheduleModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field."""

    model_config = ConfigDict(extra="forbid")

    teams: list[TeamModel]
    days: list[DayModel]
    unavailability_penalties: list[UnavailabilityPenaltyModel] = Field(default_factory=list)
    team_assignments: list[TeamAssignmentModel] = Field(default_factory=list)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.TournamentSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
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
        """engine object -> pydantic (including the current assignment and score)."""
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
    """Reproduction of java.util.Random (48-bit LCG) — the same random numbers as the Java demo data."""

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
    """Reproduction of the Java quickstart DemoDataGenerator: 7 teams, 18 days x 4 slots = 72
    assignments, 12 unavailable days (the same draws as java.util.Random(0))."""
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
