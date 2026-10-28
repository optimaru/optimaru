from assembly_line_balancing.models import (
    AssemblyLineBalancingProblemModel,
    PrecedenceModel,
    TaskModel,
    TaskPairModel,
)

from assembly_line_balancing_cpsat import solve


def test_exact_matches_manual_lower_bound():
    result = solve(
        AssemblyLineBalancingProblemModel(
            cycle_time=5,
            tasks=[TaskModel(id="A", duration=3), TaskModel(id="B", duration=2), TaskModel(id="C", duration=3)],
            precedences=[PrecedenceModel(before="A", after="C")],
        )
    )
    assert result.score_hard == 0
    assert result.stations_used == 2


def test_exact_respects_station_domains_and_separation_zoning():
    result = solve(
        AssemblyLineBalancingProblemModel(
            cycle_time=5,
            max_stations=2,
            tasks=[
                TaskModel(id="A", duration=3, fixed_station=0),
                TaskModel(id="B", duration=3, min_station=1, max_station=1),
            ],
            precedences=[PrecedenceModel(before="A", after="B")],
            different_station_pairs=[TaskPairModel(first="A", second="B")],
        )
    )
    assert [task.station for task in result.tasks] == [0, 1]
    assert result.score_hard == 0
