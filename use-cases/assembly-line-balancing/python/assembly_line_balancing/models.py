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
    warm_start: bool = Field(
        default=False,
        description="Start the search from the stations on `tasks`; tasks still at -1 go through the construction.",
    )

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


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape; tasks are matched by id. A task the solution
    leaves out, or holds at -1, is placed by the normal construction. Anything `solve` returns is accepted, feasible
    or not: `max_stations`, station domains and `fixed_station` stay hard penalties, so a seed may break them, and
    `solve` itself can return indices at or above the task count or `max_stations`.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("tasks"), list):
        raise ValueError("solution must be a problem model or a dict with a 'tasks' list")
    known = {task.id for task in problem.tasks}
    stations: dict[str, int] = {}
    for entry in solution["tasks"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or entry["id"] not in known:
            raise ValueError(f"solution references unknown task: {entry!r}")
        task_id = entry["id"]
        if task_id in stations:
            raise ValueError(f"solution assigns task {task_id!r} more than once")
        station = entry.get("station", -1)
        if type(station) is not int or station < -1:
            raise ValueError(f"task {task_id!r} has invalid station {station!r}")
        stations[task_id] = station
    seeded = problem.model_copy(deep=True)
    for task in seeded.tasks:
        task.station = stations.get(task.id, -1)
    seeded.stations_used = seeded.station_loads = seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded
