from cpsat_plugins.benchmark import run_instance
from employee_scheduling_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_instances_are_the_seeded_demo_at_growing_sizes() -> None:
    built = {name: factory() for name, factory in benchmark.instances().items()}

    assert list(built) == ["small", "demo", "medium", "medium-2", "large", "xlarge"]
    assert [len(item.model_input.shifts) for item in built.values()] == [21, 84, 336, 336, 672, 1120]
    # The second medium instance is another draw, not a copy.
    assert built["medium"].model_dump() != built["medium-2"].model_dump()
    again = benchmark.instances()["demo"]()
    assert built["demo"].model_dump() == again.model_dump()


def test_both_answers_are_rescored_and_the_engine_crosschecks() -> None:
    # The smallest instance: CP-SAT has an answer well inside two seconds on a slow runner too.
    result = run_instance(benchmark, "small", benchmark.instances()["small"], 2.0, 1, 2)

    engine, cpsat = result.runs
    assert engine.score is not None and cpsat.score is not None
    assert engine.score.names == cpsat.score.names == ("hard", "soft")
    assert engine.self_report is not None
    assert engine.crosscheck == ""
