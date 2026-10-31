from assembly_line_balancing_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)
    problem = benchmark.instances()["small"]()
    assert benchmark.describe(problem) == "20 tasks / cycle 60"
