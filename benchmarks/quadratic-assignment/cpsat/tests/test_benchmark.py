from cpsat_plugins.benchmark import score_solution
from quadratic_assignment_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert set(benchmark.instances()) == {"plant6", "random8", "random10"}
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_the_engine_reports_its_own_score() -> None:
    problem = benchmark.instances()["plant6"]()
    benchmark.solve_engine(problem, 0.2, 1)
    assert benchmark.self_report(problem) == (problem.score_hard, problem.score_soft)
    assert problem.score_hard == 0 and problem.score_soft == -problem.total_cost


def test_the_cpsat_answer_rescores_like_the_engine() -> None:
    problem = benchmark.instances()["random8"]()
    solved = benchmark.solve_cpsat(problem, 5.0, 1, 2)
    rescored = score_solution(benchmark.PACKAGE, solved)
    assert rescored.levels == (solved.score_hard, solved.score_soft)
