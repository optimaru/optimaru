"""Type stubs for `task_assigning._core` — the task-assigning C++ solver.

The compiled module wraps the C++ domain in `use-cases/task-assigning/cpp`,
which imports the generic engine in `include/optimaru`.

The planning variable is each `Employee.tasks` (LIST): the ordered list of task
indices the employee performs back to back. A task in no list is unassigned
(the Java original's `@PlanningListVariable(allowsUnassignedValues = true)`).
The shadow values `Task.employee` (-1 == unassigned) / `Task.start_time` (-1 == null)
are re-derived from the lists by `calculate_score` / `Solver.solve`.

Score convention (BendableScore [1]hard/[3]soft, as in the Java quickstart; all penalties):
  hard[0]  noMissingSkills              required skills the employee lacks
  soft[0]  minimizeUnassignedTasks      1 per unassigned task
  soft[1]  minimizeMakespan             endTime^2 per employee (end of the last task)
  soft[2]  criticalPriorityTaskEndTime  endTime x 4
           majorPriorityTaskEndTime     endTime x 2
           minorPriorityTaskEndTime     endTime x 1

Internally the engine uses HardSoftScore(int64 hard, double soft), mapped
  engine.hard = hard * 10**9 + soft0
  engine.soft = soft1 * 10**7 + soft2
as above (level priority holds while |soft0| < 10**9, |soft2| < 10**7, |soft1| < 9x10**8;
see cpp/include/ta/domain.hpp for the exact rules).
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any

class Score:
    """BendableScore [1]hard/[3]soft. hard 0 == feasible (unassigned is soft0).
    Lexicographic comparison (hard > soft0 > soft1 > soft2)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft0(self) -> int: ...
    @property
    def soft1(self) -> int: ...
    @property
    def soft2(self) -> int: ...
    def is_feasible(self) -> bool:
        """True if hard >= 0."""

class Affinity(Enum):
    """Employee-customer affinity. Duration multiplier: HIGH 1 / MEDIUM 2 / LOW 3 / NONE 4."""

    NONE = ...
    LOW = ...
    MEDIUM = ...
    HIGH = ...

class Priority(Enum):
    """endTime penalty weight: CRITICAL 4 / MAJOR 2 / MINOR 1 (soft[2])."""

    MINOR = ...
    MAJOR = ...
    CRITICAL = ...

class Customer:
    """Problem fact: a customer."""
    def __init__(self, id: str, name: str = "") -> None: ...
    id: str
    name: str

class TaskType:
    """Problem fact: a task type. base_duration is in minutes (before the affinity multiplier)."""
    def __init__(
        self,
        code: str,
        title: str = "",
        base_duration: int = 0,
        required_skills: list[str] = ...,
    ) -> None: ...
    code: str
    title: str
    base_duration: int
    required_skills: list[str]

class Task:
    """Planning entity (LIST element): one task. task_type/customer are
    indices into the solution lists."""
    def __init__(
        self,
        id: str,
        task_type: int,
        index_in_task_type: int = 0,
        customer: int = -1,
        min_start_time: int = 0,
        priority: Priority = ...,
    ) -> None: ...
    id: str
    task_type: int
    index_in_task_type: int
    customer: int
    min_start_time: int
    priority: Priority
    @property
    def employee(self) -> int:
        """Shadow: index of the assigned employee (-1 == unassigned)."""
    @property
    def start_time(self) -> int:
        """Shadow: start time (minutes; -1 == null). The first task gets min_start_time,
        later ones max(previous end, min_start_time)."""

class Employee:
    """Planning entity: an employee. `tasks` is the planning list variable (ordered
    task indices). Reading it from the solution returns a copy — bulk-write with
    TaskAssigningSolution.set_task_lists."""
    def __init__(
        self,
        id: str,
        full_name: str = "",
        skills: list[str] = ...,
        affinity_by_customer: list[Affinity] = ...,
        tasks: list[int] = ...,
    ) -> None: ...
    id: str
    full_name: str
    skills: list[str]
    affinity_by_customer: list[Affinity]
    tasks: list[int]

class TaskAssigningSolution:
    """Planning solution. The constructor validates index references and the integrity
    of the task lists (ranges, duplicate assignment)."""
    def __init__(
        self,
        task_types: list[TaskType],
        customers: list[Customer],
        tasks: list[Task],
        employees: list[Employee],
    ) -> None: ...
    @property
    def task_types(self) -> list[TaskType]: ...
    @property
    def customers(self) -> list[Customer]: ...
    @property
    def tasks(self) -> list[Task]: ...
    @property
    def employees(self) -> list[Employee]: ...
    @property
    def score(self) -> Score: ...
    def task_lists(self) -> list[list[int]]:
        """Task index list per employee (snapshot of the planning variable)."""
    def set_task_lists(self, task_lists: list[list[int]]) -> None:
        """Bulk-write the task lists per employee (validates length/range/duplicates)."""

def calculate_score(solution: TaskAssigningSolution) -> Score:
    """Full recompute of the Bendable score (re-derives shadows + stores solution.score)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    full_assert: bool
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    relocate_move_weight: float
    list_swap_move_weight: float
    two_opt_move_weight: float
    or_opt_move_weight: float
    tail_swap_move_weight: float
    best_score_limit: tuple[int, int, int, int]
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, solution: TaskAssigningSolution) -> None:
        """Solves in place: relocates employees[*].tasks and refreshes the shadows and
        solution.score."""

    def solve_with_constraints(self, solution: TaskAssigningSolution, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see task_assigning.constraints). The given list replaces the default set;
        None keeps the default set."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case).
    The same names task_assigning.constraints uses."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """Per-constraint, per-unit **raw** costs — before weight/exponent/level rounding.
    `task_assigning.constraints.score()` takes these and produces a score.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
