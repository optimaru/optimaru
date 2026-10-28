"""Class-based CP-SAT plugins for sports league scheduling."""

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


class MatchesOnSameDay(domain.MatchesOnSameDay, ConstraintPlugin[LeagueContext]):
    default_level = "hard"

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
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
                        objective.add(self.name, both, bound=1)


class RepeatMatchOnNextDay(
    domain.RepeatMatchOnNextDay, ConstraintPlugin[LeagueContext]
):
    default_level = "hard"

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
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
                        objective.add(self.name, both, bound=1)


class _ConsecutiveSidePlugin(ConstraintPlugin[LeagueContext]):
    side: ClassVar[str]

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
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
                objective.add(self.name, bad, bound=1)


class ConsecutiveHomeMatches(domain.ConsecutiveHomeMatches, _ConsecutiveSidePlugin):
    default_level = "hard"
    side = "home"


class ConsecutiveAwayMatches(domain.ConsecutiveAwayMatches, _ConsecutiveSidePlugin):
    default_level = "hard"
    side = "away"


class StartToAwayHop(domain.StartToAwayHop, ConstraintPlugin[LeagueContext]):
    default_level = "soft"

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        for match, match_data in enumerate(context.problem.matches):
            for round_, round_data in enumerate(context.problem.rounds):
                if (
                    round_data.index != 0
                    and round_data.index - 1 in context.round_position_by_index
                ):
                    continue
                value = context.distance[match_data.away_team][match_data.home_team]
                objective.add(self.name, value * context.x[match, round_], bound=value)


class AwayToEndHop(domain.AwayToEndHop, ConstraintPlugin[LeagueContext]):
    default_level = "soft"

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        for match, match_data in enumerate(context.problem.matches):
            for round_, round_data in enumerate(context.problem.rounds):
                if (
                    round_data.index != context.max_round_index
                    and round_data.index + 1 in context.round_position_by_index
                ):
                    continue
                value = context.distance[match_data.home_team][match_data.away_team]
                objective.add(self.name, value * context.x[match, round_], bound=value)


class _BetweenMatchesPlugin(ConstraintPlugin[LeagueContext]):
    transition: ClassVar[str]

    def amount(self, context: LeagueContext, left: Any, right: Any) -> int:
        if self.transition == "home_to_away" and right.away_team == left.home_team:
            return context.distance[left.home_team][right.home_team]
        if self.transition == "away_to_away" and right.away_team == left.away_team:
            return context.distance[left.home_team][right.home_team]
        if self.transition == "away_to_home" and right.home_team == left.away_team:
            return context.distance[left.home_team][left.away_team]
        return 0

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
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
                        objective.add(self.name, amount * both, bound=amount)


class HomeToAwayHop(domain.HomeToAwayHop, _BetweenMatchesPlugin):
    default_level = "soft"
    transition = "home_to_away"


class AwayToAwayHop(domain.AwayToAwayHop, _BetweenMatchesPlugin):
    default_level = "soft"
    transition = "away_to_away"


class AwayToHomeHop(domain.AwayToHomeHop, _BetweenMatchesPlugin):
    default_level = "soft"
    transition = "away_to_home"


class ClassicMatches(domain.ClassicMatches, ConstraintPlugin[LeagueContext]):
    default_level = "soft"

    def build(self, context: LeagueContext, objective: ObjectiveBuilder) -> None:
        for match, match_data in enumerate(context.problem.matches):
            if not match_data.classic_match:
                continue
            for round_, round_data in enumerate(context.problem.rounds):
                invalid = int(not round_data.weekend_or_holiday)
                objective.add(
                    self.name,
                    invalid * context.x[match, round_],
                    bound=invalid,
                )


REGISTRY = ConstraintRegistry(
    [
        MatchesOnSameDay(),
        ConsecutiveHomeMatches(),
        ConsecutiveAwayMatches(),
        RepeatMatchOnNextDay(),
        StartToAwayHop(),
        HomeToAwayHop(),
        AwayToAwayHop(),
        AwayToHomeHop(),
        AwayToEndHop(),
        ClassicMatches(),
    ]
)
