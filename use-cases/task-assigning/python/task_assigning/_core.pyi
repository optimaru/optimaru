"""Type stubs for `task_assigning._core` — the task-assigning C++ solver.

The compiled module wraps the C++ domain in `use-cases/task-assigning/cpp`,
which imports the generic engine in `include/optimaru`.

The planning variable is each `Employee.tasks` (LIST): the ordered list of task
indices the employee performs back to back. A task in no list is unassigned
(the Java original's `@PlanningListVariable(allowsUnassignedValues = true)`).
The shadow values `Task.employee` (-1 == unassigned) / `Task.start_time` (-1 == null)
are re-derived from the lists by `calculate_score` / `Solver.solve`.

Score convention (BendableScore [1]hard/[3]soft, as in the Java quickstart; all penalties),
at the default weights (constraint_info()):
  hard[0]  required_skill_cost                   required skills the employee lacks (hard, 1)
  soft[0]  unassigned_tasks_cost                 1 per unassigned task (medium, 1)
  soft[1]  squared_makespan_cost                 endTime^2 per employee, end of the last task (soft, 10**7)
  soft[2]  critical_priority_task_end_time_cost  endTime x 4 (soft, 4)
           major_priority_task_end_time_cost     endTime x 2 (soft, 2)
           minor_priority_task_end_time_cost     endTime x 1 (soft, 1)

Internally the engine uses HardSoftScore(int64 hard, double soft). The medium level (soft0)
folds below hard and the soft level packs soft1 above soft2:
  engine.hard = hard * 10**9 + soft0     (medium_fold {"kind": "below_hard", "unit": 1e9})
  engine.soft = soft1 * 10**7 + soft2    (squared_makespan_cost's default weight 10**7)
(level priority holds while |soft0| < 10**9, |soft2| < 10**7, |soft1| < 9x10**8;
see cpp/include/ta/domain.hpp for the exact rules).
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any, Literal, NamedTuple

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
    """Full recompute of the Bendable score at the default weights (re-derives shadows +
    stores solution.score)."""

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
    """(hard, soft0, soft1, soft2): stop early once bestScore >= this. -(2**63)
    on soft1 or soft2 means no floor there (only hard and soft0 are compared),
    so (0, 0, -(2**63), -(2**63)) stops at the first solution with every skill
    met and every task assigned."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, solution: TaskAssigningSolution) -> None:
        """Solves in place: relocates employees[*].tasks and refreshes the shadows and
        solution.score."""

    def solve_with_constraints(self, solution: TaskAssigningSolution, constraints: Iterable[Any] | None = ...) -> None:
        """Score the same solve with a caller-chosen constraint set
        (see task_assigning.constraints). The given list replaces the default set;
        None keeps the default set. A name may be picked several times: each pick is its
        own instance with its own knobs, its level included, and the instances add up.
        A pick moved onto medium lands on soft0 (below hard)."""

def list_constraints() -> list[str]:
    """Constraint names scored by this domain (default assembly order, snake_case; the unit
    constraints first, the global unassigned_tasks_cost last): required_skill_cost,
    squared_makespan_cost, critical_priority_task_end_time_cost,
    major_priority_task_end_time_cost, minor_priority_task_end_time_cost,
    unassigned_tasks_cost. The same names task_assigning.constraints uses."""

def constraint_costs(
    solution: TaskAssigningSolution, constraints: Iterable[Any] | None = ...
) -> dict[str, dict[str, Any]]:
    """Raw per-employee cost of every constraint (one total for the global
    unassigned_tasks_cost), before weight/exponent/offset/level:
    {key: {"costs": [...], "weight": float, "exponent": float, "offset": float,
    "clip": bool, "hard": bool, "global": bool, "level": "hard" | "medium" | "soft",
    "reward": bool, "level_flippable": bool, "name": str, "label": str,
    "default_weight": float, "default_level": str,
    "medium_fold": {"kind": "below_hard", "unit": 1e9}}}. Every employee is a unit the
    per-employee constraints evaluate, an idle one included (a literal offset,
    clip=False, charges it). None = the default constraints; a pick list (as for solve)
    scores exactly those picks, each entry carrying its pick's knobs, the k-th pick of a
    name keyed "<name>#<k>" from the second on. The medium fold is on EVERY entry: it
    belongs to the domain. `task_assigning.constraints.score()` turns it into the
    engine score."""

class ConstraintInfo(NamedTuple):
    """One constraint-catalog row, read off the C++ default assembly."""

    name: str
    default_level: Literal["hard", "medium", "soft"]
    default_weight: float
    level_flippable: bool
    reward: bool
    default_enabled: bool

def constraint_info() -> list[ConstraintInfo]:
    """The constraint catalog, in list_constraints() order:
    required_skill_cost (hard, 1), squared_makespan_cost (soft, 1e7),
    critical_priority_task_end_time_cost (soft, 4), major_priority_task_end_time_cost
    (soft, 2), minor_priority_task_end_time_cost (soft, 1), unassigned_tasks_cost
    (medium, 1); all flippable, none a reward, all scored by default (default_enabled)."""
