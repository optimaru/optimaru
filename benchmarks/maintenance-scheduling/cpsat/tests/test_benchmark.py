from maintenance_scheduling_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_every_instance_builds() -> None:
    built = {name: build() for name, build in benchmark.instances().items()}
    assert built and all(problem is not None for problem in built.values())
    # Every rung has a size to show beside its name.
    assert all(benchmark.describe(problem) for problem in built.values())


def test_a_rung_is_the_same_instance_every_time() -> None:
    from optimaru.maintenance_scheduling.models import MaintenanceScheduleModel

    # The generators draw from random.Random(seed) alone: a second build is the same data.
    for name, build in benchmark.instances().items():
        first, second = (MaintenanceScheduleModel.from_core(build()).model_dump() for _ in range(2))
        assert first == second, name
