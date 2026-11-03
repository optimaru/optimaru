import sys

from sports_league_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    import external_solver.sports_league_scheduling
    import sports_league

    assert sys.modules["optimaru.sports_league_scheduling"] is sports_league
    assert callable(external_solver.sports_league_scheduling.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None
