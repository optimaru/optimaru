"""`maintenance_scheduling._core` 타입 스텁 — maintenance-scheduling C++ 솔버.

컴파일된 모듈은 `use-cases/maintenance-scheduling/cpp` 의 C++ 도메인 구현을
감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

Java quickstart 의 planning variable 2개(crew, startDate)는 entity(Job)당
BASIC int 변수 1개로 합성 인코딩된다::

    crew_date = start_date_index * crew_count + crew_index   # -1 == 미배정

start_date_index 는 달력 [from_date, to_date) 의 주중(월~금) 목록
(`MaintenanceSchedule.start_dates()`) 의 인덱스다. 날짜는 전부 epoch day
(1970-01-01 기준 일수, Java LocalDate.toEpochDay)이고, endDate shadow 는
주말을 건너뛰며 duration 만큼 진행한 exclusive 종료일이다
(`plus_business_days`).

점수 규약: `hard` 는 제약 위반 일수 합(0 == feasible), `soft` 는 최적화
목적식. 둘 다 최대화한다 (덜 음수일수록 좋다).

제약 (HardSoftScore; 일수는 Java ChronoUnit.DAYS 와 같은 달력 일수):
  hard 1. crewConflict        같은 crew 의 겹치는 작업쌍마다 -겹친 일수
  hard 2. minStartDate        minStartDate 보다 일찍 시작한 일수만큼 -1
  hard 3. maxEndDate          maxEndDate 보다 늦게 끝난 일수만큼 -1
  soft 4. beforeIdealEndDate  idealEndDate 보다 일찍 끝난 일수만큼 -1
  soft 5. afterIdealEndDate   idealEndDate 보다 늦게 끝난 일수만큼 -1'000'000
  soft 6. tagConflict         겹치는 작업쌍의 공유 태그 x 겹친 일수만큼
                              -1'000
"""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    """HardSoft 점수. `hard` 는 정수 위반 합(0 == feasible); `soft` 는
    목적식. 사전식 비교(hard 우선)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """hard == 0 이면 True."""

class Crew:
    """problem fact: 작업 조. 같은 날 최대 한 작업만 수행할 수 있다."""
    def __init__(self, id: str, name: str) -> None: ...
    id: str
    name: str

class Job:
    """planning entity: 배정할 유지보수 작업. `crew_date` 가 planning
    variable (-1 == 미배정); 솔버가 쓴다. 날짜는 epoch day."""
    def __init__(
        self,
        id: str,
        name: str,
        duration_in_days: int,
        min_start_date: int | None = None,
        max_end_date: int | None = None,
        ideal_end_date: int | None = None,
        tags: list[str] = ...,
    ) -> None: ...
    id: str
    name: str
    duration_in_days: int
    """작업일 수 (주말 제외 business days)."""
    min_start_date: int | None
    """inclusive; None = 제한 없음."""
    max_end_date: int | None
    """exclusive; None = 제한 없음."""
    ideal_end_date: int | None
    """exclusive; None = 이상 종료일 없음."""
    tags: list[str]
    crew_date: int
    """start_date_index * crew_count + crew_index; -1 == 미배정."""
    pinned: bool
    """True 면 crew_date 를 고정 (점수엔 반영되지만 절대 움직이지 않는다).
    quickstart 에는 없는 엔진 확장."""

class MaintenanceSchedule:
    """planning solution: 달력 [from_date, to_date) x crews 그리드(값 범위)
    + jobs(entity). solve 는 jobs 의 `crew_date` 와 `score` 를 in place 로
    바꾼다.

    주의: `jobs` 등 리스트 속성 접근은 복사본을 돌려주므로, 배정을 바꾸려면
    `set_assignment` 를 쓰거나 리스트 전체를 다시 대입해야 한다."""
    def __init__(
        self,
        from_date: int,
        to_date: int,
        crews: list[Crew],
        jobs: list[Job],
    ) -> None: ...
    from_date: int
    to_date: int
    crews: list[Crew]
    jobs: list[Job]
    @property
    def score(self) -> Score: ...
    def crew_count(self) -> int: ...
    def date_count(self) -> int:
        """시작일 값 범위 크기 (달력의 주중 일수)."""
    def crew_date_count(self) -> int: ...
    def crew_date_of(self, date_index: int, crew_index: int) -> int: ...
    def date_index_of(self, crew_date: int) -> int: ...
    def crew_index_of(self, crew_date: int) -> int: ...
    def start_dates(self) -> list[int]:
        """시작일 값 범위: [from_date, to_date) 의 주중 epoch day 목록
        (Java createStartDateList())."""
    def job_start_date(self, job_index: int) -> int | None:
        """배정된 시작 epoch day; 미배정이면 None."""
    def job_end_date(self, job_index: int) -> int | None:
        """endDate shadow (exclusive; 주말 건너뜀); 미배정이면 None."""
    def assignment(self) -> list[int]: ...
    def set_assignment(self, assignment: list[int]) -> None: ...

def calculate_score(schedule: MaintenanceSchedule) -> Score:
    """전체 재계산 (schedule.score 에도 저장)."""

def plus_business_days(start_date: int, business_days: int) -> int:
    """Java Job.calculateEndDate: 주말을 건너뛰며 business_days 만큼 진행한
    exclusive 종료 epoch day."""

def generate_demo_data(size: str = "SMALL", from_date: int | None = None) -> MaintenanceSchedule:
    """Java DemoDataGenerator 인스턴스 (java.util.Random(17) 비트 동일
    재현). SMALL: crew 3/8주/작업 14, LARGE: crew 5/16주/작업 48.
    from_date 기본값은 다음 월요일 (Java 데모와 동일); 재현 가능한 테스트는
    고정 월요일 epoch day 를 넘겨라."""

class SolverConfig:
    """엔진 설정 (기본: 30초 예산, late acceptance + ALNS)."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = auto: clamp(8 * jobs, 400, 3200)."""
    full_assert: bool
    """True 면 매 무브마다 incremental == full 재검증 (디버깅용; 매우
    느림)."""
    ruin_recreate: bool
    ruin_recreate_unimproved_step_trigger: int
    ruin_recreate_min_ruin_fraction: float
    ruin_recreate_max_ruin_fraction: float
    ruin_recreate_max_ruin_count: int
    ruin_recreate_operator_decay: float
    ruin_recreate_rewards: Iterable[float]
    ruin_recreate_polish_steps: int
    change_move_weight: float
    swap_move_weight: float
    window_value_probability: float
    """change 무브가 작업 고유의 [minStart, idealEnd] 시작일 구간에서
    날짜를 뽑을 확률. 0 이면 (날짜 x crew) 전체에서 균등 추출."""
    hot_job_probability: float
    """무브의 시드를 현재 tagConflict 에 걸린 작업에서 고를 확률."""
    free_crew_candidates: int
    """guided change 무브가 작업 기간 내내 비어 있는 crew 를 찾기 위해
    시도하는 무작위 crew 수. 0 이면 균등 추출."""
    best_score_limit: tuple[int, float]
    """bestScore >= (hard, soft) 도달 시 조기 종료. (0, -1e18) 이면 첫
    feasible 에서 멈춘다."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: MaintenanceSchedule) -> None:
        """in place 로 푼다: jobs[*].crew_date 와 schedule.score 를 쓴다.
        pinned 작업은 배정을 유지한다."""

    def solve_with_constraints(self, schedule: MaintenanceSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (maintenance_scheduling.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    maintenance_scheduling.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `maintenance_scheduling.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
