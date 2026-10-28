from .common import Constraint, score_costs


class MachineEligibility(Constraint):
    """Missing assignments or assignments to a machine not listed for the route step."""

    name = "machine_eligibility"
    label = "Eligible equipment"
    default_hard = True


class LotRoutePrecedence(Constraint):
    """Release-time and ordered route-step violations, including re-entrant routes."""

    name = "lot_route_precedence"
    label = "Lot route precedence"
    default_hard = True


class MachineCapacity(Constraint):
    """Overlapping processing time on unary equipment."""

    name = "machine_capacity"
    label = "Unary equipment capacity"
    default_hard = True


class BatchCapacity(Constraint):
    """Capacity-time above the simultaneous lot limit of batch equipment."""

    name = "batch_capacity"
    label = "Batch capacity"
    default_hard = True


class BatchFamilyCompatibility(Constraint):
    """Overlap time between incompatible recipe families on batch equipment."""

    name = "batch_family_compatibility"
    label = "Batch recipe compatibility"
    default_hard = True


class SetupTransitions(Constraint):
    """Missing sequence-dependent recipe setup time between consecutive runs."""

    name = "setup_transitions"
    label = "Sequence-dependent setup"
    default_hard = True


class MaintenanceWindows(Constraint):
    """Processing time overlapping preventive-maintenance or downtime windows."""

    name = "maintenance_windows"
    label = "Preventive maintenance windows"
    default_hard = True


class MaximumQueueTime(Constraint):
    """Waiting time above a route step's maximum queue-time limit."""

    name = "maximum_queue_time"
    label = "Maximum queue time"
    default_hard = True


class MinimumQueueTime(Constraint):
    """Missing minimum hold time between consecutive route steps."""

    name = "minimum_queue_time"
    label = "Minimum queue time"
    default_hard = True


class InterToolTransport(Constraint):
    """Missing material-handling time between consecutive equipment groups."""

    name = "inter_tool_transport"
    label = "Inter-tool transport"
    default_hard = True


class AuxiliaryResourceCapacity(Constraint):
    """Concurrent use above an operator, reticle, fixture, or transporter capacity."""

    name = "auxiliary_resource_capacity"
    label = "Auxiliary resource capacity"
    default_hard = True


class AuxiliaryResourceAvailability(Constraint):
    """Required-resource usage overlapping an unavailable window."""

    name = "auxiliary_resource_availability"
    label = "Auxiliary resource availability"
    default_hard = True


class PinnedAssignments(Constraint):
    """Frozen operation assignments changed during replanning."""

    name = "pinned_assignments"
    label = "Pinned assignments"
    default_hard = True


class MaximumCycleTime(Constraint):
    """Lot cycle time above its configured maximum."""

    name = "maximum_cycle_time"
    label = "Maximum cycle time"
    default_hard = True


class MachinePreference(Constraint):
    """Qualification, yield, or operating preference cost of the selected equipment."""

    name = "machine_preference"
    label = "Equipment preference"


class MinimumBatchFill(Constraint):
    """Missing lots below a batch tool's preferred minimum run size."""

    name = "minimum_batch_fill"
    label = "Minimum batch fill"


class MachineLoadBalance(Constraint):
    """Processing-load range between alternative tools in each equipment group."""

    name = "machine_load_balance"
    label = "Equipment-group load balance"


class WeightedTardiness(Constraint):
    """Lot tardiness multiplied by business priority."""

    name = "weighted_tardiness"
    label = "Weighted lot tardiness"
    default_weight = 10.0


class TotalCycleTime(Constraint):
    """Total elapsed time from lot release to completion."""

    name = "total_cycle_time"
    label = "Total cycle time"


class Makespan(Constraint):
    """Completion time of the last operation in the fab schedule."""

    name = "makespan"
    label = "Makespan"


machine_eligibility = MachineEligibility()
lot_route_precedence = LotRoutePrecedence()
machine_capacity = MachineCapacity()
batch_capacity = BatchCapacity()
batch_family_compatibility = BatchFamilyCompatibility()
setup_transitions = SetupTransitions()
maintenance_windows = MaintenanceWindows()
maximum_queue_time = MaximumQueueTime()
minimum_queue_time = MinimumQueueTime()
inter_tool_transport = InterToolTransport()
auxiliary_resource_capacity = AuxiliaryResourceCapacity()
auxiliary_resource_availability = AuxiliaryResourceAvailability()
pinned_assignments = PinnedAssignments()
maximum_cycle_time = MaximumCycleTime()
machine_preference = MachinePreference()
minimum_batch_fill = MinimumBatchFill()
machine_load_balance = MachineLoadBalance()
weighted_tardiness = WeightedTardiness()
total_cycle_time = TotalCycleTime()
makespan = Makespan()

DEFAULTS = [
    machine_eligibility,
    lot_route_precedence,
    machine_capacity,
    batch_capacity,
    batch_family_compatibility,
    setup_transitions,
    maintenance_windows,
    maximum_queue_time,
    minimum_queue_time,
    inter_tool_transport,
    auxiliary_resource_capacity,
    auxiliary_resource_availability,
    pinned_assignments,
    maximum_cycle_time,
    machine_preference,
    minimum_batch_fill,
    machine_load_balance,
    weighted_tardiness,
    total_cycle_time,
    makespan,
]


def score(costs: dict, constraints=None):
    return score_costs(costs, constraints, DEFAULTS)
