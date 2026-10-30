"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Product/Operator/Line/Job/PackagingSchedule 1:1, but times are
`datetime.datetime` (naive, converted to epoch minutes) and cleaning times are a
"previous product id -> timedelta" map as in Java. The line's `job_ids` order is the
source of truth for the assignment, and each job's line_id / start_cleaning_date_time /
start_production_date_time / end_date_time are result-only fields
(ignored by `to_core`)::

    from food_packaging.models import PackagingScheduleModel

    model = PackagingScheduleModel.from_core(schedule)  # engine -> pydantic
    clone = PackagingScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                         # pydantic -> engine

`pydantic` is an optional dependency (`pip install food_packaging[models]`).
"""

from datetime import date, datetime, timedelta
from itertools import pairwise

from pydantic import BaseModel, ConfigDict, Field

from . import _core

__all__ = [
    "JobModel",
    "LineModel",
    "OperatorModel",
    "PackagingScheduleModel",
    "ProductModel",
    "datetime_to_minute",
    "minute_to_datetime",
    "with_initial_solution",
]

_EPOCH = datetime(1970, 1, 1)


def datetime_to_minute(value: datetime) -> int:
    """naive datetime -> epoch minutes (from 1970-01-01T00:00)."""
    return int((value - _EPOCH).total_seconds()) // 60


def minute_to_datetime(minute: int) -> datetime:
    """epoch minutes -> naive datetime."""
    return _EPOCH + timedelta(minutes=minute)


def _opt_to_datetime(minute: int | None) -> datetime | None:
    return None if minute is None else minute_to_datetime(minute)


class ProductModel(BaseModel):
    """A product. cleaning_durations mirrors Java's Map<Product, Duration>:
    keys are the id of the product made on the line 'just before'. When a (previous,
    current) pair without an entry actually ends up adjacent, it errors as in Java."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str
    cleaning_durations: dict[str, timedelta] = Field(default_factory=dict)


class OperatorModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: str


class JobModel(BaseModel):
    """Planning entity (an element of the LIST variable). LineModel.job_ids is the source
    of truth for the assignment; line_id and start_cleaning/start_production/end are
    result-only shadow fields (ignored by to_core). min_start_time and priority are read
    by no constraint, the same as in Java."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str
    product_id: str
    duration: timedelta
    min_start_time: datetime
    ideal_end_time: datetime
    max_end_time: datetime
    priority: int = 1
    pinned: bool = False

    line_id: str | None = None
    start_cleaning_date_time: datetime | None = None
    start_production_date_time: datetime | None = None
    end_date_time: datetime | None = None


class LineModel(BaseModel):
    """Planning entity. job_ids is the @PlanningListVariable (order matters),
    operator_id is Java's basic @PlanningVariable Line.operator."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str
    start_date_time: datetime
    operator_id: str | None = None
    job_ids: list[str] = Field(default_factory=list)


class PackagingScheduleModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field.
    Jobs in no line's job_ids are unassigned (the solver leaves pinned unassigned
    jobs alone, and the rest can get assigned on a re-solve — the same as
    Java)."""

    model_config = ConfigDict(extra="forbid")

    from_date: date  # inclusive
    to_date: date  # exclusive
    products: list[ProductModel]
    operators: list[OperatorModel]
    lines: list[LineModel]
    jobs: list[JobModel]
    score_hard: int | None = None
    score_medium: int | None = None
    score_soft: int | None = None

    def to_core(self) -> "_core.PackagingSchedule":
        """pydantic -> engine object."""
        product_index = {p.id: i for i, p in enumerate(self.products)}
        operator_index = {o.id: i for i, o in enumerate(self.operators)}
        job_index = {j.id: i for i, j in enumerate(self.jobs)}
        if len(product_index) != len(self.products):
            raise ValueError("duplicate product id")
        if len(job_index) != len(self.jobs):
            raise ValueError("duplicate job id")

        count = len(self.products)
        cleanup = [-1] * (count * count)  # -1 == missing, like Java's map
        for cur, product in enumerate(self.products):
            for prev_id, duration in product.cleaning_durations.items():
                if prev_id not in product_index:
                    raise ValueError(f"product {product.id}: unknown previous product {prev_id!r}")
                cleanup[product_index[prev_id] * count + cur] = int(duration.total_seconds()) // 60

        lines = []
        for line in self.lines:
            if line.operator_id is not None and line.operator_id not in operator_index:
                raise ValueError(f"line {line.id}: unknown operator_id {line.operator_id!r}")
            for job_id in line.job_ids:
                if job_id not in job_index:
                    raise ValueError(f"line {line.id}: unknown job id {job_id!r}")
            lines.append(
                _core.Line(
                    line.id,
                    line.name,
                    datetime_to_minute(line.start_date_time),
                    operator_index[line.operator_id] if line.operator_id is not None else -1,
                    [job_index[job_id] for job_id in line.job_ids],
                )
            )

        jobs = []
        for job in self.jobs:
            if job.product_id not in product_index:
                raise ValueError(f"job {job.id}: unknown product_id {job.product_id!r}")
            jobs.append(
                _core.Job(
                    job.id,
                    job.name,
                    product_index[job.product_id],
                    int(job.duration.total_seconds()) // 60,
                    datetime_to_minute(job.min_start_time),
                    datetime_to_minute(job.ideal_end_time),
                    datetime_to_minute(job.max_end_time),
                    job.priority,
                    job.pinned,
                )
            )

        return _core.PackagingSchedule(
            datetime_to_minute(datetime(self.from_date.year, self.from_date.month, self.from_date.day)),
            datetime_to_minute(datetime(self.to_date.year, self.to_date.month, self.to_date.day)),
            [_core.Product(p.id, p.name) for p in self.products],
            cleanup,
            [_core.Operator(o.id) for o in self.operators],
            lines,
            jobs,
        )

    @classmethod
    def from_core(cls, schedule: "_core.PackagingSchedule") -> "PackagingScheduleModel":
        """engine object -> pydantic (including the current assignment, shadows and score)."""
        schedule.update_shadow_variables()
        # Accessing a pybind vector field copies the whole list, so read it
        # only once.
        core_products = schedule.products
        core_operators = schedule.operators
        core_lines = schedule.lines
        core_jobs = schedule.jobs

        count = len(core_products)
        products = []
        for cur, product in enumerate(core_products):
            durations = {}
            for prev in range(count):
                minutes = schedule.cleanup_of(prev, cur)
                if minutes >= 0:
                    durations[core_products[prev].id] = timedelta(minutes=minutes)
            products.append(ProductModel(id=product.id, name=product.name, cleaning_durations=durations))

        operators = [OperatorModel(id=o.id) for o in core_operators]
        lines = [
            LineModel(
                id=line.id,
                name=line.name,
                start_date_time=minute_to_datetime(line.start_minute),
                operator_id=(core_operators[line.operator_index].id if line.operator_index >= 0 else None),
                job_ids=[core_jobs[j].id for j in line.jobs],
            )
            for line in core_lines
        ]
        jobs = [
            JobModel(
                id=job.id,
                name=job.name,
                product_id=core_products[job.product_index].id,
                duration=timedelta(minutes=job.duration_minutes),
                min_start_time=minute_to_datetime(job.min_start_minute),
                ideal_end_time=minute_to_datetime(job.ideal_end_minute),
                max_end_time=minute_to_datetime(job.max_end_minute),
                priority=job.priority,
                pinned=job.pinned,
                line_id=(core_lines[job.line_index].id if job.line_index >= 0 else None),
                start_cleaning_date_time=_opt_to_datetime(job.start_cleaning_minute),
                start_production_date_time=_opt_to_datetime(job.start_production_minute),
                end_date_time=_opt_to_datetime(job.end_minute),
            )
            for job in core_jobs
        ]
        return cls(
            from_date=minute_to_datetime(schedule.from_minute).date(),
            to_date=minute_to_datetime(schedule.to_minute).date(),
            products=products,
            operators=operators,
            lines=lines,
            jobs=jobs,
            score_hard=schedule.score.hard,
            score_medium=schedule.score.medium,
            score_soft=schedule.score.soft,
        )


def with_initial_solution(
    problem: PackagingScheduleModel, solution: PackagingScheduleModel | dict
) -> PackagingScheduleModel:
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returned, as the model or as its JSON dict. Each
    line's `job_ids` and `operator_id` become the ones it has there, replacing
    what `problem` had; a line the solution leaves out starts empty, with no
    operator. Lines and jobs are matched by id. Only those two fields are read:
    the jobs' pins, the facts and the shadow fields stay as `problem` has them.

    Jobs in no line are placed by construction, which may also leave them in the
    unassigned pool when that scores best. A solution that assigns every job is
    the exact starting point of the job lists, with two limits on the default
    solve (`fp.solve` without `constraints=`):

    - The operators are not fixed. `improve_operator_assignment` runs after the
      search even with no time to search, and moves a seeded operator whenever
      that strictly cuts the cleaning conflict. The `constraints=` solve never
      moves one.
    - A time limit of 1 ms or less returns before construction, so jobs left out
      stay unassigned. Give it any longer limit to have them placed.

    Pinned jobs stay problem facts: a line in `solution` must start with the
    pinned jobs it has in `problem`, and no other pinned job may appear. Raises
    ValueError otherwise, on an unknown line, job or operator, on a line or job
    given twice, and on a job following another with no cleaning time between
    their products, which would otherwise throw in the middle of a solve.
    """
    solved: dict[str, LineModel] = {}
    for line in PackagingScheduleModel.model_validate(solution).lines:
        if line.id in solved:
            raise ValueError(f"line {line.id} appears twice in the solution")
        solved[line.id] = line
    if unknown := solved.keys() - {line.id for line in problem.lines}:
        raise ValueError(f"unknown line {sorted(unknown)}")
    jobs = {job.id: job for job in problem.jobs}
    products = {product.id: product for product in problem.products}
    operators = {operator.id for operator in problem.operators}
    pinned = {job.id for job in problem.jobs if job.pinned}

    result = problem.model_copy(deep=True)
    seen: set[str] = set()
    for line in result.lines:
        seed = solved.get(line.id)
        job_ids = list(seed.job_ids) if seed else []
        operator_id = seed.operator_id if seed else None
        if operator_id is not None and operator_id not in operators:
            raise ValueError(f"line {line.id}: unknown operator {operator_id!r}")
        for job_id in job_ids:
            if job_id not in jobs:
                raise ValueError(f"line {line.id}: unknown job {job_id!r}")
            if job_id in seen:
                raise ValueError(f"line {line.id}: job {job_id!r} appears twice in the solution")
            seen.add(job_id)
        prefix = [job_id for job_id in line.job_ids if job_id in pinned]
        if job_ids[: len(prefix)] != prefix:
            raise ValueError(f"line {line.id}: the jobs do not start with its pinned jobs {prefix}")
        if moved := pinned.intersection(job_ids[len(prefix) :]):
            raise ValueError(f"line {line.id}: pinned job {sorted(moved)} cannot move here")
        for previous, job_id in pairwise(job_ids):
            before, after = jobs[previous].product_id, jobs[job_id].product_id
            if before not in products[after].cleaning_durations:
                raise ValueError(
                    f"line {line.id}: no cleaning time from {before!r} to {after!r} between {previous!r} and {job_id!r}"
                )
        line.job_ids = job_ids
        line.operator_id = operator_id
    return result
