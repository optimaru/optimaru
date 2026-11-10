from semiconductor_scheduling_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert list(benchmark.instances()) == ["lots6", "lots12", "lots18", "lots24", "lots36", "lots48"]
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)
    problem = benchmark.instances()["lots6"]()
    assert benchmark.describe(problem) == "6 lots / 6 machines"


def test_every_instance_builds() -> None:
    built = {name: build() for name, build in benchmark.instances().items()}
    assert built and all(problem is not None for problem in built.values())
    # Every rung has a size to show beside its name.
    assert all(benchmark.describe(problem) for problem in built.values())
