from max_cut_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert set(benchmark.instances()) == {"g30", "g80", "g200"}
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_the_engine_reports_its_own_score() -> None:
    problem = benchmark.instances()["g30"]()
    benchmark.solve_engine(problem, 0.2, 1)
    assert benchmark.self_report(problem) == (problem.score_hard, problem.score_soft)
    assert problem.score_hard == 0 and problem.score_soft == problem.cut_weight


def test_both_answers_rescore_with_the_engine_evaluator() -> None:
    from cpsat_plugins.benchmark import score_solution

    problem = benchmark.instances()["g30"]()
    answer = benchmark.solve_cpsat(problem, 2.0, 1, 2)
    rescored = score_solution(benchmark.PACKAGE, answer)
    assert rescored.levels == (answer.score_hard, answer.score_soft) == (0, float(answer.cut_weight))
