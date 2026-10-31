"""Solver settings and objective weights."""

from pydantic import BaseModel, ConfigDict, Field, model_validator

from teacher_scheduling.config import env


class ObjectiveWeights(BaseModel):
    """Soft-constraint weights. 0 removes the term from the objective.

    Weights of custom SoftTerms beyond the six declared ones (S1–S6) are passed
    as extra fields named after the term, e.g. `ObjectiveWeights(first_period_load=2)`.
    A custom term without a weight counts as 1 (see `weight_for`).
    """

    model_config = ConfigDict(extra="allow")

    teacher_gap: int = Field(default=20, ge=0, description="Penalty per idle period in the middle of a teacher's day")
    teacher_workday: int = Field(default=3, ge=0, description="Penalty per teacher working day (favours compact teaching days)")
    teacher_free_day: int = Field(default=30, ge=0, description="Penalty when a requested free day gets a lesson")
    class_gap: int = Field(default=10, ge=0, description="Penalty per idle period in the middle of a class's day")
    avoided_period: int = Field(default=5, ge=0, description="Penalty for a lesson in one of its subject's avoided periods")
    non_preferred_period: int = Field(default=1, ge=0, description="Penalty for a lesson outside its subject's preferred periods")

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
    relative_gap_limit: float | None = Field(default=None, ge=0, description="Stop early once this relative gap is reached")
    weights: ObjectiveWeights = Field(default_factory=ObjectiveWeights)
