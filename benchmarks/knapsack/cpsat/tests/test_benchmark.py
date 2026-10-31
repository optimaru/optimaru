from knapsack_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert set(benchmark.instances()) == {"n50", "n200", "n1000"}
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_the_engine_reports_its_own_score() -> None:
    problem = benchmark.instances()["n50"]()
    benchmark.solve_engine(problem, 0.2, 1)
    assert benchmark.self_report(problem) == (problem.score_hard, problem.score_soft)
    assert problem.score_hard == 0 and problem.score_soft == problem.total_value
