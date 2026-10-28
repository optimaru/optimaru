"""`sports_league._core` 타입 스텁 — sports-league-scheduling C++ 솔버.

컴파일된 모듈은 `use-cases/sports-league-scheduling/cpp` 의 C++ 도메인
구현을 감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

도메인: 리그의 모든 경기(Match, planning entity)에 라운드(Round, planning
값)를 배정한다. `Match.round` 가 planning variable 로, `LeagueSchedule.
rounds` 리스트의 위치(-1 == 미배정)를 담는다. 라운드 인접성(같은 날 /
다음 날 / 처음 / 마지막)은 전부 `Round.index` 기준이다.

점수 규약: `hard` 는 제약 위반 수(0 == feasible), `soft` 는 이동 거리(km)
와 클래식 벌점의 합에 음수를 취한 값. 둘 다 최대화한다.

제약 (HardSoftScore, Java quickstart 와 동일 의미론):
  hard 1. matchesOnSameDay      같은 라운드 인덱스에서 팀을 공유하는 경기
                                쌍마다 -1 (두 팀을 다 공유해도 쌍당 1번)
  hard 2. consecutiveHome       한 팀의 홈 경기가 구별되는 라운드 인덱스
                                4개 이상을 연속으로 덮으면 그 런의 길이만큼
                                -n (같은 인덱스의 중복 경기는 1로 센다)
  hard 3. consecutiveAway       원정 경기도 동일
  hard 4. repeatMatchOnNextDay  다음 라운드 인덱스에 리턴 매치가 있으면
                                경기마다 -1 (ifExists: 경기당 최대 1번)
  soft 5. startToAwayHop        이전 라운드 인덱스가 없는 경기마다
                                -dist(away, home)
  soft 6. homeToAwayHop         연속 인덱스 경기쌍 (m1, m2) 에서 m2.away ==
                                m1.home 이면 -dist(m1.home, m2.home)
  soft 7. awayToAwayHop         m2.away == m1.away 이면
                                -dist(m1.home, m2.home)
  soft 8. awayToHomeHop         m2.home == m1.away 이면
                                -dist(m1.home, m1.away)
  soft 9. awayToEndHop          다음 라운드 인덱스가 없는 경기마다
                                -dist(home, away)
  soft 10. classicMatches       주말/휴일이 아닌 라운드의 클래식 경기마다
                                -1000
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

class Team:
    """problem fact: 팀. 경기는 팀 리스트의 위치로 참조한다."""
    def __init__(self, id: str, name: str = "") -> None: ...
    id: str
    name: str

class Round:
    """problem fact 이자 planning 값. `index` 는 제약이 쓰는 라운드 번호
    (>= 0; 보통 0..N-1 연속이지만 희소해도 된다)."""
    def __init__(self, index: int, weekend_or_holiday: bool = False) -> None: ...
    index: int
    weekend_or_holiday: bool

class Match:
    """planning entity: 라운드를 배정받을 경기 하나. `round` 가 planning
    variable (-1 == 미배정, 값은 rounds 리스트의 위치); 솔버가 쓴다."""
    def __init__(
        self,
        id: str,
        home_team: int,
        away_team: int,
        classic_match: bool = False,
        round: int = -1,
    ) -> None: ...
    id: str
    home_team: int
    away_team: int
    classic_match: bool
    round: int

class LeagueSchedule:
    """planning solution: teams/rounds(값 범위) + matches(entity) + 거리
    행렬. solve 는 matches 의 `round` 와 `score` 를 in place 로 바꾼다.

    주의: `matches` 등 리스트 속성 접근은 복사본을 돌려주므로, 배정을
    바꾸려면 `set_assignment` 를 쓰거나 LeagueSchedule 생성 전에 Match 에
    써 둔다."""
    def __init__(
        self,
        teams: list[Team],
        rounds: list[Round],
        matches: list[Match],
        distance_to_team: list[list[int]] = ...,
    ) -> None: ...
    teams: list[Team]
    rounds: list[Round]
    matches: list[Match]
    distance_to_team: list[list[int]]
    """[from][to] km 거리 행렬 (방향 보존). 빈 리스트 = 전부 0."""
    @property
    def score(self) -> Score: ...
    def team_count(self) -> int: ...
    def round_count(self) -> int: ...
    def match_count(self) -> int: ...
    def assignment(self) -> list[int]:
        """경기별 현재 round 값 (솔루션 벡터)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """경기별 round 일괄 쓰기. 길이가 경기 수와 다르면 ValueError."""

class Breakdown:
    """제약별 full recompute 집계 (전부 양수 카운트/거리; 부호는 score 가
    적용한다)."""
    @property
    def matches_on_same_day(self) -> int: ...
    @property
    def consecutive_home_matches(self) -> int: ...
    @property
    def consecutive_away_matches(self) -> int: ...
    @property
    def repeat_match_on_next_day(self) -> int: ...
    @property
    def start_to_away_hop(self) -> int: ...
    @property
    def home_to_away_hop(self) -> int: ...
    @property
    def away_to_away_hop(self) -> int: ...
    @property
    def away_to_home_hop(self) -> int: ...
    @property
    def away_to_end_hop(self) -> int: ...
    @property
    def classic_match_count(self) -> int: ...
    def hard_total(self) -> int: ...
    def soft_total(self) -> int:
        """이동 거리 합 + 1000 * classic_match_count."""

def calculate_score(schedule: LeagueSchedule) -> Score:
    """10개 제약 전체의 full recompute (`schedule.score` 에도 저장)."""

def compute_breakdown(schedule: LeagueSchedule) -> Breakdown:
    """제약별 위반 집계의 full recompute."""

class SolverConfig:
    """`Solver` 의 종료 조건 / 시드 / 탐색 옵션."""
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    """solve() 의 wall-clock 예산 (기본 30초)."""
    unimproved_spent_limit_seconds: float
    """마지막 best 갱신 이후 경과 시간 제한; 0 이면 비활성."""
    step_count_limit: int
    """local-search step 예산; 0 이면 비활성."""
    unimproved_step_count_limit: int
    """마지막 best 갱신 이후 step 수 제한; 0 이면 비활성."""
    random_seed: int
    parallel_solver_count: int
    """독립 멀티스타트 수 (seed+i); best 가 이긴다."""
    late_acceptance_size: int
    """LA 히스토리 길이. 0 = 자동 (clamp(32 * 경기 수, 400, 3200))."""
    full_assert: bool
    """move 마다 incremental == full recompute 재검증 (디버깅용, 느림)."""
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
    nearby_size: int
    """nearby 이웃 수 — 같은 팀을 공유하는 경기를 홈 구장 거리 순으로.
    0 = 자동 (4 * 팀 수), 음수면 nearby 선택 비활성."""
    nearby_probability: float
    """swap 파트너를 nearby 이웃에서 뽑을 확률."""
    costliest_match_sample: int
    """이동할 경기를 뽑을 때 무작위 표본 크기 — 이 중 현재 이동거리
    페널티가 가장 큰 경기를 고른다 (토너먼트 선택). 1 이면 균등 무작위.
    infeasible 인 동안에는 자동으로 균등 무작위로 물러난다."""
    best_score_limit: tuple[int, float]
    """bestScore >= (hard, soft) 도달 시 조기 종료. (0, -1e18) 은 첫
    feasible 에서 멈춘다."""
    on_new_best: Callable[[int, Score], None] | None
    """새 best 마다 호출: on_new_best(step, score)."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: LeagueSchedule) -> None:
        """in place 로 푼다: matches[*].round 와 schedule.score 를 쓴다."""

    def solve_with_constraints(self, schedule: LeagueSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (sports_league.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    sports_league.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `sports_league.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
