import sys

from sports_league_cpsat import benchmark


def test_external_solver_uses_the_engine_in_this_checkout():
    import external_solver.sports_league_scheduling

    from optimaru import sports_league_scheduling

    assert sys.modules["optimaru.sports_league_scheduling"] is sports_league_scheduling
    assert callable(external_solver.sports_league_scheduling.catalog)


def test_every_instance_builds():
    assert benchmark.instances()
    for build in benchmark.instances().values():
        assert build() is not None


def test_a_rung_is_the_same_instance_every_time() -> None:
    from optimaru.sports_league_scheduling.models import LeagueScheduleModel

    # The generators draw from random.Random(seed) alone: a second build is the same data.
    for name, build in benchmark.instances().items():
        first, second = (LeagueScheduleModel.from_core(build()).model_dump() for _ in range(2))
        assert first == second, name
