from quadratic_assignment.models import ForbiddenAssignmentModel, QuadraticAssignmentProblemModel

from quadratic_assignment_cpsat import solve


def test_exact_matches_manual_cost():
    result = solve(
        QuadraticAssignmentProblemModel(
            facilities=["F1", "F2"],
            locations=["L1", "L2"],
            flows=[[0, 3], [2, 0]],
            distances=[[0, 4], [5, 0]],
        )
    )
    assert (result.total_cost, result.score_soft) == (22, -22.0)


def test_exact_respects_fixed_forbidden_and_capacity_contracts():
    result = solve(
        QuadraticAssignmentProblemModel(
            facilities=["F1", "F2"],
            locations=["L1", "L2"],
            flows=[[0, 1], [1, 0]],
            distances=[[0, 1], [1, 0]],
            assignment_costs=[[100, 1], [1, 100]],
            fixed_assignments={"L1": "F2"},
            forbidden_assignments=[ForbiddenAssignmentModel(facility="F1", location="L1")],
            facility_demands={"F1": 5, "F2": 2},
            location_capacities={"L1": 2, "L2": 5},
        )
    )
    assert result.assignment == {"L1": "F2", "L2": "F1"}
    assert result.score_hard == 0
