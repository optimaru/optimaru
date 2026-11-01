"""Class-based CP-SAT plugins for sports league scheduling.

Each plugin subclasses the domain spec of the same name; its default level and
weight come from the engine's ``sports_league.constraint_info()`` (the C++
components' kDefaultLevel / kDefaultWeight), so they are not restated here.
Before 0.2.0 the derby plugin restated only its level and so priced a workday
derby at weight 1 instead of the engine's 1000; reading the engine's catalog
removes that mismatch.

Every plugin adds one term per engine unit: the three unit constraints
(start_to_away_hop_cost, away_to_end_hop_cost, workday_classic_matches_cost) one
per round position, every round included (one that cannot cost is the constant
0), the seven globals one for their total. So a pick's ``offset`` / ``clip``
shape each round (or the total) exactly as the engine does (``terms_per_unit``),
and a constraint picked several times adds one set of terms per pick.
"""

from dataclasses import dataclass, field
from typing import Any, ClassVar

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from ortools.sat.python import cp_model
from sports_league import constraints as domain
from sports_league.models import LeagueScheduleModel


@dataclass
class LeagueContext:
    model: cp_model.CpModel
    problem: LeagueScheduleModel
    x: dict[tuple[int, int], Any]
    distance: list[list[int]]
    max_round_index: int
    round_position_by_index: dict[int, int]
    _and_cache: dict[tuple[Any, ...], Any] = field(default_factory=dict)

    def all(self, key: tuple[Any, ...], literals: list[Any]) -> Any:
        if key not in self._and_cache:
            both = self.model.new_bool_var("_".join(map(str, key)))
            self.model.add_bool_and(literals).only_enforce_if(both)
            self.model.add_bool_or(
                [literal.Not() for literal in literals]
            ).only_enforce_if(both.Not())
            self._and_cache[key] = both
        return self._and_cache[key]


class TeamConflictCost(domain.TeamConflictCost, ConstraintPlugin[LeagueContext]):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        pairs = []
        for left in range(len(context.problem.matches)):
            a = context.problem.matches[left]
            teams = {a.home_team, a.away_team}
            for right in range(left + 1, len(context.problem.matches)):
                b = context.problem.matches[right]
                if not teams & {b.home_team, b.away_team}:
                    continue
                for left_round, left_data in enumerate(context.problem.rounds):
                    for right_round, right_data in enumerate(context.problem.rounds):
                        if left_data.index != right_data.index:
                            continue
                        both = context.all(
                            ("pair", left, left_round, right, right_round),
                            [
                                context.x[left, left_round],
                                context.x[right, right_round],
                            ],
                        )
                        pairs.append(both)
        objective.add(self.name, sum(pairs), bound=len(pairs))


class RepeatMatchOnNextDayCost(
    domain.RepeatMatchOnNextDayCost, ConstraintPlugin[LeagueContext]
):
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        pairs = []
        for left in range(len(context.problem.matches)):
            a = context.problem.matches[left]
            for right in range(left + 1, len(context.problem.matches)):
                b = context.problem.matches[right]
                if not (a.home_team == b.away_team and a.away_team == b.home_team):
                    continue
                for left_round, left_data in enumerate(context.problem.rounds):
                    for right_round, right_data in enumerate(context.problem.rounds):
                        if abs(left_data.index - right_data.index) != 1:
                            continue
                        both = context.all(
                            ("pair", left, left_round, right, right_round),
                            [
                                context.x[left, left_round],
                                context.x[right, right_round],
                            ],
                        )
                        pairs.append(both)
        objective.add(self.name, sum(pairs), bound=len(pairs))


class _ConsecutiveSidePlugin(ConstraintPlugin[LeagueContext]):
    side: ClassVar[str]
    terms_per_unit = True  # a global constraint: one term for its total

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        bad_indices = []
        for team in range(len(context.problem.teams)):
            present = []
            for round_index in range(context.max_round_index + 1):
                terms = [
                    context.x[match, context.round_position_by_index[round_index]]
                    for match, match_data in enumerate(context.problem.matches)
                    if round_index in context.round_position_by_index
                    and getattr(match_data, f"{self.side}_team") == team
                ]
                value = context.model.new_bool_var(
                    f"team_{team}_{self.side}_{round_index}"
                )
                if terms:
                    context.model.add_max_equality(value, terms)
                else:
                    context.model.add(value == 0)
                present.append(value)
            runs_of_four = [
                context.all(
                    ("team", team, self.side, "four", start),
                    present[start : start + 4],
                )
                for start in range(max(0, len(present) - 3))
            ]
            for round_index in range(len(present)):
                covering = [
                    run
                    for start, run in enumerate(runs_of_four)
                    if start <= round_index < start + 4
                ]
                if not covering:
                    continue
                bad = context.model.new_bool_var(
                    f"team_{team}_{self.side}_bad_{round_index}"
                )
                context.model.add_max_equality(bad, covering)
                bad_indices.append(bad)
        objective.add(self.name, sum(bad_indices), bound=len(bad_indices))


class ConsecutiveHomeMatchesCost(
    domain.ConsecutiveHomeMatchesCost, _ConsecutiveSidePlugin
):
    side = "home"


class ConsecutiveAwayMatchesCost(
    domain.ConsecutiveAwayMatchesCost, _ConsecutiveSidePlugin
):
    side = "away"


def _per_round(name, context, objective, amount_of) -> None:
    """One term per round position: the summed ``amount_of(match, round)`` of the
    matches played there (the constant 0 when no match can cost there)."""
    for round_, round_data in enumerate(context.problem.rounds):
        amounts = [
            (match, amount_of(match_data, round_data))
            for match, match_data in enumerate(context.problem.matches)
        ]
        amounts = [(match, amount) for match, amount in amounts if amount]
        objective.add(
            name,
            sum(amount * context.x[match, round_] for match, amount in amounts),
            bound=sum(amount for _, amount in amounts),
        )


class StartToAwayHopCost(domain.StartToAwayHopCost, ConstraintPlugin[LeagueContext]):
    terms_per_unit = True  # one term per round position

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        def amount(match_data, round_data) -> int:
            if (
                round_data.index != 0
                and round_data.index - 1 in context.round_position_by_index
            ):
                return 0
            return context.distance[match_data.away_team][match_data.home_team]

        _per_round(self.name, context, objective, amount)


class AwayToEndHopCost(domain.AwayToEndHopCost, ConstraintPlugin[LeagueContext]):
    terms_per_unit = True  # one term per round position

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        def amount(match_data, round_data) -> int:
            if (
                round_data.index != context.max_round_index
                and round_data.index + 1 in context.round_position_by_index
            ):
                return 0
            return context.distance[match_data.home_team][match_data.away_team]

        _per_round(self.name, context, objective, amount)


class _BetweenMatchesPlugin(ConstraintPlugin[LeagueContext]):
    transition: ClassVar[str]
    terms_per_unit = True  # a global constraint: one term for its total

    def amount(self, context: LeagueContext, left: Any, right: Any) -> int:
        if self.transition == "home_to_away" and right.away_team == left.home_team:
            return context.distance[left.home_team][right.home_team]
        if self.transition == "away_to_away" and right.away_team == left.away_team:
            return context.distance[left.home_team][right.home_team]
        if self.transition == "away_to_home" and right.home_team == left.away_team:
            return context.distance[left.home_team][left.away_team]
        return 0

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        terms = []
        bound = 0
        for left in range(len(context.problem.matches)):
            for right in range(len(context.problem.matches)):
                if left == right:
                    continue
                left_data = context.problem.matches[left]
                right_data = context.problem.matches[right]
                amount = self.amount(context, left_data, right_data)
                if not amount:
                    continue
                for left_round, left_round_data in enumerate(context.problem.rounds):
                    for right_round, right_round_data in enumerate(
                        context.problem.rounds
                    ):
                        if right_round_data.index != left_round_data.index + 1:
                            continue
                        both = context.all(
                            ("travel", left, left_round, right, right_round),
                            [
                                context.x[left, left_round],
                                context.x[right, right_round],
                            ],
                        )
                        terms.append(amount * both)
                        bound += amount
        objective.add(self.name, sum(terms), bound=bound)


class HomeToAwayHopCost(domain.HomeToAwayHopCost, _BetweenMatchesPlugin):
    transition = "home_to_away"


class AwayToAwayHopCost(domain.AwayToAwayHopCost, _BetweenMatchesPlugin):
    transition = "away_to_away"


class AwayToHomeHopCost(domain.AwayToHomeHopCost, _BetweenMatchesPlugin):
    transition = "away_to_home"


class WorkdayClassicMatchesCost(
    domain.WorkdayClassicMatchesCost, ConstraintPlugin[LeagueContext]
):
    terms_per_unit = True  # one term per round position

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        _per_round(
            self.name,
            context,
            objective,
            lambda match_data, round_data: int(
                match_data.classic_match and not round_data.weekend_or_holiday
            ),
        )


# Plugin order = sports_league.list_constraints() order (unit constraints first).
REGISTRY = ConstraintRegistry(
    [
        StartToAwayHopCost(),
        AwayToEndHopCost(),
        WorkdayClassicMatchesCost(),
        TeamConflictCost(),
        ConsecutiveHomeMatchesCost(),
        ConsecutiveAwayMatchesCost(),
        RepeatMatchOnNextDayCost(),
        HomeToAwayHopCost(),
        AwayToAwayHopCost(),
        AwayToHomeHopCost(),
    ]
)
