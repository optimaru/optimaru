from assembly_line_balancing.models import (
    AssemblyLineBalancingProblemModel,
    PrecedenceModel,
    TaskGroupModel,
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


def test_a_split_group_costs_its_distinct_stations_minus_one():
    """Stations [0, 1, 1] cost 1 (the Python scorer), not 2 (members off the first member's station)."""
    from assembly_line_balancing.constraints import FixedTaskStations, ForbiddenTaskStations, SameStationGroups

    result = solve(
        AssemblyLineBalancingProblemModel(
            cycle_time=10,
            tasks=[
                TaskModel(id="A", duration=1, fixed_station=0),
                TaskModel(id="B", duration=1, fixed_station=1),
                TaskModel(id="C", duration=1, forbidden_stations=[0]),
            ],
            same_station_groups=[TaskGroupModel(id="g", task_ids=["A", "B", "C"])],
        ),
        constraints=[FixedTaskStations(), SameStationGroups(hard=False, weight=2), ForbiddenTaskStations(hard=False)],
    )
    assert [task.station for task in result.tasks] == [0, 1, 1]
    assert result.score_soft == -2.0


def test_a_forbidden_station_listed_twice_counts_once():
    from assembly_line_balancing.constraints import ForbiddenTaskStations, MinimizeStations, StationCapacity

    result = solve(
        AssemblyLineBalancingProblemModel(
            cycle_time=12,
            tasks=[TaskModel(id="A", duration=6, forbidden_stations=[0, 0]), TaskModel(id="B", duration=6)],
        ),
        constraints=[StationCapacity(), MinimizeStations(weight=3), ForbiddenTaskStations(hard=False, weight=2)],
    )
    assert [task.station for task in result.tasks] == [0, 0]
    assert result.score_soft == -5.0
