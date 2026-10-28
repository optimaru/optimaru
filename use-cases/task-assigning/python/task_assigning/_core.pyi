"""`task_assigning._core` 타입 스텁 — task-assigning C++ 솔버.

컴파일된 모듈은 `use-cases/task-assigning/cpp` 의 C++ 도메인 구현을 감싼다;
그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

planning variable 은 각 `Employee.tasks` (LIST) 다: 그 직원이 연달아 수행할
task 인덱스의 순서 있는 리스트. 어느 리스트에도 없는 task 는 미배정이다
(Java 원본의 `@PlanningListVariable(allowsUnassignedValues = true)`).
shadow 값 `Task.employee` (-1 == 미배정) / `Task.start_time` (-1 == null) 은
`calculate_score` / `Solver.solve` 가 리스트에서 다시 유도한다.

점수 규약 (BendableScore [1]hard/[3]soft, Java quickstart 그대로; 전부 벌점):
  hard[0]  noMissingSkills              직원에게 없는 필수 스킬 수
  soft[0]  minimizeUnassignedTasks      미배정 task 1건당 1
  soft[1]  minimizeMakespan             직원별 endTime^2 (마지막 task 의 end)
  soft[2]  criticalPriorityTaskEndTime  endTime x 4
           majorPriorityTaskEndTime     endTime x 2
           minorPriorityTaskEndTime     endTime x 1

엔진 내부는 HardSoftScore(int64 hard, double soft) 이고
  engine.hard = hard * 10**9 + soft0
  engine.soft = soft1 * 10**7 + soft2
로 매핑된다 (|soft0| < 10**9, |soft2| < 10**7, |soft1| < 9x10**8 에서 레벨
우선순위가 보존; 자세한 규칙은 cpp/include/ta/domain.hpp).
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any

class Score:
    """BendableScore [1]hard/[3]soft. hard 0 == feasible (미배정은 soft0).
    사전식 비교(hard > soft0 > soft1 > soft2)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft0(self) -> int: ...
    @property
    def soft1(self) -> int: ...
    @property
    def soft2(self) -> int: ...
    def is_feasible(self) -> bool:
        """hard >= 0 이면 True."""

class Affinity(Enum):
    """직원-고객 친밀도. 소요 시간 배수: HIGH 1 / MEDIUM 2 / LOW 3 / NONE 4."""

    NONE = ...
    LOW = ...
    MEDIUM = ...
    HIGH = ...

class Priority(Enum):
    """endTime 벌점 가중치: CRITICAL 4 / MAJOR 2 / MINOR 1 (soft[2])."""

    MINOR = ...
    MAJOR = ...
    CRITICAL = ...

class Customer:
    """problem fact: 고객."""
    def __init__(self, id: str, name: str = "") -> None: ...
    id: str
    name: str

class TaskType:
    """problem fact: 태스크 종류. base_duration 은 분 (affinity 배수 전)."""
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
    """planning entity (LIST 원소): 태스크 한 건. task_type/customer 는
    solution 리스트 인덱스."""
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
        """shadow: 담당 직원 인덱스 (-1 == 미배정)."""
    @property
    def start_time(self) -> int:
        """shadow: 시작 시각(분; -1 == null). 첫 task 는 min_start_time,
        이후는 max(이전 end, min_start_time)."""

class Employee:
    """planning entity: 직원. `tasks` 가 planning list variable (순서 있는
    task 인덱스). solution 에서 읽으면 복사본이다 — 일괄 쓰기는
    TaskAssigningSolution.set_task_lists 로."""
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
    """planning solution. 생성자가 인덱스 참조와 task 리스트의 무결성(범위,
    중복 배정)을 검증한다."""
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
        """직원별 task 인덱스 리스트 (planning variable 스냅숏)."""
    def set_task_lists(self, task_lists: list[list[int]]) -> None:
        """직원별 task 리스트 일괄 쓰기 (길이/범위/중복 검증)."""

def calculate_score(solution: TaskAssigningSolution) -> Score:
    """Bendable 점수 full recompute (shadow 재유도 + solution.score 저장)."""

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
        """in place 로 푼다: employees[*].tasks 를 재배치하고 shadow 와
        solution.score 를 갱신한다."""

    def solve_with_constraints(self, solution: TaskAssigningSolution, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (task_assigning.constraints 참고). 준 목록이 기본 세트를 대체하고,
        None 이면 기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    task_assigning.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `task_assigning.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
