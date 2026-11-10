"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Team/Round/Match/LeagueSchedule and SolverConfig 1:1. Use it to
store or transfer the engine shape as-is (debug dumps, regression snapshots,
...)::

    from optimaru.sports_league_scheduling.models import LeagueScheduleModel, SolveConfig

    model = LeagueScheduleModel.from_core(schedule)   # engine -> pydantic
    clone = LeagueScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                       # pydantic -> engine

`pydantic` is an optional dependency (`pip install optimaru.sports_league_scheduling[models]`).
"""

from pydantic import Field, model_validator

from .._models import SolveConfigBase, StrictModel, entities_by_id, require_same_order, solution_dict
from . import _core

__all__ = [
    "LeagueScheduleModel",
    "MatchModel",
    "RoundModel",
    "SolveConfig",
    "TeamModel",
    "with_initial_solution",
]


class TeamModel(StrictModel):
    """Problem fact: a team. A match's home/away reference positions in the team list."""

    id: str
    name: str = ""


class RoundModel(StrictModel):
    """A round, the value a match is assigned. index is the round number the
    constraints use (same day / next day / first / last checks), weekend_or_holiday feeds
    workday_classic_matches_cost."""

    index: int = Field(ge=0)
    weekend_or_holiday: bool = False


class MatchModel(StrictModel):
    """One match. round is a position in the rounds list (-1 = unassigned)."""

    id: str
    home_team: int = Field(ge=0)
    away_team: int = Field(ge=0)
    classic_match: bool = False
    round: int = -1


class LeagueScheduleModel(StrictModel):
    """Pydantic mirror of the schedule. score is a read-only result, kept as a separate field."""

    teams: list[TeamModel]
    rounds: list[RoundModel]
    matches: list[MatchModel]
    distance_to_team: list[list[int]] = Field(default_factory=list)
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def _check_indices(self):
        """`round` is a raw position the engine reads without a range check. It refuses bad teams itself."""
        value_count = len(self.rounds)
        for match in self.matches:
            if not -1 <= match.round < value_count:
                raise ValueError(f"match {match.id!r}: round {match.round!r} is out of range -1..{value_count - 1}")
        return self

    def to_core(self) -> "_core.LeagueSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        self._check_indices()  # the fields stay mutable after validation
        return _core.LeagueSchedule(
            [_core.Team(t.id, t.name) for t in self.teams],
            [_core.Round(r.index, r.weekend_or_holiday) for r in self.rounds],
            [
                _core.Match(
                    m.id,
                    m.home_team,
                    m.away_team,
                    classic_match=m.classic_match,
                    round=m.round,
                )
                for m in self.matches
            ],
            [list(row) for row in self.distance_to_team],
        )

    @classmethod
    def from_core(cls, schedule: "_core.LeagueSchedule") -> "LeagueScheduleModel":
        """engine object -> pydantic (including the current assignment and score)."""
        return cls(
            teams=[TeamModel(id=t.id, name=t.name) for t in schedule.teams],
            rounds=[RoundModel(index=r.index, weekend_or_holiday=r.weekend_or_holiday) for r in schedule.rounds],
            matches=[
                MatchModel(
                    id=m.id,
                    home_team=m.home_team,
                    away_team=m.away_team,
                    classic_match=m.classic_match,
                    round=m.round,
                )
                for m in schedule.matches
            ],
            distance_to_team=[list(row) for row in schedule.distance_to_team],
            score_hard=schedule.score.hard,
            score_soft=schedule.score.soft,
        )


class SolveConfig(SolveConfigBase):
    """Serializable mirror of `_core.SolverConfig` (for `solve(config=...)`).

    ``late_acceptance_size`` 0 is automatic: ``clamp(8 * matches, 400, 3200)``. A positive value
    is used as it is.
    """

    def to_core(self) -> "_core.SolverConfig":
        return self._apply_to(_core.SolverConfig())


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns (`LeagueScheduleModel.from_core`), or a plain dict
    of that shape (the JSON the API returned). Matches are matched by id. A match left
    out becomes unassigned and the construction heuristic places it, and only it, so
    a complete solution starts the search exactly from it. `problem` is not mutated.

    `round` is a position in `rounds`, not `Round.index`, so it is only valid for the
    same rounds in the same order: ValueError if `solution` lists different ones.

    Raises ValueError naming the match for an unknown or repeated id and for a `round`
    outside -1..round_count - 1 (the engine does not range-check).
    """
    if not isinstance(problem, LeagueScheduleModel):
        raise TypeError(f"problem must be a LeagueScheduleModel: {type(problem)!r}")
    solved = solution_dict(solution)
    require_same_order(solved, "rounds", [r.index for r in problem.rounds], key="index")
    matches = entities_by_id(solved, "matches", [m.id for m in problem.matches])
    value_count = len(problem.rounds)
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for match in seeded.matches:
        value = matches.get(match.id, {}).get("round", -1)
        if not isinstance(value, int) or isinstance(value, bool) or not -1 <= value < value_count:
            raise ValueError(f"match {match.id!r}: round {value!r} is out of range -1..{value_count - 1}")
        match.round = value
    return seeded
