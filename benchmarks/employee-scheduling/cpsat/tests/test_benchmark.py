from cpsat_plugins.benchmark import run_instance

from employee_scheduling_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_instances_are_the_seeded_demo_at_growing_sizes() -> None:
    built = {name: factory() for name, factory in benchmark.instances().items()}

    assert list(built) == ["demo", "medium", "large"]
    assert [len(item.model_input.shifts) for item in built.values()] == [84, 336, 672]
    again = benchmark.instances()["demo"]()
    assert built["demo"].model_dump() == again.model_dump()


def test_both_answers_are_rescored_and_the_engine_crosschecks() -> None:
    result = run_instance(benchmark, "demo", benchmark.instances()["demo"], 2.0, 1, 2)

    engine, cpsat = result.runs
    assert engine.score is not None and cpsat.score is not None
    assert engine.score.names == cpsat.score.names == ("hard", "soft")
    assert engine.self_report is not None
    assert engine.crosscheck == ""
