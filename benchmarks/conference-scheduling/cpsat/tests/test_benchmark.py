import sys

from conference_scheduling_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    from optimaru import conference_scheduling
    import external_solver.conference_scheduling

    assert sys.modules["optimaru.conference_scheduling"] is conference_scheduling
    assert callable(external_solver.conference_scheduling.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None
