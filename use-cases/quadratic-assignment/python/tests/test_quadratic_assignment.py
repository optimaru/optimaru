from quadratic_assignment import calculate_score, constraint_costs, solve
from quadratic_assignment.models import ForbiddenAssignmentModel, QuadraticAssignmentProblemModel


def test_manual_two_facilities():
    problem = QuadraticAssignmentProblemModel(
        facilities=["F1", "F2"],
        locations=["L1", "L2"],
        flows=[[0, 3], [2, 0]],
        distances=[[0, 4], [5, 0]],
        assignment={"L1": "F1", "L2": "F2"},
    )
    assert tuple(calculate_score(problem)) == (0, -22.0)
    solve(problem, time_limit_seconds=0.05)
    assert problem.total_cost == 22


def test_assignment_side_constraints_match_manual_values():
    problem = QuadraticAssignmentProblemModel(
        facilities=["F1", "F2"],
        locations=["L1", "L2"],
        flows=[[0, 3], [2, 0]],
        distances=[[0, 4], [5, 0]],
        assignment_costs=[[7, 1], [2, 8]],
        assignment={"L1": "F1", "L2": "F2"},
        fixed_assignments={"L1": "F2"},
        forbidden_assignments=[ForbiddenAssignmentModel(facility="F2", location="L2")],
        facility_demands={"F1": 5, "F2": 2},
        location_capacities={"L1": 3, "L2": 10},
    )
    costs = constraint_costs(problem)
    assert costs["fixed_assignments"] == [1]
    assert costs["forbidden_assignments"] == [0, 1]
    assert costs["location_capacities"] == [2, 0]
    assert costs["minimize_assignment_cost"] == [15]
    assert costs["minimize_flow_distance"] == [22]
