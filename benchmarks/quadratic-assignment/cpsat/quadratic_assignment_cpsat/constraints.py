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

Offsets (``terms_per_unit``): the three side constraints add one term per
location the engine evaluates (its ``scopedUnits()``: the fixed locations, the
locations named by a forbidden row, the capacitated locations), a term of raw
0 included, so a pick's ``offset`` / ``clip`` shape each location exactly as
the engine does and a literal offset charges exactly those locations. The two
objectives add one term for their total. ``ObjectiveBuilder.add`` shapes a
term over a non-negative range, so an offset on an objective is taken only when
its data cannot make the total negative (no negative assignment cost, or no
negative flow / distance); otherwise CP-SAT refuses it. On such data the
objective cannot be hard either, the engine's sign rule (its
``rawCostCanBeNegative``), refused in the engine's words first. A constraint
picked several times adds one set of terms per pick.
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


def _refuse_a_signed_total(objective: ObjectiveBuilder, name: str, data: str, values) -> None:
    """A total the data can make negative: the engine's sign rule, then CP-SAT's own limit.

    On data with a negative entry (the engine's ``rawCostCanBeNegative``) the
    instance cannot be hard, in the engine's words (``refuse_signed``); and an
    offset on such a total cannot be shaped by ``ObjectiveBuilder.add``.
    """
    values = list(values)
    if not any(value < 0 for value in values):
        return
    objective.refuse_signed(name)
    _spec, config = objective.selected[name]
    if config.offset != 0:
        raise ValueError(
            f"constraint {name!r} offset needs non-negative {data} for CP-SAT (the total could be negative)"
        )


def _span(values) -> int:
    """Width of [min(0, min values), max(0, max values)]: covers every value and 0."""
    values = list(values)
    if not values:
        return 0
    return max(0, max(values)) - min(0, min(values))


class PinnedAssignmentsCost(domain.PinnedAssignmentsCost, ConstraintPlugin[QapContext]):
    """Per fixed location, 1 - x[its facility, location]."""

    terms_per_unit = True  # one term per fixed location, the engine's scope

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        pins = sorted(
            (context.location_index[location], context.facility_index[facility])
            for location, facility in context.problem.fixed_assignments.items()
        )
        for location, facility in pins:
            objective.add(self.name, 1 - context.assigned[facility, location], bound=1)


class ForbiddenAssignmentsCost(domain.ForbiddenAssignmentsCost, ConstraintPlugin[QapContext]):
    """Per location named by a forbidden row, x[f, l] over its DISTINCT forbidden facilities:
    a row listed twice counts once, as in the engine.

    (Before 0.2.0 this summed over the rows, so a repeated row counted twice here
    and once in the metaheuristic scorer.)"""

    terms_per_unit = True

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        forbidden: dict[int, set[int]] = {}
        for row in context.problem.forbidden_assignments:
            forbidden.setdefault(context.location_index[row.location], set()).add(context.facility_index[row.facility])
        for location in sorted(forbidden):
            facilities = sorted(forbidden[location])
            objective.add(self.name, sum(context.assigned[f, location] for f in facilities), bound=len(facilities))


class LocationCapacityCost(domain.LocationCapacityCost, ConstraintPlugin[QapContext]):
    """Per capacitated location, max(0, demand of its facility - capacity) (missing demand = 0)."""

    terms_per_unit = True

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        problem = context.problem
        demands = [problem.facility_demands.get(facility, 0) for facility in problem.facilities]
        most = max(demands, default=0)
        capacities = sorted(
            (context.location_index[location], capacity) for location, capacity in problem.location_capacities.items()
        )
        for index, capacity in capacities:
            demand = sum(demands[f] * context.assigned[f, index] for f in range(context.size))
            # One facility per location: the excess is at most the largest demand.
            excess = context.model.new_int_var(0, most, f"location_{index}_excess")
            context.model.add(excess >= demand - capacity)
            objective.add(self.name, excess, bound=most)


class AssignmentCost(domain.AssignmentCost, ConstraintPlugin[QapContext]):
    """sum c[f][l] * x[f, l]; 0 without an assignment_costs matrix."""

    terms_per_unit = True  # a global: one term, its total

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        costs = context.problem.assignment_costs
        _refuse_a_signed_total(
            objective, self.name, "assignment costs", (value for row in costs or () for value in row)
        )
        if costs is None:
            _add(objective, self.name, [], 0)
            return
        n = context.size
        terms = [costs[f][l] * context.assigned[f, l] for f in range(n) for l in range(n) if costs[f][l]]
        # Each facility contributes exactly one entry of its row.
        _add(objective, self.name, terms, sum(_span(row) for row in costs))


class FlowDistanceCost(domain.FlowDistanceCost, ConstraintPlugin[QapContext]):
    """sum over facility pairs a, b of flows[a][b] * distances[loc a][loc b], diagonal included.

    The diagonal a == b is x[a, l] itself with the coefficient
    flows[a][a] * distances[l][l]. For each unordered pair a < b with a non-zero
    term, one bool y[la, lb] per pair of distinct locations stands for
    "a at la and b at lb"; it carries both directions, flows[a][b] *
    distances[la][lb] + flows[b][a] * distances[lb][la]. Linear channeling
    instead of products (Adams-Johnson style):

        sum over lb of y[la, lb] == x[a, la]   for every la
        sum over la of y[la, lb] == x[b, lb]   for every lb

    On a permutation this forces y[la, lb] == x[a, la] * x[b, lb] exactly, so a
    pinned permutation gives the engine's raw cost. Built only when the
    constraint is selected: O(n^4) bools, O(n^3) linear equalities."""

    terms_per_unit = True

    def build(self, context: QapContext, objective: ObjectiveBuilder) -> None:
        problem = context.problem
        flows, distances = problem.flows, problem.distances
        _refuse_a_signed_total(
            objective,
            self.name,
            "flows and distances",
            (value for matrix in (flows, distances) for row in matrix for value in row),
        )
        n = context.size
        x = context.assigned
        model = context.model
        terms = []
        for a in range(n):
            for l in range(n):
                coefficient = flows[a][a] * distances[l][l]
                if coefficient:
                    terms.append(coefficient * x[a, l])
        sites = [(la, lb) for la in range(n) for lb in range(n) if la != lb]
        for a in range(n):
            for b in range(a + 1, n):
                coefficients = {
                    (la, lb): flows[a][b] * distances[la][lb] + flows[b][a] * distances[lb][la] for la, lb in sites
                }
                if not any(coefficients.values()):
                    continue
                y = {site: model.new_bool_var(f"pair_{a}_{b}_{site[0]}_{site[1]}") for site in sites}
                for l in range(n):
                    model.add(sum(y[l, lb] for lb in range(n) if lb != l) == x[a, l])
                    model.add(sum(y[la, l] for la in range(n) if la != l) == x[b, l])
                terms.extend(coefficient * y[site] for site, coefficient in coefficients.items() if coefficient)
        # Each ordered facility pair (a == b included) contributes exactly one
        # flows[a][b] * distances[la][lb]: its range is within |flow| * the
        # distance span.
        distance_span = _span(value for row in distances for value in row)
        bound = sum(abs(value) for row in flows for value in row) * distance_span
        _add(objective, self.name, terms, bound)


def signed_objectives(problem: QuadraticAssignmentProblemModel) -> dict[str, bool]:
    """Per objective, whether its data holds a negative entry (the engine's ``rawCostCanBeNegative``)."""
    return {
        AssignmentCost.name: any(value < 0 for row in problem.assignment_costs or () for value in row),
        FlowDistanceCost.name: any(
            value < 0 for matrix in (problem.flows, problem.distances) for row in matrix for value in row
        ),
    }


def refuse_signed_objectives(objective: ObjectiveBuilder, problem: QuadraticAssignmentProblemModel) -> None:
    """The engine's sign rule for every selected objective, before any term is built.

    The engine checks it when it resets, in its calculator's order: the first
    instance of each name in the assembly's order, then the repeats in pick
    order. So with two such picks on hard, CP-SAT names the one the engine names.
    """
    signed = signed_objectives(problem)
    order = [spec.name for spec in REGISTRY.specs]

    def rank(item: tuple[int, str]) -> tuple:
        index, key = item
        name, _, repeat = key.partition("#")
        return (bool(repeat), index if repeat else order.index(name))

    for _index, key in sorted(enumerate(objective.selected), key=rank):
        spec, _config = objective.selected[key]
        if signed.get(spec.name):
            objective.refuse_signed(key)


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
