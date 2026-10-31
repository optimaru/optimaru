"""Class-based CP-SAT plugins for the quadratic-assignment use case.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``quadratic_assignment.constraint_info()`` (the
C++ components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Each one adds exactly the raw cost the engine's component reports — the same
input and permutation give the same per-constraint value, and 0 means no
violation (``tests/test_constraint_parity.py``).

The planning variables are one bool ``x[f, l]`` per facility and location, with
exactly one location per facility and one facility per location (a
permutation, like the engine's swap-only search).

``ObjectiveBuilder`` folds the levels into one objective with the multiplier
``bound + 1`` of the levels below, which is only lexicographic when each term's
bound covers its whole RANGE. Flows, distances and direct costs are not
validated >= 0, so the two objectives pass the width of their value range (with
0 included), not just their largest value.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from quadratic_assignment import constraints as domain
from quadratic_assignment.models import QuadraticAssignmentProblemModel


@dataclass(frozen=True)
class QapContext:
    model: cp_model.CpModel
    problem: QuadraticAssignmentProblemModel
    assigned: dict[tuple[int, int], Any]  # (facility index, location index) -> bool var
    facility_index: dict[str, int]
    location_index: dict[str, int]

    @property
    def size(self) -> int:
        return len(self.problem.facilities)


def _add(objective: ObjectiveBuilder, name: str, terms: list, bound: int) -> None:
    """Add ``sum(terms)``; no terms is an identically-zero penalty (bound 0: nothing is added)."""
    objective.add(name, sum(terms), bound=bound if terms else 0)


def _span(values) -> int:
    """Width of [min(0, min values), max(0, max values)]: covers every value and 0."""
    values = list(values)
    if not values:
        return 0
    return max(0, max(values)) - min(0, min(values))


class PinnedAssignmentsCost(domain.PinnedAssignmentsCost, ConstraintPlugin[QapContext]):
    """Per fixed location, 1 - x[its facility, location]."""

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        pins = [
            (context.facility_index[facility], context.location_index[location])
            for location, facility in context.problem.fixed_assignments.items()
        ]
        _add(objective, self.name, [1 - context.assigned[pin] for pin in pins], len(pins))


class ForbiddenAssignmentsCost(domain.ForbiddenAssignmentsCost, ConstraintPlugin[QapContext]):
    """x[f, l] over the DISTINCT forbidden pairs: a row listed twice counts once, as in the engine.

    (Before 0.2.0 this summed over the rows, so a repeated row counted twice here
    and once in the metaheuristic scorer.)"""

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        pairs = sorted(
            {
                (context.facility_index[row.facility], context.location_index[row.location])
                for row in context.problem.forbidden_assignments
            }
        )
        _add(objective, self.name, [context.assigned[pair] for pair in pairs], len(pairs))


class LocationCapacityCost(domain.LocationCapacityCost, ConstraintPlugin[QapContext]):
    """Per capacitated location, max(0, demand of its facility - capacity) (missing demand = 0)."""

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        problem = context.problem
        demands = [problem.facility_demands.get(facility, 0) for facility in problem.facilities]
        excesses = []
        for location, capacity in problem.location_capacities.items():
            index = context.location_index[location]
            demand = sum(demands[f] * context.assigned[f, index] for f in range(context.size))
            excess = context.model.new_int_var(0, max(demands, default=0), f"location_{index}_excess")
            context.model.add(excess >= demand - capacity)
            excesses.append(excess)
        _add(objective, self.name, excesses, sum(demands))


class AssignmentCost(domain.AssignmentCost, ConstraintPlugin[QapContext]):
    """sum c[f][l] * x[f, l]; 0 without an assignment_costs matrix."""

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        costs = context.problem.assignment_costs
        if costs is None:
            _add(objective, self.name, [], 0)
            return
        n = context.size
        terms = [costs[f][l] * context.assigned[f, l] for f in range(n) for l in range(n) if costs[f][l]]
        # Each facility contributes exactly one entry of its row.
        _add(objective, self.name, terms, sum(_span(row) for row in costs))


class FlowDistanceCost(domain.FlowDistanceCost, ConstraintPlugin[QapContext]):
    """sum over facility pairs a, b of flows[a][b] * distances[loc a][loc b], diagonal included.

    y[a, b, la, lb] = x[a, la] AND x[b, lb] for a != b and la != lb (the only
    combinations a permutation can make); the diagonal a == b is x[a, l] itself
    with the coefficient flows[a][a] * distances[l][l]. Built only when the
    constraint is selected: O(n^4) variables."""

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        problem = context.problem
        flows, distances = problem.flows, problem.distances
        n = context.size
        x = context.assigned
        terms = []
        for a in range(n):
            for l in range(n):
                coefficient = flows[a][a] * distances[l][l]
                if coefficient:
                    terms.append(coefficient * x[a, l])
        for a in range(n):
            for b in range(n):
                if a == b or not flows[a][b]:
                    continue
                for la in range(n):
                    for lb in range(n):
                        coefficient = flows[a][b] * distances[la][lb]
                        if la == lb or not coefficient:
                            continue
                        both = context.model.new_bool_var(f"pair_{a}_{b}_{la}_{lb}")
                        context.model.add_multiplication_equality(both, [x[a, la], x[b, lb]])
                        terms.append(coefficient * both)
        # Each ordered facility pair (a == b included) contributes exactly one
        # flows[a][b] * distances[la][lb]: its range is within |flow| * the
        # distance span.
        distance_span = _span(value for row in distances for value in row)
        bound = sum(abs(value) for row in flows for value in row) * distance_span
        _add(objective, self.name, terms, bound)


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry(
    [
        PinnedAssignmentsCost(),
        ForbiddenAssignmentsCost(),
        LocationCapacityCost(),
        AssignmentCost(),
        FlowDistanceCost(),
    ]
)
