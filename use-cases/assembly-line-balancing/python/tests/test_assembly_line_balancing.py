from assembly_line_balancing import calculate_score, constraint_costs, solve
from assembly_line_balancing.models import (
    AssemblyLineBalancingProblemModel,
    PrecedenceModel,
    TaskGroupModel,
    TaskModel,
    TaskPairModel,
)


def test_manual_constraints_and_meta():
    problem = AssemblyLineBalancingProblemModel(
        cycle_time=5,
        tasks=[TaskModel(id="A", duration=3, station=1), TaskModel(id="B", duration=3, station=0)],
        precedences=[PrecedenceModel(before="A", after="B")],
    )
    assert tuple(calculate_score(problem)) == (-1, -2.0)
    solve(problem, time_limit_seconds=0.05)
    assert problem.score_hard == 0
    assert problem.stations_used == 2


def test_zoning_and_station_domain_costs_match_manual_values():
    problem = AssemblyLineBalancingProblemModel(
        cycle_time=5,
        max_stations=1,
        tasks=[
            TaskModel(id="A", duration=3, station=1, max_station=1, fixed_station=0),
            TaskModel(id="B", duration=3, station=0, forbidden_stations=[0]),
        ],
        precedences=[PrecedenceModel(before="A", after="B")],
        same_station_groups=[TaskGroupModel(id="zone", task_ids=["A", "B"])],
        different_station_pairs=[TaskPairModel(first="A", second="B")],
    )
    costs = constraint_costs(problem)
    assert costs["station_count_limit"] == [1, 0]
    assert costs["fixed_task_stations"] == [1, 0]
    assert costs["forbidden_task_stations"] == [0, 1]
    assert costs["same_station_groups"] == [1]
    assert costs["different_station_pairs"] == [0]
    assert costs["task_precedence"] == [1]
