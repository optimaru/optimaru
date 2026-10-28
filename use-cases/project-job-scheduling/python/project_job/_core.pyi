"""`project_job._core` 타입 스텁 — project-job-scheduling C++ 솔버.

컴파일된 모듈은 `use-cases/project-job-scheduling/cpp` 의 C++ 도메인 구현을
감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

planning variable 은 `Allocation.mode_delay` 하나다 (BASIC int) — Java
quickstart 의 변수 2개 (executionMode, delay∈[0, delay_range_size)) 의 합성
인코딩::

    -1                                  미초기화 (자원 사용 없음,
                                        날짜는 delay 0/duration 0 통과)
    delay * max_mode_count + mode_slot  실제 mode index = mode_slot %
                                        해당 job 의 mode 수 — 전역 값
                                        범위의 어떤 값도 모든 job 에
                                        유효하다 (Java 의 per-entity
                                        value range 의미론 보존)

Java 의 shadow variable 체인 (predecessorsDoneDate → startDate → endDate)
은 파생 상태다: `calculate_score` / `Solver.solve` 가 위상 정렬 순서로
재계산해 채운다.

점수 규약 (HardMediumSoftScore, Java quickstart 그대로; 전부 벌점):
  hard 1.  nonRenewableResourceCapacity  non-renewable 자원별 전체 사용 합이
                                         용량 초과: -(초과량)
  hard 2.  renewableResourceCapacity     renewable 자원별 x 일자별 사용 합이
                                         용량 초과: -(초과량)
  medium 3. totalProjectDelay            SINK 종료일 - critical path 종료일
                                         (양수만): -(지연 합)
  soft 4.  totalMakespan                 -(최대 SINK 종료일)

엔진 내부는 HardSoftScore(int64 hard, double soft) 이고
`engine.soft = medium * 10**9 + soft` 로 매핑된다 (|soft| < 10**9 에서
레벨 우선순위 보존; 자세한 규칙은 cpp/include/pjs/domain.hpp).
"""

from collections.abc import Callable, Iterable
from enum import Enum
from typing import Any

class Score:
    """HardMediumSoft 점수. hard 0 == feasible. 사전식 비교(hard 우선)."""
    @property
    def hard(self) -> int: ...
    @property
    def medium(self) -> int: ...
    @property
    def soft(self) -> int: ...
    def is_feasible(self) -> bool:
        """hard >= 0 이면 True."""

class JobType(Enum):
    SOURCE = ...
    STANDARD = ...
    SINK = ...

class Project:
    """problem fact: 프로젝트. release_date 는 'Total project delay' 제약의
    기준에만 쓰인다 (시작일을 강제하지 않는다 — Java quickstart 그대로)."""
    def __init__(self, id: str, release_date: int = 0, critical_path_duration: int = 0) -> None: ...
    id: str
    release_date: int
    critical_path_duration: int
    def critical_path_end_date(self) -> int:
        """release_date + critical_path_duration."""

class Resource:
    """problem fact: 자원. renewable 이면 용량이 일자별, 아니면 전체 기간
    기준. `project` 는 ProjectJobSchedule.projects 인덱스, -1 == 글로벌
    (정보용 — 제약은 capacity/renewable 만 본다)."""
    def __init__(self, id: str, capacity: int, renewable: bool = True, project: int = -1) -> None: ...
    id: str
    capacity: int
    renewable: bool
    project: int

class ResourceRequirement:
    """실행 모드의 자원 요구량. `resource` 는 ProjectJobSchedule.resources
    인덱스."""
    def __init__(self, resource: int, requirement: int) -> None: ...
    resource: int
    requirement: int

class ExecutionMode:
    """job 의 실행 방식 하나 (기간 + 자원 요구량)."""
    def __init__(self, id: str, duration: int = 0, resource_requirements: list[ResourceRequirement] = ...) -> None: ...
    id: str
    duration: int
    resource_requirements: list[ResourceRequirement]

class Job:
    """problem fact: 작업. `project` 는 인덱스; 모든 job 은 실행 모드가 1개
    이상 필요하다 (SOURCE/SINK 는 기간 0 모드 1개 — Java quickstart 관례)."""
    def __init__(
        self, id: str, project: int, job_type: JobType = ..., execution_modes: list[ExecutionMode] = ...
    ) -> None: ...
    id: str
    project: int
    job_type: JobType
    execution_modes: list[ExecutionMode]

class Allocation:
    """planning entity: job 하나의 할당 (job 과 1:1). `mode_delay` 가
    planning variable (모듈 docstring 의 인코딩 참고); 솔버가 쓴다.
    `predecessor_allocations` 는 선행 할당 인덱스 (비순환이어야 한다).
    SOURCE/SINK 할당은 항상 pinned; `pinned` 는 STANDARD 도 고정한다."""
    def __init__(
        self,
        id: str,
        job: int,
        predecessor_allocations: list[int] = ...,
        mode_delay: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: str
    job: int
    predecessor_allocations: list[int]
    mode_delay: int
    pinned: bool
    @property
    def predecessors_done_date(self) -> int: ...
    @property
    def start_date(self) -> int: ...
    @property
    def end_date(self) -> int: ...

class ProjectJobSchedule:
    """planning solution. 생성자가 인스턴스를 검증한다 (인덱스 범위, 모드
    없는 job, 선행 그래프 사이클 -> ValueError) . solve 는 allocations 의
    `mode_delay` 와 파생 날짜, `score` 를 in place 로 바꾼다.

    주의: `allocations` 등 리스트 속성 접근은 복사본을 돌려주므로, 배정을
    바꾸려면 `set_assignment` 를 써야 한다."""
    def __init__(
        self,
        projects: list[Project],
        resources: list[Resource],
        jobs: list[Job],
        allocations: list[Allocation],
        delay_range_size: int = 500,
    ) -> None: ...
    @property
    def projects(self) -> list[Project]: ...
    @property
    def resources(self) -> list[Resource]: ...
    @property
    def jobs(self) -> list[Job]: ...
    @property
    def allocations(self) -> list[Allocation]: ...
    @property
    def delay_range_size(self) -> int: ...
    @property
    def score(self) -> Score: ...
    @property
    def max_mode_count(self) -> int: ...
    def allocation_count(self) -> int: ...
    def value_count(self) -> int:
        """delay_range_size * max_mode_count (BASIC 값 범위 크기)."""
    def encode_value(self, delay: int, mode_slot: int) -> int:
        """delay * max_mode_count + mode_slot. job 의 mode index 는 항상
        유효한 slot 이다."""
    def delay_of_value(self, value: int) -> int: ...
    def mode_slot_of_value(self, value: int) -> int: ...
    def execution_mode_index_of_value(self, allocation: int, value: int) -> int:
        """slot % 해당 job 의 mode 수 (합성 값 -> 실제 mode index)."""
    def assignment(self) -> list[int]:
        """allocation 별 현재 mode_delay 값 (해)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """planning variable 일괄 쓰기 (`allocations` 접근은 복사본이라
        개별 쓰기는 반영되지 않는다)."""

def calculate_score(schedule: ProjectJobSchedule) -> Score:
    """HardMediumSoft 점수의 full recompute; 파생 날짜
    (predecessors_done_date/start_date/end_date)도 갱신한다 (schedule.score
    에도 저장)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = 자동: clamp(8 * allocation 수, 400, 3200)."""
    full_assert: bool
    """FULL_ASSERT: 매 무브마다 인크리멘털 점수를 full recompute 와 대조
    (디버깅용; 매우 느림)."""
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: list[float]
    ruin_recreate_polish_steps: int
    change_move_weight: float
    swap_move_weight: float
    best_score_limit: tuple[int, int, int]
    """(hard, medium, soft) 이상에 도달하면 조기 종료. 예: (0, -(2**62),
    -(2**62)) 는 첫 feasible 해에서 멈춘다."""
    on_new_best: Callable[[int, Score], None] | None
    """새 best 해마다 (step, score) 로 호출."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: ProjectJobSchedule) -> None:
        """in place 로 푼다: allocations[*].mode_delay, 파생 날짜와
        schedule.score 를 채운다. pinned 할당(모든 SOURCE/SINK + 사용자
        지정 STANDARD)은 값이 유지되고, 미초기화 pinned 할당은 (첫 mode,
        delay 0) 으로 먼저 고정된다."""

    def solve_with_constraints(self, schedule: ProjectJobSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (project_job.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    project_job.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `project_job.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
