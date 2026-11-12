from knapsack_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    names = list(benchmark.instances())
    assert len(names) == 20
    # One name per size, and the other draws of a size numbered after it.
    assert [name for name in names if "-" not in name] == ["n50", "n100", "n200", "n500", "n1000", "n2000", "n5000"]
    assert names[:4] == ["n50", "n50-2", "n50-3", "n100"]
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_the_engine_reports_its_own_score() -> None:
    problem = benchmark.instances()["n50"]()
    benchmark.solve_engine(problem, 0.2, 1)
    assert benchmark.self_report(problem) == (problem.score_hard, problem.score_soft)
    assert problem.score_hard == 0 and problem.score_soft == problem.total_value


def test_every_instance_builds() -> None:
    built = {name: build() for name, build in benchmark.instances().items()}
    assert built and all(problem is not None for problem in built.values())
    # Every rung has a size to show beside its name.
    assert all(benchmark.describe(problem) for problem in built.values())
    # Another draw of a size is another instance.
    assert built["n50"].model_dump() != built["n50-2"].model_dump()
