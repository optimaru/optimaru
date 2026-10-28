"""Typed stubs for the pybind11 extension food_packaging._core."""

from collections.abc import Callable, Iterable
from typing import Any

class Score:
    def __init__(self, hard: int = 0, medium: int = 0, soft: int = 0) -> None: ...
    @property
    def hard(self) -> int: ...
    @property
    def medium(self) -> int: ...
    @property
    def soft(self) -> int: ...
    def is_feasible(self) -> bool: ...
    def __eq__(self, other: object) -> bool: ...

class ConstraintBreakdown:
    @property
    def max_end_date_time(self) -> int: ...
    @property
    def operator_cleaning_conflict(self) -> int: ...
    @property
    def ideal_end_date_time(self) -> int: ...
    @property
    def maximize_jobs_assigned(self) -> int: ...
    @property
    def minimize_makespan(self) -> int: ...
    def to_score(self) -> Score: ...

class Product:
    id: str
    name: str
    def __init__(self, id: str, name: str) -> None: ...

class Operator:
    id: str
    def __init__(self, id: str) -> None: ...

class Job:
    id: str
    name: str
    product_index: int
    duration_minutes: int
    min_start_minute: int
    ideal_end_minute: int
    max_end_minute: int
    priority: int
    pinned: bool
    def __init__(
        self,
        id: str,
        name: str,
        product_index: int,
        duration_minutes: int,
        min_start_minute: int,
        ideal_end_minute: int,
        max_end_minute: int,
        priority: int = 1,
        pinned: bool = False,
    ) -> None: ...
    @property
    def line_index(self) -> int: ...
    @property
    def start_cleaning_minute(self) -> int | None: ...
    @property
    def start_production_minute(self) -> int | None: ...
    @property
    def end_minute(self) -> int | None: ...

class Line:
    id: str
    name: str
    start_minute: int
    operator_index: int
    jobs: list[int]
    def __init__(
        self,
        id: str,
        name: str,
        start_minute: int,
        operator_index: int = -1,
        jobs: list[int] = ...,
    ) -> None: ...

class PackagingSchedule:
    from_minute: int
    to_minute: int
    products: list[Product]
    cleanup_minutes: list[int]
    operators: list[Operator]
    lines: list[Line]
    jobs: list[Job]
    unassigned_jobs: list[int]
    def __init__(
        self,
        from_minute: int,
        to_minute: int,
        products: list[Product],
        cleanup_minutes: list[int],
        operators: list[Operator],
        lines: list[Line],
        jobs: list[Job],
        unassigned_jobs: list[int] = ...,
    ) -> None: ...
    @property
    def score(self) -> Score: ...
    def line_count(self) -> int: ...
    def cleanup_of(self, previous_product: int, product: int) -> int: ...
    def update_shadow_variables(self) -> None: ...

def calculate_score(schedule: PackagingSchedule) -> Score: ...
def analyze(schedule: PackagingSchedule) -> ConstraintBreakdown: ...
def assign_missing_operators(schedule: PackagingSchedule) -> None: ...
def improve_operator_assignment(schedule: PackagingSchedule) -> bool: ...
def generate_demo_data(line_count: int = 5, job_count: int = 100) -> PackagingSchedule: ...

class SolverConfig:
    spent_limit_seconds: float
    unimproved_spent_limit_seconds: float
    step_count_limit: int
    unimproved_step_count_limit: int
    random_seed: int
    parallel_solver_count: int
    late_acceptance_size: int
    full_assert: bool
    operator_rounds: int
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
    best_score_limit: tuple[int, float]
    on_new_best: Callable[[int, Score], None] | None
    def __init__(self) -> None: ...

class Solver:
    def __init__(self, config: SolverConfig = ...) -> None: ...
    def solve(self, schedule: PackagingSchedule) -> None: ...
    def solve_with_constraints(self, schedule: PackagingSchedule, constraints: Iterable[Any] | None = ...) -> None:
        """같은 solve 를 호출자가 고른 제약 세트로 채점한다
        (food_packaging.constraints 참고). 준 목록이 기본 세트를 대체하고, None 이면
        기본 세트를 그대로 쓴다."""

def list_constraints() -> list[str]:
    """이 도메인이 채점하는 제약 이름들 (기본 조립 순서, snake_case).
    food_packaging.constraints 가 쓰는 이름과 같다."""

def constraint_costs(solution, /) -> dict[str, dict]:
    """제약별 unit 별 **원본** cost — 가중치/exponent/레벨 반올림 전.
    `food_packaging.constraints.score()` 가 이걸 받아 점수를 낸다.
    {name: {"costs": [...], "weight": w, "exponent": e,
            "hard": bool, "global": bool}}."""
