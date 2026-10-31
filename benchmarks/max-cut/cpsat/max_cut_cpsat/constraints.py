"""Class-based CP-SAT plugins for the max-cut use case.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``max_cut.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Each one adds exactly the raw cost the engine's component reports, summed over
the two sides for the four unit constraints — the same input and partition give
the same per-constraint value, and 0 means no violation
(``tests/test_constraint_parity.py``). Every term is an exact function of the
side variables (equalities, not one-sided bounds), so it holds its raw value in
every feasible solution, whatever the objective.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from max_cut import constraints as domain
from max_cut.models import MaxCutProblemModel
from ortools.sat.python import cp_model


@dataclass(frozen=True)
class MaxCutContext:
    model: cp_model.CpModel
    problem: MaxCutProblemModel
    side: dict[str, Any]  # one bool var per vertex: 1 = side 1, 0 = side 0
    crossed: list[Any]  # one bool var per edge: its endpoints are on different sides


def _differs(model: cp_model.CpModel, a, b, name: str):
    """A bool equal to (a != b) for two bool vars (a self-pair gives the constant 0)."""
    if a is b:
        return 0
    flag = model.new_bool_var(name)
    model.add(a != b).only_enforce_if(flag)
    model.add(a == b).only_enforce_if(flag.Not())
    return flag


def _side_sizes(context: MaxCutContext) -> tuple[Any, Any]:
    one = sum(context.side.values())
    return len(context.problem.vertices) - one, one


class FixedPartitionCost(domain.FixedPartitionCost, ConstraintPlugin[MaxCutContext]):
    """Fixed vertices off their side (one per vertex; the engine reports them per side)."""

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        fixed = context.problem.fixed_partition
        off = [context.side[vertex] if side == 0 else 1 - context.side[vertex] for vertex, side in fixed.items()]
        objective.add(self.name, sum(off), bound=len(off))


class SamePartitionPairsCost(domain.SamePartitionPairsCost, ConstraintPlugin[MaxCutContext]):
    """Same-side pairs split across the cut (a self-pair never is)."""

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        pairs = context.problem.same_partition_pairs
        split = [
            _differs(context.model, context.side[pair.first], context.side[pair.second], f"same_{index}_split")
            for index, pair in enumerate(pairs)
        ]
        objective.add(self.name, sum(split), bound=len(pairs))


class DifferentPartitionPairsCost(domain.DifferentPartitionPairsCost, ConstraintPlugin[MaxCutContext]):
    """Different-side pairs left on one side (a self-pair always is)."""

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        pairs = context.problem.different_partition_pairs
        joined = [
            1 - _differs(context.model, context.side[pair.first], context.side[pair.second], f"different_{index}_split")
            for index, pair in enumerate(pairs)
        ]
        objective.add(self.name, sum(joined), bound=len(pairs))


class PartitionSizeBoundsCost(domain.PartitionSizeBoundsCost, ConstraintPlugin[MaxCutContext]):
    """Per side max(0, min - n) + max(0, n - max), n = the side's size."""

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        problem = context.problem
        n = len(problem.vertices)
        minimum = 0 if problem.min_partition_size is None else problem.min_partition_size
        maximum = n if problem.max_partition_size is None else problem.max_partition_size
        violations = []
        for index, count in enumerate(_side_sizes(context)):
            under = context.model.new_int_var(0, n, f"side_{index}_under")
            over = context.model.new_int_var(0, n, f"side_{index}_over")
            context.model.add_max_equality(under, [0, minimum - count])
            context.model.add_max_equality(over, [0, count - maximum])
            violations.extend((under, over))
        # One side's violation is at most max(min, n - max) <= n.
        objective.add(self.name, sum(violations), bound=2 * n)


class PartitionImbalanceCost(domain.PartitionImbalanceCost, ConstraintPlugin[MaxCutContext]):
    """|n0 - n1| (opt-in: weight 0 by default, then the term is skipped)."""

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        n = len(context.problem.vertices)
        zero, one = _side_sizes(context)
        imbalance = context.model.new_int_var(0, n, "partition_imbalance")
        context.model.add_abs_equality(imbalance, one - zero)
        objective.add(self.name, imbalance, bound=n)


class CutWeightCost(domain.CutWeightCost, ConstraintPlugin[MaxCutContext]):
    """The reward: -(weight of the crossed edges); a self-loop never crosses."""

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        edges = context.problem.edges
        cut = sum(edge.weight * crossed for edge, crossed in zip(edges, context.crossed, strict=True))
        objective.add_reward(self.name, cut, bound=sum(edge.weight for edge in edges))


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry(
    [
        FixedPartitionCost(),
        SamePartitionPairsCost(),
        DifferentPartitionPairsCost(),
        PartitionSizeBoundsCost(),
        PartitionImbalanceCost(),
        CutWeightCost(),
    ]
)
