"""`tournament_scheduling._core` 타입 스텁 — tournament-scheduling C++ 솔버.

컴파일된 모듈은 `use-cases/tournament-scheduling/cpp` 의 C++ 도메인 구현을
감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

Java quickstart 의 제약 (HardMediumSoftBigDecimalScore) 과 점수 매핑:
  hard 1. oneAssignmentPerDatePerTeam  같은 날 같은 팀의 배정 쌍마다 -1
  hard 2. unavailabilityPenalty        (팀, 일) 불가 사실에 배정이 있으면 -1
  med  3. fairAssignmentCountPerTeam   팀별 배정 수 LoadBalance 불공정도
  soft 4. evenlyConfrontationCount     팀 쌍별 대결 수 LoadBalance 불공정도

엔진 점수는 HardSoftScore(hard int, soft float) 라 medium 을 soft 에
접는다: ``soft = -(medium 불공정도 * MEDIUM_SCALE + soft 불공정도)``
(MEDIUM_SCALE == 1e6; medium 이 soft 를 항상 지배한다). "불공정도" 는
Timefold LoadBalance::unfairness — sqrt(sum(load^2) - sum(load)^2 / n) 을
유효숫자 6자리(HALF_EVEN)로 반올림한 값이다.
"""

from collections.abc import Callable, Iterable, Sequence
from typing import Any

MEDIUM_SCALE: float

class Score:
    """HardSoft 점수. `hard` 는 정수 위반 합(0 == feasible); `soft` 는
    접힌 medium/soft 목적식. 사전식 비교(hard 우선)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """hard == 0 이면 True."""

class Team:
    """problem fact: 참가 팀. 리스트 인덱스가 planning variable 의 값이다."""
    def __init__(self, name: str) -> None: ...
    name: str

class Day:
    """problem fact: 경기 일자. 배정/불가일은 days 인덱스로 참조한다."""
    def __init__(self, date_index: int) -> None: ...
    date_index: int

class UnavailabilityPenalty:
    """problem fact: (team, day) 불가 — 해당 칸에 배정이 하나라도 있으면
    하드 1점 (fact 하나당 1점; Java 의 forEach + ifExists)."""
    def __init__(self, team: int, day: int) -> None: ...
    team: int
    day: int

class TeamAssignment:
    """planning entity: 일자의 경기 슬롯 하나. `team` 이 planning variable
    (-1 == 미배정); 솔버가 쓴다. id 는 유일해야 한다 (Java @PlanningId)."""
    def __init__(
        self,
        id: int,
        day: int,
        index_in_day: int = 0,
        team: int = -1,
        pinned: bool = False,
    ) -> None: ...
    id: int
    day: int
    index_in_day: int
    team: int
    """teams 리스트 인덱스; -1 == 미배정."""
    pinned: bool
    """True 면 team 을 고정 (점수엔 반영되지만 절대 움직이지 않는다)."""

class TournamentSchedule:
    """planning solution: teams(값 범위) + days + 불가일 + 배정(entity).
    solve 는 team_assignments 의 `team` 과 `score` 를 in place 로 바꾼다.

    주의: `team_assignments` 등 리스트 속성 접근은 복사본을 돌려주므로,
    배정을 바꾸려면 `set_assignment` 를 쓴다."""
    def __init__(
        self,
        teams: Sequence[Team],
        days: Sequence[Day],
        unavailability_penalties: Sequence[UnavailabilityPenalty] = (),
        team_assignments: Sequence[TeamAssignment] = (),
    ) -> None: ...
    teams: list[Team]
    days: list[Day]
    unavailability_penalties: list[UnavailabilityPenalty]
    team_assignments: list[TeamAssignment]
    @property
    def score(self) -> Score: ...
    def team_count(self) -> int: ...
    def day_count(self) -> int: ...
    def assignment(self) -> list[int]:
        """배정별 현재 team 인덱스 (-1 == 미배정)."""

    def set_assignment(self, assignment: Sequence[int]) -> None:
        """planning variable 일괄 쓰기 (길이는 배정 수와 같아야 한다)."""

def calculate_score(schedule: TournamentSchedule) -> Score:
    """전체 재계산 (schedule.score 에도 저장)."""

def unfairness(loads: Sequence[int]) -> float:
    """Timefold LoadBalance::unfairness (유효숫자 6자리, HALF_EVEN)."""

class SolverConfig:
    """엔진 설정. 기본: 30초 예산, LA + ALNS, 싱글 스타트."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = 자동: clamp(8 * 배정 수, 400, 3200)."""
    full_assert: bool
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
    descent_weight: float
    """스텝이 move mix 대신 nearby 엔티티 쌍 best-improvement 하강을 돌릴
    확률 (한 sweep ≈ 2 * 팀 수 평가). 기본 0 = 비활성."""
    ruin_recreate_as_move_weight: float
    """acceptor 판정을 받는 평범한 무브로서의 destroy-repair 가중치
    (정체 킥과 별개). 기본 0 = 비활성."""
    ruin_recreate_as_move_max_ruin: int
    custom_move_probability: float
    """커스텀 연산자(TeamCountMove)가 스텝에서 뽑힐 확률 상한. 1 = 무제한."""
    value_recommender: bool
    """change 무브의 값 선택을 하드-피저블 팀(그날 경기 없음 + 불가일 아님)
    으로 유도한다 (모놀리식 경로 전용). 기본 False."""
    team_count_moves: bool
    """적재 팀 수 매크로 무브(팀 비우기/되살리기). medium 레벨의 팀 수
    장벽을 넘는 유일한 경로라 기본 True."""
    best_score_limit: tuple[int, float]
    """이 점수 이상이면 조기 종료. 예: (0, -1e18) == 첫 feasible 에서 정지."""
    on_new_best: Callable[[int, Score], None] | None

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: TournamentSchedule) -> None:
        """in place 로 푼다: team_assignments[*].team 과 schedule.score 를
        쓴다. 고정(pinned) 배정은 유지된다."""

    def solve_with_constraints(self, schedule: TournamentSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (tournament_scheduling.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    tournament_scheduling.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `tournament_scheduling.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
