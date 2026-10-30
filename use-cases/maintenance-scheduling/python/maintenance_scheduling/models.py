"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Crew/Job/MaintenanceSchedule 1:1, but dates are handled as
`datetime.date` (epoch day integers on the engine side). Assignments are stored as a
human-readable (crew_id, start_date) pair instead of the composite crew_date, and end_date
(the weekend-skipping shadow) is a result-only field::

    from maintenance_scheduling.models import MaintenanceScheduleModel

    model = MaintenanceScheduleModel.from_core(schedule)  # engine -> pydantic
    clone = MaintenanceScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                           # pydantic -> engine

`pydantic` is an optional dependency (`pip install maintenance_scheduling[models]`).
"""

from datetime import date, timedelta

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter

from . import _core

__all__ = [
    "CrewModel",
    "JobModel",
    "MaintenanceScheduleModel",
    "date_to_epoch",
    "epoch_to_date",
    "with_initial_solution",
]

_EPOCH = date(1970, 1, 1)
_START_DATE = TypeAdapter(date | None)


def date_to_epoch(d: date) -> int:
    """datetime.date -> epoch day (Java LocalDate.toEpochDay)."""
    return (d - _EPOCH).days


def epoch_to_date(epoch_day: int) -> date:
    """epoch day -> datetime.date."""
    return _EPOCH + timedelta(days=epoch_day)


def _opt_to_epoch(d: date | None) -> int | None:
    return None if d is None else date_to_epoch(d)


def _opt_to_date(epoch_day: int | None) -> date | None:
    return None if epoch_day is None else epoch_to_date(epoch_day)


class CrewModel(BaseModel):
    """A crew. One crew can do at most one job per day."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str


class JobModel(BaseModel):
    """Planning entity. crew_id/start_date are the assignment (both set means assigned),
    end_date is the weekend-skipping shadow (result only, ignored by to_core)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str
    duration_in_days: int = Field(ge=0)
    min_start_date: date | None = None  # inclusive
    max_end_date: date | None = None  # exclusive
    ideal_end_date: date | None = None  # exclusive
    tags: list[str] = Field(default_factory=list)
    crew_id: str | None = None
    start_date: date | None = None
    end_date: date | None = None
    pinned: bool = False


class MaintenanceScheduleModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field."""

    model_config = ConfigDict(extra="forbid")

    from_date: date  # inclusive
    to_date: date  # exclusive
    crews: list[CrewModel]
    jobs: list[JobModel]
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.MaintenanceSchedule":
        """pydantic -> engine object. The crew_id/start_date assignment is encoded as the
        composite crew_date value (start_date must be a weekday within the calendar)."""
        schedule = _core.MaintenanceSchedule(
            date_to_epoch(self.from_date),
            date_to_epoch(self.to_date),
            [_core.Crew(c.id, c.name) for c in self.crews],
            [
                _core.Job(
                    j.id,
                    j.name,
                    j.duration_in_days,
                    _opt_to_epoch(j.min_start_date),
                    _opt_to_epoch(j.max_end_date),
                    _opt_to_epoch(j.ideal_end_date),
                    list(j.tags),
                )
                for j in self.jobs
            ],
        )
        crew_index = {c.id: i for i, c in enumerate(self.crews)}
        date_index = {epoch: i for i, epoch in enumerate(schedule.start_dates())}
        assignment = []
        pinned_flags = []
        for j in self.jobs:
            pinned_flags.append(j.pinned)
            if j.crew_id is None or j.start_date is None:
                assignment.append(-1)
                continue
            if j.crew_id not in crew_index:
                raise ValueError(f"job {j.id}: unknown crew_id {j.crew_id!r}")
            epoch = date_to_epoch(j.start_date)
            if epoch not in date_index:
                raise ValueError(f"job {j.id}: start_date {j.start_date} is not a weekday inside the work calendar")
            assignment.append(schedule.crew_date_of(date_index[epoch], crew_index[j.crew_id]))
        schedule.set_assignment(assignment)
        # .jobs returns copies, so pinned flags are written back in bulk too.
        jobs = schedule.jobs
        for job, pinned in zip(jobs, pinned_flags):
            job.pinned = pinned
        schedule.jobs = jobs
        return schedule

    @classmethod
    def from_core(cls, schedule: "_core.MaintenanceSchedule") -> "MaintenanceScheduleModel":
        """engine object -> pydantic (including the current assignment, the end_date shadow and score)."""
        crews = [CrewModel(id=c.id, name=c.name) for c in schedule.crews]
        jobs = []
        for i, job in enumerate(schedule.jobs):
            crew_id = None
            if job.crew_date >= 0:
                crew_id = crews[schedule.crew_index_of(job.crew_date)].id
            jobs.append(
                JobModel(
                    id=job.id,
                    name=job.name,
                    duration_in_days=job.duration_in_days,
                    min_start_date=_opt_to_date(job.min_start_date),
                    max_end_date=_opt_to_date(job.max_end_date),
                    ideal_end_date=_opt_to_date(job.ideal_end_date),
                    tags=list(job.tags),
                    crew_id=crew_id,
                    start_date=_opt_to_date(schedule.job_start_date(i)),
                    end_date=_opt_to_date(schedule.job_end_date(i)),
                    pinned=job.pinned,
                )
            )
        return cls(
            from_date=epoch_to_date(schedule.from_date),
            to_date=epoch_to_date(schedule.to_date),
            crews=crews,
            jobs=jobs,
            score_hard=schedule.score.hard,
            score_soft=schedule.score.soft,
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


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns (`MaintenanceScheduleModel.from_core`), or a plain
    dict of that shape (the JSON the API returned). Jobs are matched by id. A job left
    out becomes unassigned and the construction heuristic places it, and only it, so
    a complete solution starts the search exactly from it. `problem` is not mutated.
    Pinned jobs keep their crew and start date on `problem`, whatever `solution` says.

    Raises ValueError naming the job for an unknown or repeated id, for a crew_id or
    start_date set without the other, for an unknown crew and for a start_date that is
    not a weekday inside the work calendar. `end_date` is a shadow: it is cleared here
    and the solve fills it in again.
    """
    if not isinstance(problem, MaintenanceScheduleModel):
        raise TypeError(f"problem must be a MaintenanceScheduleModel: {type(problem)!r}")
    jobs = _entities_by_id(_solution_dict(solution), "jobs", [j.id for j in problem.jobs])
    crew_ids = {c.id for c in problem.crews}
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for job in seeded.jobs:
        if not job.pinned:
            entity = jobs.get(job.id, {})
            job.crew_id = entity.get("crew_id")
            try:
                job.start_date = _START_DATE.validate_python(entity.get("start_date"))
            except ValueError as error:
                raise ValueError(f"job {job.id}: invalid start_date {entity.get('start_date')!r}") from error
            job.end_date = None
        if (job.crew_id is None) != (job.start_date is None):
            raise ValueError(f"job {job.id}: crew_id and start_date must be set together")
        if job.crew_id is not None and (not isinstance(job.crew_id, str) or job.crew_id not in crew_ids):
            raise ValueError(f"job {job.id}: unknown crew_id {job.crew_id!r}")
    seeded.to_core()  # rejects a start_date that is not a weekday inside the work calendar
    return seeded
