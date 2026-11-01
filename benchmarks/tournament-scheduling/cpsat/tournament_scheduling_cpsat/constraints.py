"""Class-based CP-SAT plugins for tournament scheduling.

Each plugin subclasses the domain spec of the same name and only adds
``build()``: its default level and weight come from the engine's
``constraint_info()`` (cpsat-common reads the row), so the two solvers share
one set of defaults.

Raw-cost parity with the engine (``tournament_scheduling.constraint_costs``):

- ``team_conflict_cost`` and ``team_unavailability_cost`` add exactly the
  engine's raw cost to the objective (one term per same-day assignment pair of
  a team, one per violated unavailability fact).
- ``assignment_count_imbalance_cost`` and ``confrontation_count_imbalance_cost``
  are NOT raw-identical. The engine's raw cost is the unfairness
  ``round6(sqrt(ssd))`` with ``ssd = sum(load^2) - sum(load)^2 / n`` over the
  ``n`` loaded items; CP-SAT has no square root, so the objective term is the
  integral surrogate ``scale * ssd`` (rounded up to an integer where ``n`` does
  not divide ``scale``), see `_add_unfairness_surrogate`. The confrontation
  surrogate also counts a team pair once per shared day, where the engine
  counts every same-day assignment pair (the two differ only while a team
  plays twice on one day, i.e. while ``team_conflict_cost > 0``). Reported
  scores are always rescored through the engine's ``constraint_costs()``.

Repeats and offsets: a constraint picked several times adds one set of terms per
pick. The two hard plugins add one term per team the engine evaluates (every team
for ``team_conflict_cost``, the teams with an unavailability fact for the scoped
``team_unavailability_cost``), so they take integral offsets (``terms_per_unit``).
The two imbalance surrogates have no raw-identical total to shift, so they refuse a
non-zero offset on CP-SAT (``constraint 'x' offset is not supported by this CP-SAT
model``); the engine takes it.
"""

import math
from dataclasses import dataclass, field
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from tournament_scheduling import constraints as domain
from tournament_scheduling.models import TournamentScheduleModel


def _add_unfairness_surrogate(
    context: "TournamentContext",
    objective: ObjectiveBuilder,
    name: str,
    loads: list[Any],
    max_load: int,
) -> None:
    """Minimize the variance numerator over positive loads (a surrogate).

    The reference LoadBalance ignores zero-load items and reports
    ``round6(sqrt(ssd))``, ``ssd = sum(load^2) - sum(load)^2 / n``. The square
    root and the 6-significant-figure rounding are monotone, so for this term
    ON ITS OWN the integer-scaled ``scale * ssd`` added here has the same
    minimizers (up to near-ties the rounding merges). It is not the engine's
    raw cost: its objective value is ``scale * ssd``, and next to other terms
    on the same level, under a weight or summed with the other imbalance term,
    the surrogate trades off differently than the square root would.
    """
    if not loads:
        return
    model = context.model
    item_count = len(loads)
    active = []
    for index, load in enumerate(loads):
        used = model.new_bool_var(f"{name}_{index}_active")
        model.add(load >= 1).only_enforce_if(used)
        model.add(load == 0).only_enforce_if(used.negated())
        active.append(used)

    active_count = model.new_int_var(0, item_count, f"{name}_active_count")
    model.add(active_count == sum(active))
    total = model.new_int_var(0, item_count * max_load, f"{name}_total")
    model.add(total == sum(loads))
    total_square = model.new_int_var(
        0, (item_count * max_load) ** 2, f"{name}_total_square"
    )
    model.add_multiplication_equality(total_square, [total, total])
    squares = []
    for index, load in enumerate(loads):
        square = model.new_int_var(0, max_load**2, f"{name}_{index}_square")
        model.add_multiplication_equality(square, [load, load])
        squares.append(square)
    sum_squares = model.new_int_var(0, item_count * max_load**2, f"{name}_sum_squares")
    model.add(sum_squares == sum(squares))

    # The exact common denominator lcm(1..n) grows like e^n and leaves int64
    # once there are a few dozen items (team pairs grow quadratically). Cap the
    # scale so every product below stays inside int64; for counts that do not
    # divide the scale the variance is rounded up to a multiple of 1/scale,
    # which can only merge near-ties. Reported scores are rescored exactly.
    cap = max(1, 2**50 // (item_count * (item_count * max(1, max_load)) ** 2))
    scale = min(math.lcm(*range(1, item_count + 1)), cap)
    bound = scale * item_count * max_load**2
    variance = model.new_int_var(0, bound, f"{name}_scaled_variance")
    choices = []
    for count in range(item_count + 1):
        chosen = model.new_bool_var(f"{name}_active_count_{count}")
        model.add(active_count == count).only_enforce_if(chosen)
        if count == 0:
            model.add(variance == 0).only_enforce_if(chosen)
        elif scale % count == 0:
            model.add(
                variance == scale * sum_squares - (scale // count) * total_square
            ).only_enforce_if(chosen)
        else:
            model.add(
                count * variance >= scale * (count * sum_squares - total_square)
            ).only_enforce_if(chosen)
        choices.append(chosen)
    model.add_exactly_one(choices)
    objective.add(name, variance, bound=bound)


@dataclass
class TournamentContext:
    model: cp_model.CpModel
    problem: TournamentScheduleModel
    x: dict[tuple[int, int], Any]
    by_day: list[list[int]]
    cell: dict[tuple[int, int], Any]
    _and_cache: dict[tuple[Any, ...], Any] = field(default_factory=dict)

    def both(self, key: tuple[Any, ...], left: Any, right: Any) -> Any:
        if key not in self._and_cache:
            both = self.model.new_bool_var("_".join(map(str, key)))
            self.model.add(both <= left)
            self.model.add(both <= right)
            self.model.add(both >= left + right - 1)
            self._and_cache[key] = both
        return self._and_cache[key]


class TeamConflictCost(domain.TeamConflictCost, ConstraintPlugin[TournamentContext]):
    """One term per team: its same-day assignment pairs, the engine's raw cost."""

    terms_per_unit = True  # one term per team, an idle one included

    def build(self, context: TournamentContext, objective: ObjectiveBuilder) -> None:
        for team in range(len(context.problem.teams)):
            pairs = []
            for day, assignments in enumerate(context.by_day):
                members = [context.x[assignment, team] for assignment in assignments]
                for left in range(len(members)):
                    for right in range(left + 1, len(members)):
                        pairs.append(
                            context.both(
                                ("duplicate", team, day, left, right),
                                members[left],
                                members[right],
                            )
                        )
            objective.add(self.name, sum(pairs), bound=len(pairs))


class TeamUnavailabilityCost(
    domain.TeamUnavailabilityCost, ConstraintPlugin[TournamentContext]
):
    """One term per team with a fact: its facts whose cell is occupied, the engine's raw cost.

    The engine scopes this constraint to those teams, so a team without a fact adds
    no term (a literal offset charges the scoped teams only)."""

    terms_per_unit = True  # one term per scoped team

    def build(self, context: TournamentContext, objective: ObjectiveBuilder) -> None:
        team_count = len(context.problem.teams)
        cells: dict[int, list[Any]] = {}
        for penalty in context.problem.unavailability_penalties:
            if 0 <= penalty.team < team_count:
                cells.setdefault(penalty.team, []).append(
                    context.cell[penalty.team, penalty.day]
                )
        for team in sorted(cells):
            objective.add(self.name, sum(cells[team]), bound=len(cells[team]))


class AssignmentCountImbalanceCost(
    domain.AssignmentCountImbalanceCost, ConstraintPlugin[TournamentContext]
):
    """The scaled-variance surrogate of the per-team load unfairness (not raw-identical)."""

    def build(self, context: TournamentContext, objective: ObjectiveBuilder) -> None:
        assignment_count = len(context.problem.team_assignments)
        loads = []
        for team in range(len(context.problem.teams)):
            load = context.model.new_int_var(0, assignment_count, f"team_{team}_load")
            context.model.add(
                load == sum(context.x[a, team] for a in range(assignment_count))
            )
            loads.append(load)
        _add_unfairness_surrogate(
            context, objective, self.name, loads, assignment_count
        )


class ConfrontationCountImbalanceCost(
    domain.ConfrontationCountImbalanceCost, ConstraintPlugin[TournamentContext]
):
    """The scaled-variance surrogate of the per-pair confrontation unfairness.

    Not raw-identical: besides the surrogate, a pair confronts once per day both
    teams play (the engine: once per same-day assignment pair)."""

    def build(self, context: TournamentContext, objective: ObjectiveBuilder) -> None:
        team_count = len(context.problem.teams)
        day_count = len(context.problem.days)
        loads = []
        for left in range(team_count):
            for right in range(left + 1, team_count):
                meetings = [
                    context.both(
                        ("confrontation", left, right, day),
                        context.cell[left, day],
                        context.cell[right, day],
                    )
                    for day in range(day_count)
                ]
                count = context.model.new_int_var(
                    0, day_count, f"confrontation_{left}_{right}_count"
                )
                context.model.add(count == sum(meetings))
                loads.append(count)
        _add_unfairness_surrogate(context, objective, self.name, loads, day_count)


# Plugin order = list_constraints() order.
REGISTRY = ConstraintRegistry(
    [
        TeamConflictCost(),
        TeamUnavailabilityCost(),
        AssignmentCountImbalanceCost(),
        ConfrontationCountImbalanceCost(),
    ]
)
