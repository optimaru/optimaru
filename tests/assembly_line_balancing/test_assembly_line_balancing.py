from optimaru.assembly_line_balancing import calculate_score, constraint_costs, solve
from optimaru.assembly_line_balancing.models import (
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
    score = calculate_score(problem)
    assert (score.hard, score.soft) == (-1, -2.0)
    assert (problem.score_hard, problem.score_soft) == (-1, -2.0)
    assert problem.station_loads == [3, 3]
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
    costs = {name: entry["costs"] for name, entry in constraint_costs(problem).items()}
    # Per station (0, 1) for the per-station constraints, one total for the globals.
    assert costs["max_stations_cost"] == [0, 1]  # A on station 1 with max_stations 1
    assert costs["fixed_task_stations_cost"] == [0, 1]  # A off its fixed station 0
    assert costs["forbidden_task_stations_cost"] == [1, 0]  # B on its forbidden station 0
    assert costs["same_station_groups_cost"] == [1]
    assert costs["different_station_pairs_cost"] == [0, 0]
    assert costs["task_precedence_cost"] == [1]
    assert {name: sum(values) for name, values in costs.items()} == {
        "station_capacity_cost": 0,
        "task_precedence_cost": 1,
        "max_stations_cost": 1,
        "task_station_bounds_cost": 0,
        "fixed_task_stations_cost": 1,
        "forbidden_task_stations_cost": 1,
        "same_station_groups_cost": 1,
        "different_station_pairs_cost": 0,
        "used_stations_cost": 2,
        "workload_imbalance_cost": 0,
    }
