"""Solver settings and objective weights."""

from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator
from optimaru.school_timetabling import constraint_info

from teacher_scheduling.config import env


def _default_weights() -> dict[str, int]:
    """The engine's default weight of each soft constraint (C++ kDefaultWeight) — the one source.

    CP-SAT objectives are integral, so a non-integral engine default fails loudly here
    instead of being truncated."""
    weights = {}
    for row in constraint_info():
        if row.default_level != "soft":
            continue
        if not float(row.default_weight).is_integer():
            raise ValueError(f"{row.name}: CP-SAT needs an integer default weight, not {row.default_weight!r}")
        weights[row.name] = int(row.default_weight)
    return weights


_DEFAULT_WEIGHTS = _default_weights()


def _weight_field(name: str, description: str):
    return Field(default=_DEFAULT_WEIGHTS[name], ge=0, description=description)


class ObjectiveWeights(BaseModel):
    """Soft-constraint weights, keyed by the soft constraint names. 0 removes the term from the objective.

    The defaults are the engine's (`optimaru.school_timetabling.constraint_info()`). Weights
    of custom SoftTerms beyond the six declared ones are passed as extra fields
    named after the term, e.g. `ObjectiveWeights(first_period_load=2)`. A custom
    term without a weight counts as 1 (see `weight_for`).
    """

    model_config = ConfigDict(extra="allow")

    teacher_gap_cost: int = _weight_field(
        "teacher_gap_cost", "Penalty per idle period in the middle of a teacher's day"
    )
    teacher_workday_cost: int = _weight_field(
        "teacher_workday_cost", "Penalty per teacher working day (favours compact teaching days)"
    )
    teacher_free_day_cost: int = _weight_field(
        "teacher_free_day_cost", "Penalty per worked requested free day (a lesson on a preferred free day)"
    )
    class_gap_cost: int = _weight_field("class_gap_cost", "Penalty per idle period in the middle of a class's day")
    avoided_period_cost: int = _weight_field(
        "avoided_period_cost", "Penalty for a lesson in one of its subject's avoided periods"
    )
    non_preferred_period_cost: int = _weight_field(
        "non_preferred_period_cost", "Penalty for a lesson outside its subject's preferred periods"
    )

    @model_validator(mode="after")
    def _check_extra_weights(self) -> ObjectiveWeights:
        """Custom weights passed as extras follow the same rule as the declared fields (non-negative integers)."""
        for name, value in (self.model_extra or {}).items():
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"custom weight {name} must be a non-negative integer: {value!r}")
        return self

    def weight_for(self, name: str) -> int:
        """The weight for SoftTerm.name. A name neither declared nor in extras counts as 1."""
        return self.model_dump().get(name, 1)


class SolverConfig(BaseModel):
    """CP-SAT run parameters."""

    time_limit_seconds: float = Field(default_factory=lambda: env.SOLVER_TIME_LIMIT_SECONDS, gt=0)
    num_workers: int = Field(default_factory=lambda: env.SOLVER_NUM_WORKERS, ge=1)
    log_search_progress: bool = Field(default_factory=lambda: env.SOLVER_LOG_SEARCH_PROGRESS)
    random_seed: int = 0
    relative_gap_limit: float | None = Field(
        default=None, ge=0, description="Stop early once this relative gap is reached"
    )
    weights: ObjectiveWeights = Field(default_factory=ObjectiveWeights)
