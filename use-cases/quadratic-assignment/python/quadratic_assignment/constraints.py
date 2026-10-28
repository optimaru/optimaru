from .common import Constraint, score_costs


class FixedAssignments(Constraint):
    """Fixed location assignments that are not respected."""

    name = "fixed_assignments"
    label = "Fixed assignments"
    default_hard = True


class ForbiddenAssignments(Constraint):
    """Assignments using a forbidden facility-location pair."""

    name = "forbidden_assignments"
    label = "Forbidden assignments"
    default_hard = True


class LocationCapacities(Constraint):
    """Assigned facility demand exceeding its location capacity."""

    name = "location_capacities"
    label = "Location capacities"
    default_hard = True


class MinimizeAssignmentCost(Constraint):
    """Direct facility-to-location assignment cost."""

    name = "minimize_assignment_cost"
    label = "Minimize assignment cost"


class MinimizeFlowDistance(Constraint):
    """Sum of facility flow multiplied by assigned-location distance."""

    name = "minimize_flow_distance"
    label = "Minimize flow-distance cost"


fixed_assignments = FixedAssignments()
forbidden_assignments = ForbiddenAssignments()
location_capacities = LocationCapacities()
minimize_assignment_cost = MinimizeAssignmentCost()
minimize_flow_distance = MinimizeFlowDistance()
DEFAULTS = [
    fixed_assignments,
    forbidden_assignments,
    location_capacities,
    minimize_assignment_cost,
    minimize_flow_distance,
]


def score(costs: dict, constraints=None):
    return score_costs(costs, constraints, DEFAULTS)
