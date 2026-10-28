"""Task assigning (태스크 배정) — Timefold engine 기반 C++ 솔버의 Python 래퍼.

`Customer`/`TaskType`/`Task`/`Employee`/`TaskAssigningSolution`/`Score`/
`Solver`/`SolverConfig` 와 `calculate_score` 는 컴파일된 C++ 코어(`_core`)에서
온다. planning variable 은 각 `Employee.tasks` — 그 직원이 연달아 수행할
task 인덱스의 순서 있는 리스트다 (Java 원본의
`@PlanningListVariable(allowsUnassignedValues = true)`). 어느 리스트에도
없는 task 는 미배정이다.

shadow variable 은 Java 원본과 같은 규칙으로 유도된다
(`calculate_score`/`Solver.solve` 가 다시 계산):

    task.employee    == 리스트 소유 직원 인덱스 (-1 == 미배정)
    task.start_time  == 첫 task 는 자신의 min_start_time,
                        이후 task 는 max(이전 task 의 end, min_start_time)
    duration         == base_duration x affinity 배수 (HIGH 1 / MEDIUM 2 /
                        LOW 3 / NONE 4)

점수 규약 (BendableScore [1]hard/[3]soft, Java quickstart 그대로; 전부 벌점):
  hard[0]  noMissingSkills              직원에게 없는 필수 스킬 수
  soft[0]  minimizeUnassignedTasks      미배정 task 1건당 1
  soft[1]  minimizeMakespan             직원별 endTime^2
  soft[2]  {critical|major|minor}PriorityTaskEndTime
                                        endTime x 4 / x 2 / x 1
엔진 내부적으로는 HardSoftScore(int64, double)에
`engine.hard = hard * 10**9 + soft0`, `engine.soft = soft1 * 10**7 + soft2`
로 매핑된다 (레벨 순서 보존; 자세한 규칙은 cpp/include/ta/domain.hpp).

고수준 진입점은 `solve` 다::

    import task_assigning as ta
    from task_assigning.demo import generate_demo_data

    solution = generate_demo_data()      # models.TaskAssigningSolutionModel
    result = ta.solve(solution, time_limit_seconds=10.0, random_seed=42)
    print(result.score_hard, result.score_soft0, result.score_soft1, result.score_soft2)

pydantic 미러(직렬화용)는 `task_assigning.models` 에 있다 (선택 의존성).
"""

from ._core import (
    Affinity,
    Customer,
    Employee,
    Priority,
    Score,
    Solver,
    SolverConfig,
    Task,
    TaskAssigningSolution,
    TaskType,
    calculate_score,
    constraint_costs,
    list_constraints,
)

__all__ = [
    "Affinity",
    "Customer",
    "Employee",
    "Priority",
    "Score",
    "Solver",
    "SolverConfig",
    "Task",
    "TaskAssigningSolution",
    "TaskType",
    "calculate_score",
    "solve",
    "list_constraints",
    "constraint_costs",
]


def _build_config(
    *,
    time_limit_seconds: float | None,
    random_seed: int | None,
    parallel_solver_count: int | None,
    stop_at_first_feasible: bool,
    full_assert: bool,
    on_new_best,
    config,
) -> SolverConfig:
    cfg = config if config is not None else SolverConfig()
    if time_limit_seconds is not None:
        cfg.spent_limit_seconds = time_limit_seconds
    if random_seed is not None:
        cfg.random_seed = random_seed
    if parallel_solver_count is not None:
        cfg.parallel_solver_count = parallel_solver_count
    if stop_at_first_feasible:
        # hard 0 + 전 태스크 배정 도달 즉시 종료 (soft1/soft2 는 사실상
        # -inf 하한).
        cfg.best_score_limit = (0, 0, -(2**62), -(2**62))
    if full_assert:
        cfg.full_assert = True
    if on_new_best is not None:
        cfg.on_new_best = on_new_best
    return cfg


def solve(
    solution,
    *,
    time_limit_seconds: float | None = None,
    random_seed: int | None = None,
    parallel_solver_count: int | None = None,
    stop_at_first_feasible: bool = False,
    full_assert: bool = False,
    on_new_best=None,
    config=None,
    constraints=None,
):
    """태스크 배정 문제를 풀어 같은 형태로 돌려주는 고수준 API.

    ``solution`` 은 `_core.TaskAssigningSolution` (in place 로 풀고 그대로
    반환) 또는 `models.TaskAssigningSolutionModel` (엔진 형태로 변환해 풀고
    결과를 새 `TaskAssigningSolutionModel` 로 반환; pydantic 필요) 를 받는다.

    ``config`` 가 기본 설정이 된다 (`SolverConfig`; None 이면 엔진 기본값
    30초 예산). 명시한 키워드 인자가 config 위에 덮어쓴다.
    ``stop_at_first_feasible`` 은 hard 0 (미배정 0 포함) 에 도달하는 즉시
    종료한다. ``on_new_best`` 는 새 best 마다 ``(step, Score)`` 로 불린다.

    ``constraints`` 로 비용 함수를 직접 고를 수 있다
    (`task_assigning.constraints` 참고). 준 목록의 제약만 채점되고, None
    이면 기본 여섯 제약 전부를 쓴다::

        from task_assigning.constraints import get_constraints
        ta.solve(solution, constraints=get_constraints())
    """
    cfg = _build_config(
        time_limit_seconds=time_limit_seconds,
        random_seed=random_seed,
        parallel_solver_count=parallel_solver_count,
        stop_at_first_feasible=stop_at_first_feasible,
        full_assert=full_assert,
        on_new_best=on_new_best,
        config=config,
    )

    def _solve(core_solution):
        if constraints is None:
            Solver(cfg).solve(core_solution)
        else:
            Solver(cfg).solve_with_constraints(core_solution, constraints)

    if isinstance(solution, TaskAssigningSolution):
        _solve(solution)
        return solution

    # pydantic 모델 경로 (지연 임포트: pydantic 은 선택 의존성).
    from .models import TaskAssigningSolutionModel

    if not isinstance(solution, TaskAssigningSolutionModel):
        raise TypeError(
            f"solution 은 TaskAssigningSolution 또는 models.TaskAssigningSolutionModel 이어야 한다: {type(solution)!r}"
        )
    core_solution = solution.to_core()
    _solve(core_solution)
    return TaskAssigningSolutionModel.from_core(core_solution)
