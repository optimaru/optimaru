import sys

from task_assigning_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    import external_solver.task_assigning

    from optimaru import task_assigning

    assert sys.modules["optimaru.task_assigning"] is task_assigning
    assert callable(external_solver.task_assigning.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None


def test_a_rung_is_the_same_instance_every_time() -> None:
    # The generators draw from random.Random(seed) alone, so a second build describes the same.
    for name, build in benchmark.instances().items():
        assert benchmark.describe(build()) == benchmark.describe(build()), name
