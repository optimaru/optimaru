from semiconductor_scheduling_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert set(benchmark.instances()) == {"lots6", "lots12"}
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)
    problem = benchmark.instances()["lots6"]()
    assert benchmark.describe(problem) == "6 lots / 6 machines"
