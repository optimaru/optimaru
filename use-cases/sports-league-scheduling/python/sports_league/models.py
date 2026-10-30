"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Team/Round/Match/LeagueSchedule and SolverConfig 1:1. Use it to
store or transfer the engine shape as-is (debug dumps, regression snapshots,
...)::

    from sports_league.models import LeagueScheduleModel, SolveConfig

    model = LeagueScheduleModel.from_core(schedule)   # engine -> pydantic
    clone = LeagueScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                       # pydantic -> engine

`pydantic` is an optional dependency (`pip install sports_league[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "LeagueScheduleModel",
    "MatchModel",
    "RoundModel",
    "SolveConfig",
    "TeamModel",
    "with_initial_solution",
]


class TeamModel(BaseModel):
    """Problem fact: a team. A match's home/away reference positions in the team list."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str = ""


class RoundModel(BaseModel):
    """Problem fact and planning value: a round. index is the round number the
    constraints use (same day / next day / first / last checks), weekend_or_holiday feeds
    the classic-match soft constraint."""

    model_config = ConfigDict(extra="forbid")

    index: int = Field(ge=0)
    weekend_or_holiday: bool = False


class MatchModel(BaseModel):
    """Planning entity. round is a position in the rounds list (-1 = unassigned)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    home_team: int = Field(ge=0)
    away_team: int = Field(ge=0)
    classic_match: bool = False
    round: int = -1


class LeagueScheduleModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field."""

    model_config = ConfigDict(extra="forbid")

    teams: list[TeamModel]
    rounds: list[RoundModel]
    matches: list[MatchModel]
    distance_to_team: list[list[int]] = Field(default_factory=list)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.LeagueSchedule":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
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


class SolveConfig(BaseModel):
    """Serializable mirror of `_core.SolverConfig` (for `solve(config=...)`)."""

    model_config = ConfigDict(extra="forbid")

    time_limit_seconds: float = Field(default=30.0, ge=0)
    random_seed: int = 0
    parallel_solver_count: int = Field(default=1, ge=1)
    # 0 = automatic (clamp(8 * matches, 400, 3200)). A positive value is used as-is.
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

    @model_validator(mode="after")
    def _valid_ruin_fraction_range(self):
        if self.ruin_recreate_min_ruin_fraction > self.ruin_recreate_max_ruin_fraction:
            raise ValueError("ruin_recreate_min_ruin_fraction must be <= ruin_recreate_max_ruin_fraction")
        return self

    def to_core(self) -> "_core.SolverConfig":
        cfg = _core.SolverConfig()
        cfg.spent_limit_seconds = self.time_limit_seconds
        cfg.random_seed = self.random_seed
        cfg.parallel_solver_count = self.parallel_solver_count
        cfg.late_acceptance_size = self.late_acceptance_size  # 0 = auto
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
    solved = _solution_dict(solution)
    _require_same_order(solved, "rounds", [r.index for r in problem.rounds], key="index")
    matches = _entities_by_id(solved, "matches", [m.id for m in problem.matches])
    value_count = len(problem.rounds)
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for match in seeded.matches:
        value = matches.get(match.id, {}).get("round", -1)
        if not isinstance(value, int) or isinstance(value, bool) or not -1 <= value < value_count:
            raise ValueError(f"match {match.id!r}: round {value!r} is out of range -1..{value_count - 1}")
        match.round = value
    return seeded
