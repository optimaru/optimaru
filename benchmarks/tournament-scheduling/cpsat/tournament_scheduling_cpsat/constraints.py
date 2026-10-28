"""Class-based CP-SAT plugins for tournament scheduling."""

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
    """Minimize the exact variance over positive loads.

    Timefold's LoadBalance ignores zero-load items and reports the square root
    of the variance numerator.  The square root is monotone, so CP-SAT can
    minimize an integer-scaled variance without changing the optimum.
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

    scale = math.lcm(*range(1, item_count + 1))
    bound = scale * item_count * max_load**2
    variance = model.new_int_var(0, bound, f"{name}_scaled_variance")
    choices = []
    for count in range(item_count + 1):
        chosen = model.new_bool_var(f"{name}_active_count_{count}")
        model.add(active_count == count).only_enforce_if(chosen)
        if count == 0:
            model.add(variance == 0).only_enforce_if(chosen)
        else:
            model.add(
                variance == scale * sum_squares - (scale // count) * total_square
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


class OneAssignmentPerDatePerTeam(
    domain.OneAssignmentPerDatePerTeam, ConstraintPlugin[TournamentContext]
):
    default_level = "hard"

    def build(self, context: TournamentContext, objective: ObjectiveBuilder) -> None:
        for team in range(len(context.problem.teams)):
            for day, assignments in enumerate(context.by_day):
                members = [context.x[assignment, team] for assignment in assignments]
                for left in range(len(members)):
                    for right in range(left + 1, len(members)):
                        objective.add(
                            self.name,
                            context.both(
                                ("duplicate", team, day, left, right),
                                members[left],
                                members[right],
                            ),
                            bound=1,
                        )


class UnavailabilityPenalty(
    domain.UnavailabilityPenalty, ConstraintPlugin[TournamentContext]
):
    default_level = "hard"

    def build(self, context: TournamentContext, objective: ObjectiveBuilder) -> None:
        for penalty in context.problem.unavailability_penalties:
            objective.add(self.name, context.cell[penalty.team, penalty.day], bound=1)


class FairAssignmentCountPerTeam(
    domain.FairAssignmentCountPerTeam, ConstraintPlugin[TournamentContext]
):
    default_level = "medium"

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


class EvenlyConfrontationCount(
    domain.EvenlyConfrontationCount, ConstraintPlugin[TournamentContext]
):
    default_level = "soft"

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


REGISTRY = ConstraintRegistry(
    [
        OneAssignmentPerDatePerTeam(),
        UnavailabilityPenalty(),
        FairAssignmentCountPerTeam(),
        EvenlyConfrontationCount(),
    ]
)
