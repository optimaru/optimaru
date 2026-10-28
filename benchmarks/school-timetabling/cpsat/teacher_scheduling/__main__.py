"""데모 학교 문제를 풀어 시간표 리포트를 출력하는 CLI.

python -m teacher_scheduling [--time-limit N] [--backend cpsat]
"""

import argparse

from teacher_scheduling.report import render_report
from teacher_scheduling.sample import build_sample_problem
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.solver import solve
from teacher_scheduling.validate import validate_timetable


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m teacher_scheduling", description="데모 학교 시간표 편성")
    parser.add_argument("--time-limit", type=float, default=None, metavar="N", help="탐색 제한 시간(초)")
    parser.add_argument("--backend", choices=["cpsat"], default="cpsat", help="사용할 솔버 백엔드")
    args = parser.parse_args()

    config = SolverConfig() if args.time_limit is None else SolverConfig(time_limit_seconds=args.time_limit)
    problem = build_sample_problem()
    timetable = solve(problem, config)

    print(render_report(problem, timetable, config.weights))
    if not timetable.is_solved:
        return 1

    violations = validate_timetable(problem, timetable)
    if violations:
        print("\n하드 제약 위반:")
        for violation in violations:
            print(f"  - {violation}")
        return 1
    print(f"\n하드 제약 위반 없음 (수업 {len(timetable.lessons)}칸 검증)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
