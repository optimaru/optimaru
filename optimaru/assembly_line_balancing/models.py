"""The canonical assembly-line-balancing problem (pydantic v2) and its engine conversion.

`AssemblyLineBalancingProblemModel` is the JSON contract (tasks by id); `to_core()`
builds the C++ `_core.AssemblyLineBalancingProblem` (tasks by index) and
`from_core()` builds the model back, stations and results included::

    from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel

    core = AssemblyLineBalancingProblemModel.model_validate(data).to_core()
    alb.solve(core, time_limit_seconds=5)                    # in place
    result = AssemblyLineBalancingProblemModel.from_core(core)

The canonical problem carries no constraint weights: weights come only from
constraint selections, and the defaults are the C++ components'
(``assembly_line_balancing.constraint_info()``).
"""

from __future__ import annotations

from pydantic import BaseModel, Field, model_validator

from .._models import SolveRequest, StrictModel, solve_model_request
from . import _core

__all__ = [
    "AssemblyLineBalancingProblemModel",
    "AssemblyLineBalancingRequest",
    "PrecedenceModel",
    "TaskGroupModel",
    "TaskModel",
    "TaskPairModel",
    "solve_request",
    "with_initial_solution",
]


class TaskModel(StrictModel):
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


class PrecedenceModel(StrictModel):
    before: str
    after: str


class TaskPairModel(StrictModel):
    first: str
    second: str


class TaskGroupModel(StrictModel):
    id: str
    task_ids: list[str] = Field(min_length=2)


class AssemblyLineBalancingProblemModel(StrictModel):
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

    def to_core(self) -> _core.AssemblyLineBalancingProblem:
        """pydantic -> engine object: ids become task indices, `None` stays "no limit".

        The stations and `warm_start` are copied; the result fields are ignored (the
        engine recomputes them).
        """
        self.valid_ids()  # the fields stay mutable after validation
        index = {task.id: position for position, task in enumerate(self.tasks)}
        return _core.AssemblyLineBalancingProblem(
            self.cycle_time,
            [
                _core.Task(
                    task.id,
                    task.duration,
                    task.min_station,
                    task.max_station,
                    task.fixed_station,
                    list(task.forbidden_stations),
                    task.station,
                )
                for task in self.tasks
            ],
            [_core.Precedence(index[edge.before], index[edge.after]) for edge in self.precedences],
            self.max_stations,
            [
                _core.TaskGroup(group.id, [index[task_id] for task_id in group.task_ids])
                for group in self.same_station_groups
            ],
            [_core.TaskPair(index[pair.first], index[pair.second]) for pair in self.different_station_pairs],
            self.warm_start,
        )

    @classmethod
    def from_core(cls, problem: _core.AssemblyLineBalancingProblem) -> AssemblyLineBalancingProblemModel:
        """engine object -> pydantic: the facts, the stations, `warm_start` and the results
        (`station_loads`, `stations_used` from the stations; the score last computed on the core,
        0hard/0soft if it was never scored)."""
        ids = [task.id for task in problem.tasks]
        return cls(
            cycle_time=problem.cycle_time,
            tasks=[
                TaskModel(
                    id=task.id,
                    duration=task.duration,
                    min_station=task.min_station,
                    max_station=task.max_station,
                    fixed_station=task.fixed_station,
                    forbidden_stations=list(task.forbidden_stations),
                    station=task.station,
                )
                for task in problem.tasks
            ],
            precedences=[
                PrecedenceModel(before=ids[edge.before], after=ids[edge.after]) for edge in problem.precedences
            ],
            max_stations=problem.max_stations,
            same_station_groups=[
                TaskGroupModel(id=group.id, task_ids=[ids[task] for task in group.tasks])
                for group in problem.same_station_groups
            ],
            different_station_pairs=[
                TaskPairModel(first=ids[pair.first], second=ids[pair.second])
                for pair in problem.different_station_pairs
            ],
            stations_used=problem.stations_used(),
            station_loads=list(problem.station_loads()),
            score_hard=problem.score.hard,
            score_soft=problem.score.soft,
            warm_start=problem.warm_start,
        )

    def _update_from_core(self, problem: _core.AssemblyLineBalancingProblem) -> None:
        """Write a solved core's stations, results and `warm_start` back into this model, in place."""
        for task, station in zip(self.tasks, problem.assignment(), strict=True):
            task.station = station
        self.station_loads = list(problem.station_loads())
        self.stations_used = problem.stations_used()
        self.score_hard = problem.score.hard
        self.score_soft = problem.score.soft
        self.warm_start = problem.warm_start


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


class AssemblyLineBalancingRequest(SolveRequest):
    """The assembly line balancing problem with the settings of one solve (see `SolveRequest` for those)."""

    model_input: AssemblyLineBalancingProblemModel


def solve_request(request: AssemblyLineBalancingRequest, *, constraints=None) -> AssemblyLineBalancingProblemModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)
