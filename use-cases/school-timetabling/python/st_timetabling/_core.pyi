"""`st_timetabling._core` 타입 스텁 — school-timetabling C++ 솔버.

컴파일된 모듈은 `use-cases/school-timetabling/cpp` 의 C++ 도메인 구현을
감싼다; 그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.
두 도메인이 노출된다:

- 모듈 최상위: quickstart 포트 (`Timetable`, slotRoom 합성 인코딩, 아래 7제약)
- `canon` 서브모듈: 캐노니컬 풀-패리티 도메인 (`st::canon`) — CP-SAT 레퍼런스
  패키지(benchmarks/school-timetabling/cpsat)의 하드 H1~H10 / 소프트 S1~S6 을 validate.py
  의미론 그대로 구현한다.

quickstart 도메인은 entity(Lesson)당 BASIC int 변수 1개만 쓰므로
(timeslot, room) 2변수는 합성 인코딩 하나로 표현된다::

    slot_room = timeslot_index * room_count + room_index   # -1 == 미배정

점수 규약: `hard` 는 제약 위반 수(0 == feasible), `soft` 는 최적화 목적식.
둘 다 최대화한다 (덜 음수일수록 좋다).

quickstart 제약 (HardSoftScore):
  hard 1. roomConflict          같은 timeslot+room 의 수업 쌍마다 -1
  hard 2. teacherConflict       같은 timeslot 의 동일 교사 수업 쌍마다 -1
  hard 3. studentGroupConflict  같은 timeslot 의 동일 학급 수업 쌍마다 -1
  hard 4. teacherUnavailable    교사가 불가한 timeslot 의 수업마다 -1 (확장)
  soft 5. teacherRoomStability  다른 교실의 동일 교사 수업 쌍마다 -1
  soft 6. teacherTimeEfficiency 같은 날 연속 교시의 동일 교사 수업 쌍마다 +1
  soft 7. studentGroupSubjectVariety  같은 날 연속 교시의 동일 학급·동일 과목
                                수업 쌍마다 -1
"""

from collections.abc import Callable, Iterable
from typing import Protocol, TypedDict, overload

class Score:
    """HardSoft 점수. `hard` 는 정수 위반 합(0 == feasible); `soft` 는
    목적식. 사전식 비교(hard 우선)."""
    @property
    def hard(self) -> int: ...
    @property
    def soft(self) -> float: ...
    def is_feasible(self) -> bool:
        """hard == 0 이면 True."""

class Timeslot:
    """problem fact: (요일, 교시) 한 칸. day 0=월; period 는 1-base.
    (day, period) 쌍은 Timetable 안에서 유일해야 한다."""
    def __init__(self, day: int, period: int) -> None: ...
    day: int
    period: int

class Room:
    """problem fact: 교실/특별실. `type` 은 정보용 — hard 제약이 아니다."""
    def __init__(self, name: str, type: str = "") -> None: ...
    name: str
    type: str

class Lesson:
    """planning entity: 시간표에 꽂을 수업 한 칸. `slot_room` 이 planning
    variable (-1 == 미배정); 솔버가 쓴다."""
    def __init__(
        self,
        id: str,
        subject: str,
        teacher: str,
        student_group: str,
    ) -> None: ...
    id: str
    subject: str
    teacher: str
    student_group: str
    slot_room: int
    """timeslot_index * room_count + room_index; -1 == 미배정."""
    pinned: bool
    """True 면 slot_room 을 고정 (점수엔 반영되지만 절대 움직이지 않는다)."""

class Timetable:
    """planning solution: timeslots x rooms 그리드(값 범위) + lessons(entity).
    solve 는 lessons 의 `slot_room` 과 `score` 를 in place 로 바꾼다.

    주의: `lessons` 등 리스트 속성 접근은 복사본을 돌려주므로, 배정을 바꾸려면
    `set_assignment` 를 쓰거나 Timetable 생성 전에 Lesson 에 써 둔다."""
    def __init__(
        self,
        timeslots: list[Timeslot],
        rooms: list[Room],
        lessons: list[Lesson],
        teacher_unavailable: dict[str, set[int]] = ...,
    ) -> None: ...
    timeslots: list[Timeslot]
    rooms: list[Room]
    lessons: list[Lesson]
    teacher_unavailable: dict[str, set[int]]
    """교사 id -> 배정 불가 timeslot index 집합. 없는 교사는 항상 가능."""
    @property
    def score(self) -> Score: ...
    def timeslot_count(self) -> int: ...
    def room_count(self) -> int: ...
    def slot_room_count(self) -> int:
        """timeslot_count() * room_count() — slot_room 값 범위의 크기."""
    def slot_room_of(self, timeslot_index: int, room_index: int) -> int: ...
    def timeslot_index_of(self, slot_room: int) -> int: ...
    def room_index_of(self, slot_room: int) -> int: ...
    def assignment(self) -> list[int]:
        """수업별 현재 slot_room (솔루션 벡터)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """수업별 slot_room 일괄 쓰기. 길이가 lessons 수와 다르면 ValueError."""

def calculate_score(table: Timetable) -> Score:
    """7개 제약 전체의 full recompute (`table.score` 에도 저장)."""

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
    """0 은 비결정적 시드; 그 외 값은 재현 가능."""
    parallel_solver_count: int
    """독립 병렬 시작 수 (엔진 multi-start): worker i 는 random_seed+i 와
    전체 예산을 받고, 최고 해가 이긴다. 기본 1."""
    late_acceptance_size: int
    """late-acceptance 큐 길이. 0(기본) = 자동: clamp(8 * 레슨 수, 400,
    3200)."""
    full_assert: bool
    """True 면 매 move 마다 incremental 점수를 full recompute 와 대조
    (디버그용; 매우 느림)."""
    ruin_recreate: bool
    """정체 시 adaptive ruin-and-recreate 킥 활성화."""
    ruin_recreate_unimproved_step_trigger: int
    """킥 전 최소 unimproved step 수."""
    ruin_recreate_min_ruin_fraction: float
    """킥당 ruin 할 entity 최소 비율."""
    ruin_recreate_max_ruin_fraction: float
    """킥당 ruin 할 entity 최대 비율."""
    ruin_recreate_max_ruin_count: int
    """킥당 ruin entity 수의 절대 상한."""
    ruin_recreate_operator_decay: float
    """adaptive operator 가중치 감쇠; 높을수록 천천히 적응."""
    ruin_recreate_rewards: list[float]
    """NEW_BEST / BETTER / ACCEPTED / REJECTED 결과별 보상 (정확히 4개)."""
    ruin_recreate_polish_steps: int
    """킥당 hill-climbing 소규모 polish step 수."""
    ruin_recreate_as_move_weight: float
    """destroy&repair 를 acceptor 판정을 받는 일반 무브로 섞는 상대 가중치.
    음수(기본) = 자동: slotRoomCount 가 측정된 이득 구간 [900, 1300] 안이면
    0.2 / slotRoomCount, 밖이면 0 (benchmarks/school-timetabling/RESULTS.md). 0 = 비활성, 양수는 그대로
    사용. 한 번의 as-move 는 ruin 수 x slotRoomCount 번의 채점 비용이다."""
    ruin_recreate_as_move_max_ruin: int
    """as-move 한 번이 ruin 하는 레슨 수 상한 (기본 12)."""
    descent_weight: float
    """nearby 레슨 쌍 best-improvement 하강을 섞는 상대 가중치 (기본 0 =
    비활성; 이 도메인에서는 as-move 보다 약하다고 측정됨, benchmarks/school-timetabling/RESULTS.md)."""
    descent_max_sweeps: int
    """하강 한 번의 최대 sweep 수 (기본 4)."""
    change_move_weight: float
    """BASIC change move 상대 가중치; 0 이면 비활성."""
    swap_move_weight: float
    """BASIC swap move 상대 가중치; 0 이면 비활성."""
    best_score_limit: tuple[int, float]
    """bestScore >= (hard, soft) 가 되면 조기 종료 (사전식 비교).
    (0, -1e18) 은 첫 feasible 해에서 멈춘다. 기본: 도달 불가."""
    on_new_best: Callable[[int, Score], None] | None
    """best 해가 갱신될 때마다 호출: on_new_best(step, score). GIL 은
    콜백 진입 시 다시 잡는다 (multi-start worker thread 에서도 안전)."""
    same_group_nearby: bool
    """같은 studentGroup 레슨을 nearby 이웃으로 등록 (swap 파트너 편향 +
    related ruin). 기본 True."""
    same_group_nearby_probability: float
    """swap 파트너를 nearby(같은 그룹)에서 뽑을 확률 (기본 0.5)."""

class Solver:
    """local-search 솔버 (construction heuristic + late acceptance + ALNS).
    IN PLACE 로 푼다: `lesson.slot_room` 과 `table.score` 를 쓴다.
    pinned 수업은 배정을 유지한다."""
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, table: Timetable) -> None: ...

class canon:
    """캐노니컬 풀-패리티 도메인 서브모듈 (`st::canon`).

    런타임에는 `st_timetabling._core.canon` 모듈이다 (스텁에서는 네임스페이스
    클래스로 표기). CP-SAT 레퍼런스의 하드 H1~H10 / 소프트 S1~S6 을
    validate.py 의미론 그대로 구현한다:

      H1 미배정  H2 fixed(pinned)  H3 학급 충돌 C(n,2)  H4 교사 충돌 C(n,2)
      H5 allowed_slot 밖 배정      H6 (특별실 타입, 시간)별 수요 초과
      H7 (강좌, 요일) 일일 상한 초과  H8 연강 끊김  H9 교사 일일/주당 상한
      H10 교사 연속 수업 상한
      S1 교사 공강  S2 교사 출근일  S3 희망 공강일  S4 학급 공강
      S5 기피 교시  S6 선호 교시 이탈

    설계 한계: 교시 <= 16, 요일 index < 8 (비트마스크; prepare 가 거부한다).
    """

    class ObjectiveWeights:
        """S1~S6 가중치 (cpsat ObjectiveWeights 기본값). 0 이면 항 제외."""
        def __init__(
            self,
            teacher_gap: int = 20,
            teacher_workday: int = 3,
            teacher_free_day: int = 30,
            class_gap: int = 10,
            avoided_period: int = 5,
            non_preferred_period: int = 1,
        ) -> None: ...
        teacher_gap: int
        teacher_workday: int
        teacher_free_day: int
        class_gap: int
        avoided_period: int
        non_preferred_period: int

    class Lesson:
        """planning entity: (병합될 수 있는) 강좌의 주당 1시간. `slot` 이
        planning variable (timeslot index, -1 == 미배정)."""
        def __init__(
            self,
            id: str,
            class_group_id: int,
            course_key: int,
            teachers: list[int],
            room_type_demand: list[int] = ...,
            block_group: int = -1,
            pinned: bool = False,
            slot: int = -1,
        ) -> None: ...
        id: str
        class_group_id: int
        course_key: int
        teachers: list[int]
        """수업에 서는 교사 dense id 전부 (팀티칭 >= 2, 병합 레슨은 합집합)."""
        room_type_demand: list[int]
        """특별실 타입별 수요 (H6). 빈 리스트 = 수요 없음."""
        allowed_slot: list[bool]
        """slot 별 허용 테이블 (H5). 빈 리스트 = 전부 허용."""
        preference_units: list[tuple[int, int]]
        """S5/S6 단위: 멤버 강좌마다 (기피 마스크, 선호 마스크) 1쌍
        (bit = period - 1). 비면 course_key 마스크에서 유도."""
        block_group: int
        """연강 그룹 id (H8), -1 = 없음."""
        pinned: bool
        """True 면 slot 고정 (H2: 엔진이 절대 움직이지 않는다)."""
        slot: int

    class Timetable:
        """planning solution. slot 은 (day_of_slot[s], period_of_slot[s]) 로
        열거된다 — 요일마다 교시 수가 달라도 존재하는 칸만 나열하면 된다."""
        def __init__(
            self,
            day_of_slot: list[int],
            period_of_slot: list[int],
            lessons: list[canon.Lesson],
        ) -> None: ...
        day_of_slot: list[int]
        period_of_slot: list[int]
        lessons: list[canon.Lesson]
        teacher_count: int
        class_count: int
        room_capacity_by_type: list[int]
        """특별실 타입별 보유 수 (H6)."""
        teacher_max_per_day: list[int]
        teacher_max_per_week: list[int]
        """-1 = 무제한 (cpsat 의 None)."""
        teacher_max_consecutive: list[int]
        teacher_preferred_free_days: list[int]
        """교사별 희망 공강일 비트마스크 (bit d = 0-base 요일)."""
        course_daily_limit: list[int]
        course_avoided_period_mask: list[int]
        course_preferred_period_mask: list[int]
        weights: canon.ObjectiveWeights
        @property
        def score(self) -> Score: ...
        @property
        def prepared(self) -> bool: ...
        def slot_count(self) -> int: ...
        def assignment(self) -> list[int]:
            """수업별 현재 slot (솔루션 벡터)."""
        def set_assignment(self, assignment: list[int]) -> None:
            """수업별 slot 일괄 쓰기. 길이가 lessons 수와 다르면 ValueError."""

    class Breakdown:
        """제약별 위반 카운트 (validate.py 의 하드 검증 + compute_penalties
        의미론). hard_total() 이 hard 점수의 절대값, soft_penalty(weights) 가
        soft 점수의 절대값이다."""
        @property
        def unassigned(self) -> int: ...
        @property
        def class_conflicts(self) -> int: ...
        @property
        def teacher_conflicts(self) -> int: ...
        @property
        def slot_not_allowed(self) -> int: ...
        @property
        def room_over_capacity(self) -> int: ...
        @property
        def course_daily_excess(self) -> int: ...
        @property
        def block_breaks(self) -> int: ...
        @property
        def teacher_daily_excess(self) -> int: ...
        @property
        def teacher_weekly_excess(self) -> int: ...
        @property
        def consecutive_excess(self) -> int: ...
        @property
        def teacher_gap(self) -> int: ...
        @property
        def teacher_workday(self) -> int: ...
        @property
        def teacher_free_day(self) -> int: ...
        @property
        def class_gap(self) -> int: ...
        @property
        def avoided_period(self) -> int: ...
        @property
        def non_preferred_period(self) -> int: ...
        def hard_total(self) -> int: ...
        def soft_penalty(self, weights: canon.ObjectiveWeights) -> int: ...
        def penalties(self) -> dict[str, int]:
            """S1~S6 원시 카운트 (cpsat Timetable.penalties 와 같은 key)."""

    @staticmethod
    def calculate_score(table: canon.Timetable) -> Score:
        """HardSoftScore full recompute (`table.score` 에도 저장)."""

    @staticmethod
    def compute_breakdown(table: canon.Timetable) -> canon.Breakdown:
        """제약별 full recompute (H1~H10 / S1~S6 카운트)."""

    @staticmethod
    def prepare(table: canon.Timetable) -> None:
        """인스턴스 정규화 (멱등; compute_breakdown/calculate_score/solve 도
        호출한다). 잘못된 입력은 ValueError."""

    class ConstraintSpec(Protocol):
        """제약 스펙 duck-type: `st_timetabling.constraints` 의 `Constraint`
        dataclass 들과 `Assembly` 항목이 해당한다. `weight` 는 None 이면
        컴포넌트 기본값(하드 H* = 1, 소프트 S* = cpsat 기본 가중치)."""

        @property
        def name(self) -> str: ...
        @property
        def weight(self) -> float | None: ...
        @property
        def hard(self) -> bool | None: ...
        @property
        def exponent(self) -> float: ...

    CONSTRAINT_NAMES: tuple[str, ...]
    """`DecomposedCalculator.add` 가 받는 이름 전부 (레지스트리 순서):

    - 하드 (기본 weight 1): ``"unassigned_lesson"`` (H1),
      ``"class_conflict"`` (H3), ``"teacher_conflict"`` (H4),
      ``"slot_not_allowed"`` (H5), ``"room_capacity"`` (H6),
      ``"course_daily_limit"`` (H7), ``"block_adjacency"`` (H8),
      ``"teacher_load_limit"`` (H9), ``"teacher_consecutive_limit"`` (H10)
    - 소프트 (기본 weight = cpsat ObjectiveWeights 기본값):
      ``"teacher_gap"`` (S1, 20), ``"teacher_workday"`` (S2, 3),
      ``"teacher_free_day"`` (S3, 30), ``"class_gap"`` (S4, 10),
      ``"avoided_period"`` (S5, 5), ``"non_preferred_period"`` (S6, 1)

    H2(고정 시간)는 pinned 레슨으로 강제돼 채점 대상이 아니다.
    """

    class ConstraintTotalDict(TypedDict):
        name: str
        cost: float
        """가중 적용 후 합계; 양수 = 벌점."""
        is_hard: bool

    class DecomposedCalculator:
        """플러그인 방식 비용 조립 (es 패턴): 제약을 골라 weight 를 바꾸고
        하드/소프트 레벨을 뒤집은 뒤 ``Solver.solve(table, calc)`` 에
        넘기거나 고정 배정을 직접 채점한다.

        스펙 객체 iterable(`st_timetabling.constraints` 의 `Assembly` /
        스펙 리스트)로 조립하거나 :meth:`add` 로 이름을 하나씩 넣는다.
        전체 15개 기본 세트(:func:`default_calculator`)는 `weights` 가
        `table.weights` 와 같을 때 :func:`calculate_score` 와 비트 단위로
        일치한다."""

        @overload
        def __init__(self) -> None: ...
        @overload
        def __init__(self, constraints: Iterable[canon.ConstraintSpec]) -> None:
            """duck-typed: 항목은 `.name`/`.weight`/`.hard`/`.exponent` 만
            있으면 된다 (`Assembly` 와 `Constraint` dataclass 모두 해당)."""

        def add(
            self,
            name: str,
            weight: float | None = None,
            hard: bool | None = None,
            exponent: float = 1.0,
        ) -> None:
            """내장 캐노니컬 제약을 이름으로 추가한다 (`CONSTRAINT_NAMES`).

            weight:   제약 비용의 선형 배수; None 은 컴포넌트 기본값 유지.
            hard:     True/False 로 기본 하드/소프트 레벨을 뒤집는다
                      (None = 기본 유지).
            exponent: 부호 보존 거듭제곱 셰이핑:
                      contribution = weight * sign(c)*|c|^exponent.
            알 수 없는 이름은 ValueError. 중복 추가는 허용 (비용 합산)."""

        def score(self, table: canon.Timetable) -> Score:
            """`table` 의 현재 배정을 이 조립으로 채점한다 (`table.score` 에도
            저장). 채점 후 :meth:`constraint_totals` 가 그 결과를 설명한다."""

        def constraint_totals(self) -> list[canon.ConstraintTotalDict]:
            """마지막 채점/solve 결과의 제약별 가중 비용 (점수 설명)."""

        @property
        def delta_group_count(self) -> int:
            """마지막 reset/solve 가 컴파일한 구조적 delta group 수."""

    @staticmethod
    def default_calculator(weights: canon.ObjectiveWeights = ...) -> canon.DecomposedCalculator:
        """전체 캐노니컬 세트가 담긴 DecomposedCalculator: 하드 H1~H10 은
        weight 1, 소프트 S1~S6 은 `weights` (기본: cpsat 기본 가중치)."""

    class SolverConfig:
        """`canon.Solver` 설정. 엔진 공통 필드는 최상위 `SolverConfig` 와
        같고, canonical 전용으로 `weights` 와 유도 탐색 노브
        (`hot_lesson_probability`, `same_class_nearby*`,
        `smart_class_swap_*`)가 추가된다."""
        def __init__(self) -> None: ...
        spent_limit_seconds: float
        unimproved_spent_limit_seconds: float
        step_count_limit: int
        unimproved_step_count_limit: int
        random_seed: int
        parallel_solver_count: int
        late_acceptance_size: int
        """0 = 자동: clamp(4 * 레슨 수, 400, 3200). 양수는 그대로 사용."""
        full_assert: bool
        ruin_recreate: bool
        ruin_recreate_unimproved_step_trigger: int
        ruin_recreate_min_ruin_fraction: float
        ruin_recreate_max_ruin_fraction: float
        ruin_recreate_max_ruin_count: int
        ruin_recreate_operator_decay: float
        ruin_recreate_rewards: list[float]
        ruin_recreate_polish_steps: int
        ruin_recreate_as_move_weight: float
        """destroy&repair as-move 상대 가중치 (기본 0 = 비활성 — quickstart
        쪽과 달리 자동 규칙은 없다; 음수도 비활성)."""
        ruin_recreate_as_move_max_ruin: int
        descent_weight: float
        descent_max_sweeps: int
        change_move_weight: float
        swap_move_weight: float
        best_score_limit: tuple[int, float]
        on_new_best: Callable[[int, Score], None] | None
        weights: canon.ObjectiveWeights
        """solve() 시작 시 Timetable.weights 로 복사된다."""
        hot_lesson_probability: float
        """하드 위반 연루 레슨을 무브 대상으로 뽑을 확률 (0 = 비활성)."""
        same_class_nearby: bool
        """같은 학급 레슨을 nearby 이웃으로 등록 (swap 파트너/related ruin)."""
        same_class_nearby_probability: float
        smart_class_swap_moves: bool
        """best-of-k 같은학급 swap 커스텀 무브 (soft 단계 전용 k-평가)."""
        smart_class_swap_candidates: int
        reassign_candidates: int
        """미배정 레슨을 되돌릴 때 재보는 배치 수 (부분 편성 전용).

        조립이 H1 을 soft 로 내렸을 때만 쓰인다. 빽빽한 시간표에 다시 넣으려면
        아무와도 안 부딪히는 슬롯을 찾아야 하는데 무작위 한 칸은 거의 못 찾는다
        — 넓게 보는 것이 이 무브의 요점이라 swap 쪽 k 보다 크다. 트랙3 스윕
        (partial_M, 10초): 8 -> 325칸, 16 -> 328, 32 -> 342, 64 -> 341."""

    class Solver:
        """canonical 도메인 local-search 솔버 (allowed_slot 기반 value
        recommender + FFD 구성 순서 + ALNS). IN PLACE 로 푼다."""
        def __init__(self, config: canon.SolverConfig = ...) -> None: ...
        @overload
        def solve(self, table: canon.Timetable) -> None:
            """모놀리식 인크리멘털 계산기의 fast path (기본)."""
        @overload
        def solve(self, table: canon.Timetable, calculator: canon.DecomposedCalculator) -> None:
            """플러그인 조립 경로: 탐색과 최종 `table.score` 모두 `calculator`
            를 쓴다 (soft 가중치는 조립에서 온다 — config.weights 무시).
            single-start: parallel_solver_count 는 무시된다.

            조립이 H1(`unassigned_lesson`)을 soft 로 내렸거나 아예 빼면
            미배정 무브가 켜진다 — 그때만 탐색이 레슨을 slot -1 로 남길 수
            있다 (BASIC 무브 믹스의 값 범위는 [0, slot_count) 뿐이다). H1 이
            하드면 기본 경로와 똑같이 모든 레슨이 슬롯을 잡는다."""
