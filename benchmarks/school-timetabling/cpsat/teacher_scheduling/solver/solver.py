"""문제 입력 -> CP-SAT 모델 -> 편성 결과로 이어지는 실행 진입점."""

from collections.abc import Sequence

from ortools.sat.python import cp_model

from teacher_scheduling.domain.problem import TimetableProblem
from teacher_scheduling.domain.solution import ScheduledLesson, SolveStatus, Timetable
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.objective import PenaltyTerms
from teacher_scheduling.solver.registry import Constraint, build_all, default_constraints, weight_of
from teacher_scheduling.solver.rooms import assign_rooms
from teacher_scheduling.solver.variables import TimetableVariables

_STATUS_BY_CPSAT: dict[int, SolveStatus] = {
    cp_model.OPTIMAL: SolveStatus.OPTIMAL,
    cp_model.FEASIBLE: SolveStatus.FEASIBLE,
    cp_model.INFEASIBLE: SolveStatus.INFEASIBLE,
    cp_model.MODEL_INVALID: SolveStatus.MODEL_INVALID,
    cp_model.UNKNOWN: SolveStatus.UNKNOWN,
}


def solve(
    problem: TimetableProblem,
    config: SolverConfig | None = None,
    *,
    constraints: Sequence[Constraint] | None = None,
) -> Timetable:
    """모델을 만들고(변수 -> 제약) CP-SAT 로 푼 뒤 Timetable 로 변환한다.

    constraints 가 None 이면 기본 구성(하드 H1~H10, 소프트 S1~S6)을
    `config.weights` 가중치로 조립한다. 제약을 직접 주면 그 목록이 전부다 —
    기본 제약을 남기려면 `registry.merge(default_constraints(), ...)` 를 쓴다.
    제약을 완화하거나 커스텀 제약을 주입한 해는 `validate.validate_timetable`
    의 검증 범위 밖이다 (`teacher_scheduling.solver.registry` docstring 참고).
    """
    config = config or SolverConfig()

    model = cp_model.CpModel()
    variables = TimetableVariables(model, problem)
    if constraints is None:
        constraints = default_constraints(config.weights)
    penalties = build_all(model, variables, constraints)
    _minimize(model, penalties, constraints, config)

    solver = _make_solver(config)
    status = _STATUS_BY_CPSAT.get(solver.solve(model), SolveStatus.UNKNOWN)
    if not status.has_solution:
        return Timetable(status=status, wall_time_seconds=solver.wall_time)

    lessons = assign_rooms(problem, _extract_lessons(solver, variables))
    return Timetable(
        status=status,
        lessons=tuple(lessons),
        objective_value=solver.objective_value if model.has_objective() else None,
        best_objective_bound=solver.best_objective_bound if model.has_objective() else None,
        wall_time_seconds=solver.wall_time,
        penalties=_penalty_values(solver, penalties),
    )


def _minimize(
    model: cp_model.CpModel,
    penalties: PenaltyTerms,
    constraints: Sequence[Constraint],
    config: SolverConfig,
) -> None:
    """벌점 식들의 가중합을 최소화한다. 가중치 0 인 항목은 목적함수에서 빠진다.

    벌점 식은 가중치가 0 이어도 `penalties` 에 그대로 남는다 — 목적함수에서
    빼는 것과 결과에서 안 보여주는 것은 다르기 때문이다.
    """
    weights = {constraint.name: weight_of(constraint, config.weights) for constraint in constraints}
    objective = [weights[name] * expr for name, expr in penalties.items() if weights[name]]
    if objective:
        model.minimize(sum(objective))


def _make_solver(config: SolverConfig) -> cp_model.CpSolver:
    solver = cp_model.CpSolver()
    solver.parameters.max_time_in_seconds = config.time_limit_seconds
    solver.parameters.num_workers = config.num_workers
    solver.parameters.random_seed = config.random_seed
    solver.parameters.log_search_progress = config.log_search_progress
    if config.relative_gap_limit is not None:
        solver.parameters.relative_gap_limit = config.relative_gap_limit
    return solver


def _extract_lessons(solver: cp_model.CpSolver, variables: TimetableVariables) -> list[ScheduledLesson]:
    """켜진 x 변수를 시간표 칸으로 옮긴다. 특별실 배정은 이후 단계."""
    lessons = []
    for course in variables.problem.courses:
        for slot in variables.allowed_slots[course.id]:
            if solver.value(variables.x[course.id, slot]):
                lessons.append(
                    ScheduledLesson(
                        course_id=course.id,
                        class_group_id=course.class_group_id,
                        subject_id=course.subject_id,
                        teacher_ids=course.teacher_ids,
                        slot=slot,
                    )
                )
    return lessons


def _penalty_values(solver: cp_model.CpSolver, penalties: PenaltyTerms) -> dict[str, int]:
    return {name: int(solver.value(expr)) for name, expr in penalties.items()}
