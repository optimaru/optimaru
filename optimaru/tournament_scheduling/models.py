"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Team/Day/UnavailabilityPenalty/TeamAssignment/TournamentSchedule
1:1. Teams/days are referenced by list index (a team is identified by its teams index,
a day by its days index).

Score mapping (hard / medium / soft -> engine Score):
``score_hard`` is the hard level as-is, ``score_soft`` is
``-(medium unfairness * 1e6 + soft unfairness)`` — folded with a 1e6 offset so medium
(per-team assignment balance) always dominates soft (head-to-head balance)::

    from optimaru.tournament_scheduling.models import TournamentScheduleModel, generate_demo_data

    model = generate_demo_data()                 # the demo instance
    clone = TournamentScheduleModel.model_validate_json(model.model_dump_json())
    schedule = clone.to_core()                   # pydantic -> engine
"""

import random

from pydantic import Field, model_validator

from .._models import (
    SolveRequest,
    StrictModel,
    entities_by_id,
    require_same_order,
    solution_dict,
    solve_model_request,
    valid_positions,
)
from . import _core

__all__ = [
    "DayModel",
    "TeamAssignmentModel",
    "TeamModel",
    "TournamentScheduleModel",
    "TournamentScheduleRequest",
    "UnavailabilityPenaltyModel",
    "generate_demo_data",
    "solve_request",
    "with_initial_solution",
]


class TeamModel(StrictModel):
    """A participating team, identified by its list index."""

    name: str


class DayModel(StrictModel):
    """A match day. date_index is informational (references use the days list index)."""

    date_index: int = Field(ge=0)


class UnavailabilityPenaltyModel(StrictModel):
    """Unavailability: one hard point when (team, day) has at least one assignment."""

    team: int = Field(ge=0)
    day: int = Field(ge=0)


class TeamAssignmentModel(StrictModel):
    """One match slot of a day. team is the decision variable
    (None == unassigned); ids must be unique."""

    id: int
    day: int = Field(ge=0)
    index_in_day: int = Field(default=0, ge=0)
    team: int | None = None
    pinned: bool = False


class TournamentScheduleModel(StrictModel):
    """Pydantic mirror of the schedule. score is a read-only result, kept as a separate field."""

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
        valid_teams = valid_positions(team_count)
        valid_days = valid_positions(day_count)
        for position, penalty in enumerate(self.unavailability_penalties):
            if not 0 <= penalty.team < team_count:
                raise ValueError(
                    f"unavailability penalty {position}: team {penalty.team!r} is out of range {valid_teams}"
                )
            if not 0 <= penalty.day < day_count:
                raise ValueError(f"unavailability penalty {position}: day {penalty.day!r} is out of range {valid_days}")
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


def _pick(rng: random.Random, n: int) -> int:
    """An index in [0, n). Built on random(), whose output Python guarantees per seed."""
    return int(rng.random() * n)


def generate_demo_data() -> TournamentScheduleModel:
    """The demo instance: 7 teams, 18 days x 4 slots = 72
    assignments, 12 unavailable days drawn from a seeded ``random.Random`` (deterministic)."""
    rng = random.Random(0)
    teams = ["Maarten", "Geoffrey", "Lukas", "Chris", "Fred", "Radek", "Maciej"]
    day_count = 18
    penalties: list[UnavailabilityPenaltyModel] = []
    while len(penalties) < 12:
        team = _pick(rng, len(teams))
        day = _pick(rng, day_count)
        if all(p.team != team or p.day != day for p in penalties):
            penalties.append(UnavailabilityPenaltyModel(team=team, day=day))
    assignments = [TeamAssignmentModel(id=d * 4 + i, day=d, index_in_day=i) for d in range(day_count) for i in range(4)]
    return TournamentScheduleModel(
        teams=[TeamModel(name=name) for name in teams],
        days=[DayModel(date_index=d) for d in range(day_count)],
        unavailability_penalties=penalties,
        team_assignments=assignments,
    )


demo_data = generate_demo_data  # the name before 0.5.1


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
    solved = solution_dict(solution)
    require_same_order(solved, "teams", [t.name for t in problem.teams], key="name")
    assignments = entities_by_id(solved, "team_assignments", [a.id for a in problem.team_assignments])
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
                f"team assignment {assignment.id!r}: team {value!r} is out of range {valid_positions(value_count)}"
            )
    return seeded


class TournamentScheduleRequest(SolveRequest):
    """The tournament schedule with the settings of one solve (see `SolveRequest` for those)."""

    model_input: TournamentScheduleModel


def solve_request(request: TournamentScheduleRequest, *, constraints=None) -> TournamentScheduleModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)
