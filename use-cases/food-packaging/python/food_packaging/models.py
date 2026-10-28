"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Product/Operator/Line/Job/PackagingSchedule 을 1:1 로 미러링하되
시간은 `datetime.datetime`(naive, epoch 분으로 변환), 청소 시간은 Java 처럼
"직전 제품 id -> timedelta" 맵으로 다룬다. 배정은 라인의 `job_ids` 순서가
원본이고, 각 작업의 line_id / start_cleaning_date_time /
start_production_date_time / end_date_time 은 결과 전용 필드다
(`to_core` 에서 무시)::

    from food_packaging.models import PackagingScheduleModel

    model = PackagingScheduleModel.from_core(schedule)  # 엔진 -> pydantic
    clone = PackagingScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                         # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install food_packaging[models]`).
"""

from datetime import date, datetime, timedelta

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
]

_EPOCH = datetime(1970, 1, 1)


def datetime_to_minute(value: datetime) -> int:
    """naive datetime -> epoch 분 (1970-01-01T00:00 기준)."""
    return int((value - _EPOCH).total_seconds()) // 60


def minute_to_datetime(minute: int) -> datetime:
    """epoch 분 -> naive datetime."""
    return _EPOCH + timedelta(minutes=minute)


def _opt_to_datetime(minute: int | None) -> datetime | None:
    return None if minute is None else minute_to_datetime(minute)


class ProductModel(BaseModel):
    """제품. cleaning_durations 는 Java 의 Map<Product, Duration> 미러:
    key 는 라인에서 '직전'에 생산된 제품의 id. 항목이 없는 (직전, 현재)
    쌍이 실제로 연달아 배치되면 Java 처럼 에러가 난다."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str
    cleaning_durations: dict[str, timedelta] = Field(default_factory=dict)


class OperatorModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: str


class JobModel(BaseModel):
    """planning entity (LIST 변수의 원소). 배정은 LineModel.job_ids 가
    원본이고, line_id 와 start_cleaning/start_production/end 는 결과 전용
    shadow 필드다 (to_core 에서 무시). min_start_time 과 priority 는 Java
    와 동일하게 어떤 제약도 읽지 않는다."""

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
    """planning entity. job_ids 가 @PlanningListVariable(순서 유의),
    operator_id 가 Java 의 basic @PlanningVariable Line.operator."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str
    start_date_time: datetime
    operator_id: str | None = None
    job_ids: list[str] = Field(default_factory=list)


class PackagingScheduleModel(BaseModel):
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다.
    어떤 라인의 job_ids 에도 없는 작업은 미배정이다 (pinned 미배정 작업은
    솔버가 그대로 두고, 나머지는 재solve 시 배정될 수 있다 — Java 와
    동일)."""

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
        """pydantic -> 엔진 객체."""
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
        """엔진 객체 -> pydantic (현재 배정, shadow, score 포함)."""
        schedule.update_shadow_variables()
        # pybind 의 vector 필드 접근은 리스트 전체를 복사하므로 한 번만
        # 읽어 둔다.
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
