"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Crew/Job/MaintenanceSchedule 을 1:1 로 미러링하되 날짜는
`datetime.date` 로 다룬다 (엔진 쪽은 epoch day 정수). 배정은 crew_date
합성값 대신 사람이 읽는 (crew_id, start_date) 쌍으로 저장하고, end_date
(주말 제외 shadow)는 결과 전용 필드다::

    from maintenance_scheduling.models import MaintenanceScheduleModel

    model = MaintenanceScheduleModel.from_core(schedule)  # 엔진 -> pydantic
    clone = MaintenanceScheduleModel.model_validate_json(model.model_dump_json())
    schedule2 = clone.to_core()                           # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install maintenance_scheduling[models]`).
"""

from datetime import date, timedelta

from pydantic import BaseModel, ConfigDict, Field

from . import _core

__all__ = [
    "CrewModel",
    "JobModel",
    "MaintenanceScheduleModel",
    "date_to_epoch",
    "epoch_to_date",
]

_EPOCH = date(1970, 1, 1)


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
    """작업 조. 한 crew 는 같은 날 최대 하나의 작업만 수행할 수 있다."""

    model_config = ConfigDict(extra="forbid")

    id: str
    name: str


class JobModel(BaseModel):
    """planning entity. crew_id/start_date 가 배정(둘 다 채워져야 배정),
    end_date 는 주말 제외 shadow(결과 전용, to_core 에서 무시)."""

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
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

    model_config = ConfigDict(extra="forbid")

    from_date: date  # inclusive
    to_date: date  # exclusive
    crews: list[CrewModel]
    jobs: list[JobModel]
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.MaintenanceSchedule":
        """pydantic -> 엔진 객체. crew_id/start_date 배정은 crew_date 합성값
        으로 인코딩한다 (start_date 는 달력 범위 안의 주중이어야 한다)."""
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
        """엔진 객체 -> pydantic (현재 배정, shadow end_date, score 포함)."""
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
