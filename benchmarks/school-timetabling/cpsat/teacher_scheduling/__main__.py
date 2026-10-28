"""CLI that solves the demo school and prints a timetable report.

python -m teacher_scheduling [--time-limit N] [--backend cpsat]
"""

import argparse

from teacher_scheduling.report import render_report
from teacher_scheduling.sample import build_sample_problem
from teacher_scheduling.solver.config import SolverConfig
from teacher_scheduling.solver.solver import solve
from teacher_scheduling.validate import validate_timetable


def main() -> int:
    parser = argparse.ArgumentParser(prog="python -m teacher_scheduling", description="Demo school timetabling")
    parser.add_argument("--time-limit", type=float, default=None, metavar="N", help="search time limit (seconds)")
    parser.add_argument("--backend", choices=["cpsat"], default="cpsat", help="solver backend to use")
    args = parser.parse_args()

    config = SolverConfig() if args.time_limit is None else SolverConfig(time_limit_seconds=args.time_limit)
    problem = build_sample_problem()
    timetable = solve(problem, config)

    print(render_report(problem, timetable, config.weights))
    if not timetable.is_solved:
        return 1

    violations = validate_timetable(problem, timetable)
    if violations:
        print("\nhard-constraint violations:")
        for violation in violations:
            print(f"  - {violation}")
        return 1
    print(f"\nno hard-constraint violations ({len(timetable.lessons)} lesson cells checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
