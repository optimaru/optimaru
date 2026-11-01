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
    from assembly_line_balancing.constraints import (
        FixedTaskStationsCost,
        ForbiddenTaskStationsCost,
        SameStationGroupsCost,
    )

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
        constraints=[
            FixedTaskStationsCost(),
            SameStationGroupsCost(level="soft", weight=2),
            ForbiddenTaskStationsCost(level="soft"),
        ],
    )
    assert [task.station for task in result.tasks] == [0, 1, 1]
    assert result.score_soft == -2.0


def test_a_forbidden_station_listed_twice_counts_once():
    """A on its forbidden 0 costs 2 * 1 (not 2 * 2): cheaper than splitting the group (3)."""
    from assembly_line_balancing.constraints import (
        FixedTaskStationsCost,
        ForbiddenTaskStationsCost,
        SameStationGroupsCost,
    )

    result = solve(
        AssemblyLineBalancingProblemModel(
            cycle_time=12,
            tasks=[
                TaskModel(id="A", duration=6, forbidden_stations=[0, 0]),
                TaskModel(id="B", duration=6, fixed_station=0),
            ],
            same_station_groups=[TaskGroupModel(id="g", task_ids=["A", "B"])],
        ),
        constraints=[
            FixedTaskStationsCost(),
            SameStationGroupsCost(level="soft", weight=3),
            ForbiddenTaskStationsCost(level="soft", weight=2),
        ],
    )
    assert [task.station for task in result.tasks] == [0, 0]
    assert result.score_soft == -2.0


def test_station_domains_do_not_get_a_station_prefix_forced_on_them():
    """Only a plain line may force the used stations to be 0..k-1: B's min_station 1 must not open station 0
    (A and B share station 1)."""
    from assembly_line_balancing.constraints import TaskStationBoundsCost, UsedStationsCost

    result = solve(
        AssemblyLineBalancingProblemModel(
            cycle_time=10, tasks=[TaskModel(id="A", duration=3), TaskModel(id="B", duration=3, min_station=1)]
        ),
        constraints=[TaskStationBoundsCost(), UsedStationsCost()],
    )
    assert (result.score_hard, result.score_soft) == (0, -1.0)
    assert [task.station for task in result.tasks] == [1, 1]
