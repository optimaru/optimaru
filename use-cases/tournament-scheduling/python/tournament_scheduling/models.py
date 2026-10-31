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

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "DayModel",
    "TeamAssignmentModel",
    "TeamModel",
    "TournamentScheduleModel",
    "UnavailabilityPenaltyModel",
    "demo_data",
    "with_initial_solution",
]


def _valid_positions(count: int) -> str:
    """The valid positions among `count`, for an error message."""
    return f"0..{count - 1}" if count else "(the list is empty)"


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

    @model_validator(mode="after")
    def _check_indices(self):
        """Team and day positions the engine reads without a range check. A penalty has no id, so
        its errors give its place in the list."""
        team_count = len(self.teams)
        day_count = len(self.days)
        valid_teams = _valid_positions(team_count)
        valid_days = _valid_positions(day_count)
        for position, penalty in enumerate(self.unavailability_penalties):
            if not 0 <= penalty.team < team_count:
                raise ValueError(
                    f"unavailability penalty {position}: team {penalty.team!r} is out of range {valid_teams}"
                )
            if not 0 <= penalty.day < day_count:
                raise ValueError(
                    f"unavailability penalty {position}: day {penalty.day!r} is out of range {valid_days}"
                )
        for assignment in self.team_assignments:
            if not 0 <= assignment.day < day_count:
                raise ValueError(
                    f"team assignment {assignment.id!r}: day {assignment.day!r} is out of range {valid_days}"
                )
            if assignment.team is not None and not 0 <= assignment.team < team_count:
                raise ValueError(
                    f"team assignment {assignment.id!r}: team {assignment.team!r} is out of range {valid_teams}"
                )
        return self

    def to_core(self) -> "_core.TournamentSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        self._check_indices()  # the fields stay mutable after validation
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


def _solution_dict(solution) -> dict:
    if isinstance(solution, BaseModel):
        return solution.model_dump()
    if isinstance(solution, dict):
        return solution
    raise ValueError(f"solution must be a solved model or a dict of that shape: {type(solution)!r}")


def _entities_by_id(solved: dict, name: str, ids: list, key: str = "id") -> dict:
    """`solved[name]` by id. Rejects an id repeated on either side and an id outside `ids`."""
    entities = solved.get(name)
    if not isinstance(entities, list):
        raise ValueError(f"solution has no {name!r} list")
    known = set(ids)
    if len(known) != len(ids):
        raise ValueError(f"problem repeats an id in {name!r}: {next(i for i in ids if ids.count(i) > 1)!r}")
    by_id: dict = {}
    for entity in entities:
        entity_id = entity.get(key) if isinstance(entity, dict) else None
        if not isinstance(entity_id, (str, int)) or entity_id not in known:
            raise ValueError(f"solution has an unknown id in {name!r}: {entity_id!r}")
        if entity_id in by_id:
            raise ValueError(f"solution repeats an id in {name!r}: {entity_id!r}")
        by_id[entity_id] = entity
    return by_id


def _require_same_order(solved: dict, name: str, ids: list, key: str = "id") -> None:
    """A positional value only means the same thing over the same list. `solved` may omit the list."""
    values = solved.get(name)
    if values is None:
        return
    if not isinstance(values, list) or [v.get(key) if isinstance(v, dict) else None for v in values] != ids:
        raise ValueError(f"solution {name!r} list differs from the problem's (same entries, same order required)")


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns: a `TournamentScheduleModel`, or a plain dict of
    that shape (the JSON the API returned). Assignments are matched by id. An
    assignment left out becomes unassigned and the construction heuristic places it,
    and only it, so a complete solution starts the search exactly from it. `problem`
    is not mutated. Pinned assignments keep their team on `problem`, whatever
    `solution` says.

    `team` is a position in `teams`, so it is only valid for the same teams in the
    same order: ValueError if `solution` lists different ones.

    Raises ValueError naming the assignment for an unknown or repeated id and for a
    `team` outside 0..team_count - 1 (None is unassigned; the engine does not
    range-check).
    """
    if not isinstance(problem, TournamentScheduleModel):
        raise TypeError(f"problem must be a TournamentScheduleModel: {type(problem)!r}")
    solved = _solution_dict(solution)
    _require_same_order(solved, "teams", [t.name for t in problem.teams], key="name")
    assignments = _entities_by_id(solved, "team_assignments", [a.id for a in problem.team_assignments])
    value_count = len(problem.teams)
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for assignment in seeded.team_assignments:
        if not assignment.pinned:
            assignment.team = assignments.get(assignment.id, {}).get("team")
        value = assignment.team
        if value is None:
            continue
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value < value_count:
            raise ValueError(
                f"team assignment {assignment.id!r}: team {value!r} is out of range {_valid_positions(value_count)}"
            )
    return seeded
