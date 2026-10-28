"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 TaskType/Customer/Task/Employee/TaskAssigningSolution 을 1:1 로
미러링하되, Java quickstart 의 REST 표현처럼 참조는 id/code 문자열
(`task_type_code`/`customer_id`, 직원의 `tasks` 는 task id 리스트)로 둔다.
엔진 변환(`to_core`)이 참조를 인덱스로 옮긴다::

    from task_assigning.models import TaskAssigningSolutionModel

    model = TaskAssigningSolutionModel.model_validate_json(payload)
    solution = model.to_core()                          # pydantic -> 엔진
    ...solve...
    solved = TaskAssigningSolutionModel.from_core(solution)  # 엔진 -> pydantic

배정 결과는 Java 의 JSON 표현처럼 직원 쪽에 실린다: `employees[i].tasks` 가
그 직원이 수행할 task id 의 순서 있는 리스트다. task 쪽에는 계산된
`start_time` (미배정이면 None) 이 실린다.
`pydantic` 은 선택 의존성이다 (`pip install task_assigning[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = [
    "AFFINITIES",
    "PRIORITIES",
    "CustomerModel",
    "EmployeeModel",
    "TaskAssigningSolutionModel",
    "TaskModel",
    "TaskTypeModel",
]

AFFINITIES = ("NONE", "LOW", "MEDIUM", "HIGH")
PRIORITIES = ("MINOR", "MAJOR", "CRITICAL")


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class TaskTypeModel(_Model):
    """태스크 종류. base_duration 은 분 단위 기본 소요 시간 (affinity 배수
    적용 전)."""

    code: str
    title: str = ""
    base_duration: int = Field(ge=0)
    required_skills: list[str] = Field(default_factory=list)


class CustomerModel(_Model):
    """고객 (problem fact)."""

    id: str
    name: str = ""


class TaskModel(_Model):
    """planning entity: 수행할 태스크 한 건. 어느 직원이 하는지는
    `EmployeeModel.tasks` (순서 있는 task id 리스트)가 결정한다.
    `start_time` 은 계산된 shadow 값 (분; 미배정이면 None)이다."""

    id: str
    task_type_code: str
    index_in_task_type: int = 1
    customer_id: str
    min_start_time: int = Field(default=0, ge=0)
    priority: str = "MINOR"
    start_time: int | None = None

    @model_validator(mode="after")
    def _valid_priority(self):
        if self.priority not in PRIORITIES:
            raise ValueError(f"priority 는 {PRIORITIES} 중 하나여야 한다: {self.priority!r}")
        return self


class EmployeeModel(_Model):
    """planning entity: 직원. `tasks` 가 planning list variable — 이 직원이
    연달아 수행할 task id 의 순서 있는 리스트다. `customer_to_affinity` 에
    없는 고객은 NONE (배수 4) 취급."""

    id: str
    full_name: str = ""
    skills: list[str] = Field(default_factory=list)
    customer_to_affinity: dict[str, str] = Field(default_factory=dict)
    tasks: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _valid_affinities(self):
        for customer_id, affinity in self.customer_to_affinity.items():
            if affinity not in AFFINITIES:
                raise ValueError(f"customer {customer_id!r} 의 affinity 는 {AFFINITIES} 중 하나여야 한다: {affinity!r}")
        return self


class TaskAssigningSolutionModel(_Model):
    """planning solution 미러. score_* 는 읽기 전용 결과라 별도 필드로 둔다
    (BendableScore [1]hard/[3]soft 의 네 레벨)."""

    task_types: list[TaskTypeModel]
    customers: list[CustomerModel]
    tasks: list[TaskModel]
    employees: list[EmployeeModel]
    score_hard: int | None = None
    score_soft0: int | None = None
    score_soft1: int | None = None
    score_soft2: int | None = None

    @model_validator(mode="after")
    def _valid_references(self):
        type_codes = {t.code for t in self.task_types}
        customer_ids = {c.id for c in self.customers}
        task_ids = {t.id for t in self.tasks}
        if len(type_codes) != len(self.task_types):
            raise ValueError("task type code 가 중복된다")
        if len(customer_ids) != len(self.customers):
            raise ValueError("customer id 가 중복된다")
        if len(task_ids) != len(self.tasks):
            raise ValueError("task id 가 중복된다")
        if len({e.id for e in self.employees}) != len(self.employees):
            raise ValueError("employee id 가 중복된다")
        for task in self.tasks:
            if task.task_type_code not in type_codes:
                raise ValueError(f"task {task.id!r} 의 task_type_code {task.task_type_code!r} 가 없다")
            if task.customer_id not in customer_ids:
                raise ValueError(f"task {task.id!r} 의 customer_id {task.customer_id!r} 가 없다")
        assigned: set[str] = set()
        for employee in self.employees:
            for customer_id in employee.customer_to_affinity:
                if customer_id not in customer_ids:
                    raise ValueError(f"employee {employee.id!r} 의 affinity 고객 {customer_id!r} 가 없다")
            for task_id in employee.tasks:
                if task_id not in task_ids:
                    raise ValueError(f"employee {employee.id!r} 의 task {task_id!r} 가 없다")
                if task_id in assigned:
                    raise ValueError(f"task {task_id!r} 가 두 직원 이상에게 배정되어 있다")
                assigned.add(task_id)
        return self

    def to_core(self) -> "_core.TaskAssigningSolution":
        """pydantic -> 엔진 객체 (code/id 참조를 인덱스로; score 필드는
        엔진이 다시 계산하므로 무시). 직원 리스트에 없는 task 는 미배정으로
        남는다 — 솔버가 배정을 결정한다."""
        type_index = {t.code: i for i, t in enumerate(self.task_types)}
        customer_index = {c.id: i for i, c in enumerate(self.customers)}
        task_index = {t.id: i for i, t in enumerate(self.tasks)}
        return _core.TaskAssigningSolution(
            [_core.TaskType(t.code, t.title, t.base_duration, list(t.required_skills)) for t in self.task_types],
            [_core.Customer(c.id, c.name) for c in self.customers],
            [
                _core.Task(
                    t.id,
                    type_index[t.task_type_code],
                    t.index_in_task_type,
                    customer_index[t.customer_id],
                    t.min_start_time,
                    getattr(_core.Priority, t.priority),
                )
                for t in self.tasks
            ],
            [
                _core.Employee(
                    e.id,
                    e.full_name,
                    list(e.skills),
                    [getattr(_core.Affinity, e.customer_to_affinity.get(c.id, "NONE")) for c in self.customers],
                    [task_index[task_id] for task_id in e.tasks],
                )
                for e in self.employees
            ],
        )

    @classmethod
    def from_core(cls, solution: "_core.TaskAssigningSolution") -> "TaskAssigningSolutionModel":
        """엔진 객체 -> pydantic (현재 배정과 score 포함). affinity 는 NONE
        항목을 생략해 Java 의 customerToAffinity 맵처럼 담는다."""
        return cls(
            task_types=[
                TaskTypeModel(
                    code=t.code,
                    title=t.title,
                    base_duration=t.base_duration,
                    required_skills=list(t.required_skills),
                )
                for t in solution.task_types
            ],
            customers=[CustomerModel(id=c.id, name=c.name) for c in solution.customers],
            tasks=[
                TaskModel(
                    id=t.id,
                    task_type_code=solution.task_types[t.task_type].code,
                    index_in_task_type=t.index_in_task_type,
                    customer_id=solution.customers[t.customer].id,
                    min_start_time=t.min_start_time,
                    priority=t.priority.name,
                    start_time=None if t.start_time < 0 else t.start_time,
                )
                for t in solution.tasks
            ],
            employees=[
                EmployeeModel(
                    id=e.id,
                    full_name=e.full_name,
                    skills=list(e.skills),
                    customer_to_affinity={
                        solution.customers[c].id: affinity.name
                        for c, affinity in enumerate(e.affinity_by_customer)
                        if affinity != _core.Affinity.NONE
                    },
                    tasks=[solution.tasks[i].id for i in e.tasks],
                )
                for e in solution.employees
            ],
            score_hard=solution.score.hard,
            score_soft0=solution.score.soft0,
            score_soft1=solution.score.soft1,
            score_soft2=solution.score.soft2,
        )
