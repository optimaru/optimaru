import sys

from meeting_scheduling_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    import external_solver.meeting_scheduling

    from optimaru import meeting_scheduling

    assert sys.modules["optimaru.meeting_scheduling"] is meeting_scheduling
    assert callable(external_solver.meeting_scheduling.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None
