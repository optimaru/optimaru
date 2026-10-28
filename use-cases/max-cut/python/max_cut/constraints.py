from .common import Constraint, score_costs


class FixedPartition(Constraint):
    """Vertices assigned to a side different from their fixed side."""

    name = "fixed_partition"
    label = "Fixed partition"
    default_hard = True


class SamePartitionPairs(Constraint):
    """Pairs that must remain on the same side but are split."""

    name = "same_partition_pairs"
    label = "Same-side pairs"
    default_hard = True


class DifferentPartitionPairs(Constraint):
    """Pairs that must be split but are on the same side."""

    name = "different_partition_pairs"
    label = "Different-side pairs"
    default_hard = True


class PartitionSizeBounds(Constraint):
    """Number of vertices outside the inclusive size bounds on either side."""

    name = "partition_size_bounds"
    label = "Partition size bounds"
    default_hard = True


class PartitionBalance(Constraint):
    """Absolute difference between the two partition sizes."""

    name = "partition_balance"
    label = "Partition balance"
    default_weight = 0.0


class MaximizeCutWeight(Constraint):
    """Negative total weight of edges crossing the bipartition."""

    name = "maximize_cut_weight"
    label = "Maximize cut weight"


fixed_partition = FixedPartition()
same_partition_pairs = SamePartitionPairs()
different_partition_pairs = DifferentPartitionPairs()
partition_size_bounds = PartitionSizeBounds()
partition_balance = PartitionBalance()
maximize_cut_weight = MaximizeCutWeight()
DEFAULTS = [
    fixed_partition,
    same_partition_pairs,
    different_partition_pairs,
    partition_size_bounds,
    partition_balance,
    maximize_cut_weight,
]


def score(costs: dict, constraints=None):
    return score_costs(costs, constraints, DEFAULTS)
