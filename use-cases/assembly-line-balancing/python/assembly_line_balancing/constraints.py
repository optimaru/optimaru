from .common import Constraint, score_costs


class StationCapacity(Constraint):
    """Sum of processing time above the cycle time across stations."""

    name = "station_capacity"
    label = "Station capacity"
    default_hard = True


class TaskPrecedence(Constraint):
    """Number of precedence arcs assigned in reverse station order."""

    name = "task_precedence"
    label = "Task precedence"
    default_hard = True


class StationCountLimit(Constraint):
    """Task station indices beyond the configured maximum station count."""

    name = "station_count_limit"
    label = "Station count limit"
    default_hard = True


class TaskStationBounds(Constraint):
    """Task assignments outside their minimum and maximum station bounds."""

    name = "task_station_bounds"
    label = "Task station bounds"
    default_hard = True


class FixedTaskStations(Constraint):
    """Tasks not assigned to their fixed station."""

    name = "fixed_task_stations"
    label = "Fixed task stations"
    default_hard = True


class ForbiddenTaskStations(Constraint):
    """Tasks assigned to a forbidden station."""

    name = "forbidden_task_stations"
    label = "Forbidden task stations"
    default_hard = True


class SameStationGroups(Constraint):
    """Additional stations used by tasks that must be colocated."""

    name = "same_station_groups"
    label = "Same-station zoning"
    default_hard = True


class DifferentStationPairs(Constraint):
    """Task pairs assigned to the same station despite separation zoning."""

    name = "different_station_pairs"
    label = "Different-station zoning"
    default_hard = True


class MinimizeStations(Constraint):
    """Number of non-empty workstations."""

    name = "minimize_stations"
    label = "Minimize stations"


class WorkloadBalance(Constraint):
    """Range between the most- and least-loaded used stations."""

    name = "workload_balance"
    label = "Workload balance"
    default_weight = 0.0


station_capacity = StationCapacity()
task_precedence = TaskPrecedence()
station_count_limit = StationCountLimit()
task_station_bounds = TaskStationBounds()
fixed_task_stations = FixedTaskStations()
forbidden_task_stations = ForbiddenTaskStations()
same_station_groups = SameStationGroups()
different_station_pairs = DifferentStationPairs()
minimize_stations = MinimizeStations()
workload_balance = WorkloadBalance()
DEFAULTS = [
    station_capacity,
    task_precedence,
    station_count_limit,
    task_station_bounds,
    fixed_task_stations,
    forbidden_task_stations,
    same_station_groups,
    different_station_pairs,
    minimize_stations,
    workload_balance,
]


def score(costs: dict, constraints=None):
    return score_costs(costs, constraints, DEFAULTS)
