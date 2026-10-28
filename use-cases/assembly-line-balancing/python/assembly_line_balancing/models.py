from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator


class TaskModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str
    duration: int = Field(gt=0)
    min_station: int = Field(default=0, ge=0)
    max_station: int | None = Field(default=None, ge=0)
    fixed_station: int | None = Field(default=None, ge=0)
    forbidden_stations: list[int] = Field(default_factory=list)
    station: int = Field(default=-1, ge=-1)

    @model_validator(mode="after")
    def valid_station_domain(self) -> TaskModel:
        if self.max_station is not None and self.min_station > self.max_station:
            raise ValueError("min_station must not exceed max_station")
        if self.fixed_station is not None and (
            self.fixed_station < self.min_station
            or (self.max_station is not None and self.fixed_station > self.max_station)
            or self.fixed_station in self.forbidden_stations
        ):
            raise ValueError("fixed_station must be inside the allowed station domain")
        return self


class PrecedenceModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    before: str
    after: str


class TaskPairModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    first: str
    second: str


class TaskGroupModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str
    task_ids: list[str] = Field(min_length=2)


class AssemblyLineBalancingProblemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    cycle_time: int = Field(gt=0, description="Maximum processing time assigned to one station.")
    tasks: list[TaskModel] = Field(description="Tasks that must each be assigned to one station.")
    precedences: list[PrecedenceModel] = Field(default_factory=list, description="Before/after task arcs.")
    max_stations: int | None = Field(default=None, ge=1)
    same_station_groups: list[TaskGroupModel] = Field(default_factory=list)
    different_station_pairs: list[TaskPairModel] = Field(default_factory=list)
    stations_used: int | None = Field(default=None, description="Number of non-empty stations.")
    station_loads: list[int] | None = Field(default=None, description="Processing-time load of each station.")
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def valid_ids(self) -> AssemblyLineBalancingProblemModel:
        ids = {task.id for task in self.tasks}
        if len(ids) != len(self.tasks):
            raise ValueError("task ids must be unique")
        references = {endpoint for edge in self.precedences for endpoint in (edge.before, edge.after)}
        references.update(task_id for group in self.same_station_groups for task_id in group.task_ids)
        references.update(endpoint for pair in self.different_station_pairs for endpoint in (pair.first, pair.second))
        if references - ids:
            raise ValueError("precedence endpoints must be tasks")
        station_count = len(self.tasks)
        for task in self.tasks:
            values = [task.min_station, *task.forbidden_stations]
            if task.max_station is not None:
                values.append(task.max_station)
            if task.fixed_station is not None:
                values.append(task.fixed_station)
            if station_count and any(station >= station_count for station in values):
                raise ValueError("task station domains must use indices below the task count")
        return self

    def to_core(self) -> AssemblyLineBalancingProblemModel:
        return self.model_copy(deep=True)

    @classmethod
    def from_core(cls, problem: AssemblyLineBalancingProblemModel) -> AssemblyLineBalancingProblemModel:
        return problem.model_copy(deep=True)
