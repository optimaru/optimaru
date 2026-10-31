"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s TaskType/Customer/Task/Employee/TaskAssigningSolution 1:1, but like
the REST shape of the Java quickstart, references are id/code strings
(`task_type_code`/`customer_id`; an employee's `tasks` is a list of task ids).
The engine conversion (`to_core`) turns references into indices::

    from task_assigning.models import TaskAssigningSolutionModel

    model = TaskAssigningSolutionModel.model_validate_json(payload)
    solution = model.to_core()                          # pydantic -> engine
    ...solve...
    solved = TaskAssigningSolutionModel.from_core(solution)  # engine -> pydantic

As in Java's JSON shape, the assignment lives on the employee side: `employees[i].tasks` is
the ordered list of task ids that employee performs. The task side carries the computed
`start_time` (None when unassigned).
`pydantic` is an optional dependency (`pip install task_assigning[models]`).
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
    "with_initial_solution",
]

AFFINITIES = ("NONE", "LOW", "MEDIUM", "HIGH")
PRIORITIES = ("MINOR", "MAJOR", "CRITICAL")


class _Model(BaseModel):
    model_config = ConfigDict(extra="forbid")


class TaskTypeModel(_Model):
    """A task type. base_duration is the base duration in minutes (before the affinity
    multiplier)."""

    code: str
    title: str = ""
    base_duration: int = Field(ge=0)
    required_skills: list[str] = Field(default_factory=list)


class CustomerModel(_Model):
    """A customer (problem fact)."""

    id: str
    name: str = ""


class TaskModel(_Model):
    """Planning entity: one task to perform. Which employee does it is decided by
    `EmployeeModel.tasks` (an ordered list of task ids).
    `start_time` is the computed shadow value (minutes; None when unassigned)."""

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
            raise ValueError(f"priority must be one of {PRIORITIES}: {self.priority!r}")
        return self


class EmployeeModel(_Model):
    """Planning entity: an employee. `tasks` is the planning list variable — the ordered
    list of task ids this employee performs back to back. Customers missing from
    `customer_to_affinity` count as NONE (multiplier 4)."""

    id: str
    full_name: str = ""
    skills: list[str] = Field(default_factory=list)
    customer_to_affinity: dict[str, str] = Field(default_factory=dict)
    tasks: list[str] = Field(default_factory=list)

    @model_validator(mode="after")
    def _valid_affinities(self):
        for customer_id, affinity in self.customer_to_affinity.items():
            if affinity not in AFFINITIES:
                raise ValueError(f"affinity of customer {customer_id!r} must be one of {AFFINITIES}: {affinity!r}")
        return self


class TaskAssigningSolutionModel(_Model):
    """Planning solution mirror. score_* are read-only results, kept as separate fields
    (the four levels of BendableScore [1]hard/[3]soft)."""

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
            raise ValueError("duplicate task type code")
        if len(customer_ids) != len(self.customers):
            raise ValueError("duplicate customer id")
        if len(task_ids) != len(self.tasks):
            raise ValueError("duplicate task id")
        if len({e.id for e in self.employees}) != len(self.employees):
            raise ValueError("duplicate employee id")
        for task in self.tasks:
            if task.task_type_code not in type_codes:
                raise ValueError(f"task {task.id!r} has unknown task_type_code {task.task_type_code!r}")
            if task.customer_id not in customer_ids:
                raise ValueError(f"task {task.id!r} has unknown customer_id {task.customer_id!r}")
        assigned: set[str] = set()
        for employee in self.employees:
            for customer_id in employee.customer_to_affinity:
                if customer_id not in customer_ids:
                    raise ValueError(f"employee {employee.id!r} has an affinity for unknown customer {customer_id!r}")
            for task_id in employee.tasks:
                if task_id not in task_ids:
                    raise ValueError(f"employee {employee.id!r} has unknown task {task_id!r}")
                if task_id in assigned:
                    raise ValueError(f"task {task_id!r} is assigned to more than one employee")
                assigned.add(task_id)
        return self

    def to_core(self) -> "_core.TaskAssigningSolution":
        """pydantic -> engine object (code/id references to indices; the score field is
        ignored since the engine recomputes it). Tasks in no employee list stay
        unassigned — the solver decides the assignment."""
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
        """engine object -> pydantic (including the current assignment and score). Affinity
        omits NONE entries, like Java's customerToAffinity map."""
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


def with_initial_solution(
    problem: TaskAssigningSolutionModel, solution: TaskAssigningSolutionModel | dict
) -> TaskAssigningSolutionModel:
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returned, as the model or as its JSON dict. Each
    employee's `tasks` become that employee's tasks there, replacing what
    `problem` had; an employee the solution leaves out starts empty. Employees
    and tasks are matched by id.

    Tasks in no list are placed by construction. A solution that assigns every
    task is the exact starting point. Raises ValueError on an unknown employee
    or task, or a task assigned twice.
    """
    solved = {employee.id: employee for employee in TaskAssigningSolutionModel.model_validate(solution).employees}
    if unknown := solved.keys() - {employee.id for employee in problem.employees}:
        raise ValueError(f"unknown employee {sorted(unknown)}")
    data = problem.model_dump()
    for employee in data["employees"]:
        employee["tasks"] = list(solved[employee["id"]].tasks) if employee["id"] in solved else []
    return TaskAssigningSolutionModel.model_validate(data)
