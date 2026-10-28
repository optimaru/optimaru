"""`bed_allocation._core` 타입 스텁 — bed-allocation C++ 솔버.

컴파일된 모듈은 `use-cases/bed-allocation/cpp` 의 C++ 도메인 구현을 감싼다;
그 구현은 `include/optimaru` 의 범용 엔진을 임포트한다.

planning variable 은 `Stay.bed` 하나다 (BASIC int)::

    -1                            미초기화 (점수 기여 없음)
    0..bed_count()-1              실제 병상 인덱스
    bed_count() == no_bed_value() "no bed" 센티널 — quickstart 의
                                  null 병상 (allowsUnassigned = true);
                                  medium 레벨로 박수만큼 벌점

점수 규약 (HardMediumSoftScore, Java quickstart 그대로; 전부 벌점):
  hard 1.  sameBedInSameNight             같은 병상 겹침 박수 x -1000
  hard 2.  femaleInMaleRoom               MALE_ONLY 병실의 여성, 박수 x -50
  hard 3.  maleInFemaleRoom               FEMALE_ONLY 병실의 남성, 박수 x -50
  hard 4.  differentGenderInSameGenderRoomInSameNight
                                          SAME_GENDER 병실의 혼성 겹침 박수
                                          x -1000
  hard 5.  departmentMinimumAge           부서 최소 연령 미달, 박수 x -100
  hard 6.  departmentMaximumAge           부서 최대 연령 초과, 박수 x -100
  hard 7.  requiredPatientEquipment       필수 장비가 모자란 병실: 박수 x
                                          (병실에 '있는' 필수 장비 수) x -50
                                          (Java 원본의 유닛테스트로 고정된
                                          의미론 그대로)
  medium 8. assignEveryPatientToABed      "no bed" 재원의 박수 x -1
  soft 9.  preferredMaximumRoomCapacity   선호 정원 초과 병실, 박수 x -8
  soft 10. departmentSpecialty            부서 미제공 전문과목, 박수 x -10
  soft 11. departmentSpecialtyNotFirstPriority
                                          우선순위 p > 1 이면 박수 x (p-1)
                                          x -10
  soft 12. preferredPatientEquipment      박수 x (없는 선호 장비 수) x -50

엔진 내부는 HardSoftScore(int64 hard, double soft) 이고
`engine.soft = medium * 10**9 + soft` 로 매핑된다 (|soft| < 10**9 에서
레벨 우선순위 보존; 자세한 규칙은 cpp/include/ba/domain.hpp).
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

class Gender(Enum):
    MALE = ...
    FEMALE = ...

class GenderLimitation(Enum):
    ANY_GENDER = ...
    MALE_ONLY = ...
    FEMALE_ONLY = ...
    SAME_GENDER = ...

class Department:
    """problem fact: 진료 부서. minimum_age/maximum_age None == 제한 없음."""
    def __init__(
        self,
        id: str,
        name: str,
        specialty_to_priority: dict[str, int] = ...,
        minimum_age: int | None = None,
        maximum_age: int | None = None,
    ) -> None: ...
    id: str
    name: str
    specialty_to_priority: dict[str, int]
    minimum_age: int | None
    maximum_age: int | None

class Room:
    """problem fact: 병실. `department` 는 BedPlan.departments 인덱스."""
    def __init__(
        self,
        id: str,
        name: str,
        department: int = 0,
        capacity: int = 1,
        gender_limitation: GenderLimitation = ...,
        equipment: list[str] = ...,
    ) -> None: ...
    id: str
    name: str
    department: int
    capacity: int
    gender_limitation: GenderLimitation
    equipment: list[str]

class Bed:
    """problem fact (planning value): 병상. `room` 은 BedPlan.rooms 인덱스."""
    def __init__(self, id: str, room: int, index_in_room: int = 0) -> None: ...
    id: str
    room: int
    index_in_room: int

class Stay:
    """planning entity: 환자 재원 한 건. `bed` 가 planning variable
    (모듈 docstring 의 인코딩 참고); 솔버가 쓴다. 날짜는 정수 일 인덱스
    (차이만 의미 있음; night_count = departure - arrival + 1)."""
    def __init__(
        self,
        id: str,
        patient_name: str,
        patient_gender: Gender,
        patient_age: int,
        arrival_day: int,
        departure_day: int,
        specialty: str = "",
        patient_preferred_maximum_room_capacity: int | None = None,
        patient_required_equipment: list[str] = ...,
        patient_preferred_equipment: list[str] = ...,
        bed: int = -1,
    ) -> None: ...
    id: str
    patient_name: str
    patient_gender: Gender
    patient_age: int
    patient_preferred_maximum_room_capacity: int | None
    patient_required_equipment: list[str]
    patient_preferred_equipment: list[str]
    arrival_day: int
    departure_day: int
    specialty: str
    bed: int
    def night_count(self) -> int: ...
    def same_night_count(self, other: Stay) -> int: ...

class BedPlan:
    """planning solution. solve 는 stays 의 `bed` 와 `score` 를 in place 로
    바꾼다.

    주의: `stays` 등 리스트 속성 접근은 복사본을 돌려주므로, 배정을 바꾸려면
    `set_assignment` 를 써야 한다."""
    def __init__(
        self,
        departments: list[Department],
        rooms: list[Room],
        beds: list[Bed],
        stays: list[Stay],
    ) -> None: ...
    @property
    def departments(self) -> list[Department]: ...
    @property
    def rooms(self) -> list[Room]: ...
    @property
    def beds(self) -> list[Bed]: ...
    @property
    def stays(self) -> list[Stay]: ...
    @property
    def score(self) -> Score: ...
    def bed_count(self) -> int: ...
    def no_bed_value(self) -> int:
        """ "no bed" 센티널 planning 값 (== bed_count())."""
    def assignment(self) -> list[int]:
        """stay 별 현재 bed 값 (해)."""
    def set_assignment(self, assignment: list[int]) -> None:
        """planning variable 일괄 쓰기 (`stays` 접근은 복사본이라 개별
        쓰기는 반영되지 않는다)."""

def calculate_score(plan: BedPlan) -> Score:
    """HardMediumSoft 점수의 full recompute (plan.score 에도 저장)."""

class SolverConfig:
    def __init__(self) -> None: ...
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    """0 = 자동: clamp(8 * stay 수, 8000, 12000)."""
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
    nearby_probability: float
    """stay 의 날짜 구간이 가까운 것끼리 swap 파트너/related ruin 을 편향.
    0 이면 nearby 선택 비활성화."""
    nearby_size: int
    """nearby 이웃 목록 크기."""
    best_score_limit: tuple[int, int, int]
    """(hard, medium, soft) 이상에 도달하면 조기 종료. 예: (0, -(2**62),
    -(2**62)) 는 첫 feasible 해에서 멈춘다."""
    on_new_best: Callable[[int, Score], None] | None
    """새 best 해마다 (step, score) 로 호출."""

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, plan: BedPlan) -> None:
        """in place 로 푼다: stays[*].bed 와 plan.score 를 채운다. 모든
        stay 는 실제 병상 또는 "no bed" 센티널 값을 갖게 된다 (-1 아님)."""

    def solve_with_constraints(self, plan: BedPlan, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (bed_allocation.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    bed_allocation.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `bed_allocation.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
