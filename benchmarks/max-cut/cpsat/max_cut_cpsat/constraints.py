"""Class-based CP-SAT plugins for the max-cut use case.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``max_cut.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Each one adds exactly the raw cost the engine's component reports — the same
input and partition give the same per-constraint value, and 0 means no
violation (``tests/test_constraint_parity.py``). Every term is an exact function
of the side variables (equalities, not one-sided bounds), so it holds its raw
value in every feasible solution, whatever the objective.

The four unit constraints add one term per side (side 0, then side 1: the
engine's units, a side of raw 0 included), the imbalance and the cut one term
for their total, so a pick's ``offset`` / ``clip`` shape each side (or the
total) exactly as the engine does (``terms_per_unit``), and a constraint picked
several times adds one set of terms per pick.
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


def _both(model: cp_model.CpModel, a, b, name: str):
    """A bool equal to (a and b) for two literals."""
    flag = model.new_bool_var(name)
    model.add_bool_and([a, b]).only_enforce_if(flag)
    model.add_bool_or([a.Not(), b.Not()]).only_enforce_if(flag.Not())
    return flag


def _side_sizes(context: MaxCutContext) -> tuple[Any, Any]:
    one = sum(context.side.values())
    return len(context.problem.vertices) - one, one


class FixedPartitionCost(domain.FixedPartitionCost, ConstraintPlugin[MaxCutContext]):
    """Fixed vertices off their side, charged to the side they sit on (per side)."""

    terms_per_unit = True  # one term per side, like the engine's units

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        fixed = context.problem.fixed_partition
        # Side 0 holds the vertices fixed to 1 that sit on 0, side 1 those fixed to 0 on 1.
        on_zero = [1 - context.side[vertex] for vertex, side in fixed.items() if side == 1]
        on_one = [context.side[vertex] for vertex, side in fixed.items() if side == 0]
        for off in (on_zero, on_one):
            objective.add(self.name, sum(off), bound=len(off))


class SamePartitionPairsCost(domain.SamePartitionPairsCost, ConstraintPlugin[MaxCutContext]):
    """Same-side pairs split across the cut, charged to the first endpoint's side
    (a self-pair never is split)."""

    terms_per_unit = True

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        pairs = context.problem.same_partition_pairs
        per_side: tuple[list, list] = ([], [])
        for index, pair in enumerate(pairs):
            first, second = context.side[pair.first], context.side[pair.second]
            if first is second:
                continue
            # Split with the first endpoint on side 0: first = 0, second = 1; on side 1: the reverse.
            per_side[0].append(_both(context.model, first.Not(), second, f"same_{index}_split_0"))
            per_side[1].append(_both(context.model, first, second.Not(), f"same_{index}_split_1"))
        for split in per_side:
            objective.add(self.name, sum(split), bound=len(pairs))


class DifferentPartitionPairsCost(domain.DifferentPartitionPairsCost, ConstraintPlugin[MaxCutContext]):
    """Different-side pairs with both endpoints on one side, charged to that side
    (a self-pair always is joined, on its vertex's side)."""

    terms_per_unit = True

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        pairs = context.problem.different_partition_pairs
        per_side: tuple[list, list] = ([], [])
        for index, pair in enumerate(pairs):
            first, second = context.side[pair.first], context.side[pair.second]
            if first is second:
                per_side[0].append(1 - first)
                per_side[1].append(first)
                continue
            per_side[0].append(_both(context.model, first.Not(), second.Not(), f"different_{index}_joined_0"))
            per_side[1].append(_both(context.model, first, second, f"different_{index}_joined_1"))
        for joined in per_side:
            objective.add(self.name, sum(joined), bound=len(pairs))


class PartitionSizeBoundsCost(domain.PartitionSizeBoundsCost, ConstraintPlugin[MaxCutContext]):
    """Per side max(0, min - n) + max(0, n - max), n = the side's size."""

    terms_per_unit = True

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        problem = context.problem
        n = len(problem.vertices)
        minimum = 0 if problem.min_partition_size is None else problem.min_partition_size
        maximum = n if problem.max_partition_size is None else problem.max_partition_size
        for index, count in enumerate(_side_sizes(context)):
            under = context.model.new_int_var(0, n, f"side_{index}_under")
            over = context.model.new_int_var(0, n, f"side_{index}_over")
            context.model.add_max_equality(under, [0, minimum - count])
            context.model.add_max_equality(over, [0, count - maximum])
            # One side's violation is at most max(min, n - max) <= n.
            objective.add(self.name, under + over, bound=n)


class PartitionImbalanceCost(domain.PartitionImbalanceCost, ConstraintPlugin[MaxCutContext]):
    """|n0 - n1| (opt-in: weight 0 by default, then the term is skipped)."""

    terms_per_unit = True  # a global: one term, its total

    def build(self, context: MaxCutContext, objective: ObjectiveBuilder) -> None:
        n = len(context.problem.vertices)
        zero, one = _side_sizes(context)
        imbalance = context.model.new_int_var(0, n, "partition_imbalance")
        context.model.add_abs_equality(imbalance, one - zero)
        objective.add(self.name, imbalance, bound=n)


class CutWeightCost(domain.CutWeightCost, ConstraintPlugin[MaxCutContext]):
    """The reward: -(weight of the crossed edges); a self-loop never crosses."""

    terms_per_unit = True

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
