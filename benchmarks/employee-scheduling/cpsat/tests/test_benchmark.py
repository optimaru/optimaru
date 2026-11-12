from cpsat_plugins.benchmark import run_instance
from employee_scheduling_cpsat import benchmark


def test_comparison_runner_owns_its_instances() -> None:
    assert benchmark.instances()
    assert callable(benchmark.solve_engine)
    assert callable(benchmark.solve_cpsat)


def test_instances_are_the_seeded_demo_at_growing_sizes() -> None:
    built = {name: factory() for name, factory in benchmark.instances().items()}

    assert len(built) == 20
    assert [name for name in built if "-" not in name] == ["small", "demo", "medium", "large", "xlarge"]
    shifts = {name: len(item.model_input.shifts) for name, item in built.items()}
    assert {name: shifts[name] for name in ("small", "demo", "medium", "large", "xlarge")} == {
        "small": 21,
        "demo": 84,
        "medium": 336,
        "large": 672,
        "xlarge": 1120,
    }
    # The other instances of a size have its shift count, and are other draws, not copies.
    assert all(count == shifts[name.split("-")[0]] for name, count in shifts.items())
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
