"""Class-based CP-SAT plugins for employee scheduling.

One plugin per rule type of `es_scheduling.models`, under the rule's own name
and in its own units. A request may hold several rules of one type with
different parameters, so a plugin wraps one rule and the registry is assembled
per request (`registry_for`), not once at import time.

Soft terms are multiplied by `SCALE`, because some are fractional and a CP-SAT
objective is not. Those terms round to that grid, and `penalize_half_ssd` stands
in for a root, so the objective is close to the engine's score but not equal to
it. The reported score is always the engine's own (`constraint_costs`).
"""

from __future__ import annotations

import math
from collections import Counter, defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from typing import Any, ClassVar, NamedTuple

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from es_scheduling import MINUTES_PER_DAY
from es_scheduling import models as domain
from es_scheduling.models import ScheduleRequest
from ortools.sat.python import cp_model

# Fixed-point denominator of the soft level. Even, because `BalanceAssignments`
# halves it.
SCALE = 100


class Score(NamedTuple):
    """(hard, soft), both as the engine reports them."""

    hard: int
    soft: float


def score(costs: dict, constraints: None = None) -> Score:
    """Read the engine's verdict out of `solve.constraint_costs`.

    The comparison runner expects this split between raw costs and score. The
    rule list lives in the request, so there is nothing to select here.
    """
    if constraints is not None:
        raise ValueError("the rules are part of the request; change them there")
    return Score(costs["hard"], costs["soft"])


class Scope(NamedTuple):
    """The workers and shifts a rule applies to, as engine indices."""

    workers: tuple[int, ...]
    shifts: tuple[int, ...]


def _minutes_on_day(start: int, end: int, day: int) -> int:
    """`ScoreCalculator::minutesOnDay`: the day's window stops at 23:59."""
    day_start = day * MINUTES_PER_DAY
    day_end = day_start + MINUTES_PER_DAY
    low = max(start, day_start)
    high = min(end, day_end)
    if high >= day_end:
        high = day_end - 1
    return max(0, high - low)


def date_overlap(start: int, end: int, dates: Iterable[int]) -> int:
    """`ScoreCalculator::overlapWithDates`: only the first and last day count."""
    dates = set(dates)
    first, last = start // MINUTES_PER_DAY, end // MINUTES_PER_DAY
    total = _minutes_on_day(start, end, first) if first in dates else 0
    if last != first and last in dates:
        total += _minutes_on_day(start, end, last)
    return total


@dataclass
class ScheduleContext:
    """The decision variables and the derived literals the rules share."""

    model: cp_model.CpModel
    problem: ScheduleRequest
    x: dict[tuple[int, int], Any]
    unassigned: list[Any]
    _any: dict[tuple[int, tuple[int, ...]], Any] = field(default_factory=dict)
    _count: dict[tuple[int, tuple[int, ...]], Any] = field(default_factory=dict)
    _pairs: dict[tuple[int, tuple[int, ...], tuple[int, ...]], Any] = field(
        default_factory=dict
    )
    _only: dict[tuple[int, tuple[int, ...], tuple[int, ...]], Any] = field(
        default_factory=dict
    )

    def __post_init__(self) -> None:
        self.workers = self.problem.model_input.workers
        self.shifts = self.problem.model_input.shifts
        self.day = [shift.start // MINUTES_PER_DAY for shift in self.shifts]
        self.horizon = max(self.day, default=-1) + 1
        self.false = self.model.new_bool_var("false")
        self.model.add(self.false == 0)

    def scope(self, rule: Any) -> Scope:
        """`ScopedUnit`: None is everyone, a list is exactly its valid indices."""

        def resolve(indices: list[int] | None, size: int) -> tuple[int, ...]:
            if indices is None:
                return tuple(range(size))
            return tuple(sorted({i for i in indices if 0 <= i < size}))

        return Scope(
            resolve(rule.worker_indices, len(self.workers)),
            resolve(rule.shift_indices, len(self.shifts)),
        )

    def by_day(
        self, shifts: Iterable[int], location: str = ""
    ) -> list[tuple[int, ...]]:
        """The matching shifts per day; an empty location matches all of them."""
        days: list[list[int]] = [[] for _ in range(self.horizon)]
        for shift in shifts:
            if not location or self.shifts[shift].location == location:
                days[self.day[shift]].append(shift)
        return [tuple(day) for day in days]

    def weeks(
        self, shifts: Iterable[int], anchor: int = 0
    ) -> list[tuple[int, ...]]:
        """7-day blocks from the first block holding any shift of the problem."""
        raw = [(day - anchor) // 7 for day in self.day]
        if not raw:
            return []
        lowest = min(raw)
        blocks: list[list[int]] = [[] for _ in range(max(raw) - lowest + 1)]
        for shift in shifts:
            blocks[raw[shift] - lowest].append(shift)
        return [tuple(block) for block in blocks]

    def any_of(self, worker: int, shifts: tuple[int, ...]) -> Any:
        """A literal that is true exactly when `worker` holds one of `shifts`."""
        if not shifts:
            return self.false
        if len(shifts) == 1:
            return self.x[shifts[0], worker]
        key = (worker, shifts)
        if key not in self._any:
            literal = self.model.new_bool_var(f"any_{worker}_{shifts[0]}_{len(shifts)}")
            self.model.add_max_equality(literal, [self.x[s, worker] for s in shifts])
            self._any[key] = literal
        return self._any[key]

    def presence(self, worker: int, days: Sequence[tuple[int, ...]]) -> list[Any]:
        return [self.any_of(worker, day) for day in days]

    def hit(self, literals: Sequence[Any]) -> Any:
        """A literal forced true when all of `literals` hold.

        Only that direction is modelled, which is enough for a penalty: the
        objective keeps the literal false whenever it is free to be.
        """
        literal = self.model.new_bool_var("hit")
        self.model.add_bool_or([*(item.negated() for item in literals), literal])
        return literal

    def count(self, worker: int, shifts: tuple[int, ...]) -> Any:
        """How many of `shifts` `worker` holds, as a variable."""
        key = (worker, shifts)
        if key not in self._count:
            variable = self.model.new_int_var(0, len(shifts), "count")
            self.model.add(variable == sum(self.x[s, worker] for s in shifts))
            self._count[key] = variable
        return self._count[key]

    def load(self, worker: int) -> Any:
        return self.count(worker, tuple(range(len(self.shifts))))

    def pairs(self, worker: int, left: tuple[int, ...], right: tuple[int, ...]) -> Any:
        """Shift pairs `worker` holds across `left` and `right`, or within `left`.

        Shifts with the same times cost the same with any other shift, so this
        counts over those groups: per worker and group pair one variable, where
        a variable per shift pair and worker reaches 600,000 booleans on the
        largest instance.
        """
        key = (worker, left, right)
        if key not in self._pairs:
            if left == right:
                top = len(left)
                table = [held * (held - 1) // 2 for held in range(top + 1)]
                variable = self.model.new_int_var(0, table[-1], "pairs")
                self.model.add_element(self.count(worker, left), table, variable)
            else:
                variable = self.model.new_int_var(0, len(left) * len(right), "pairs")
                self.model.add_multiplication_equality(
                    variable, [self.count(worker, left), self.count(worker, right)]
                )
            self._pairs[key] = variable
        return self._pairs[key]

    def squares(self, counts: Sequence[Any], top: int) -> list[Any]:
        result = []
        for value in counts:
            square = self.model.new_int_var(0, top**2, "square")
            self.model.add_multiplication_equality(square, [value, value])
            result.append(square)
        return result

    def only_at(
        self, worker: int, here: tuple[int, ...], elsewhere: tuple[int, ...]
    ) -> Any:
        """True when `worker` holds a shift of `here` and none of `elsewhere`."""
        at = self.any_of(worker, here)
        if not elsewhere:
            return at
        key = (worker, here, elsewhere)
        if key not in self._only:
            other = self.any_of(worker, elsewhere)
            literal = self.model.new_bool_var("only_at")
            self.model.add_bool_and([at, other.negated()]).only_enforce_if(literal)
            self.model.add_bool_or([at.negated(), other, literal])
            self._only[key] = literal
        return self._only[key]


class RulePlugin(ConstraintPlugin[ScheduleContext]):
    """One rule of a request, built into the model at the rule's level."""

    rule_type: ClassVar[type]
    # The C++ component's own default level. It decides the score, so it is the
    # one that counts where a rule model's docstring says otherwise.
    default_hard: ClassVar[bool]
    # Whether the raw cost can be fractional. Only the soft level has a fixed-point
    # grid, and the engine rounds a hard cost per unit, which this model cannot follow.
    fractional = False

    def __init__(self, rule: Any, name: str) -> None:
        self.rule = rule
        if rule.exponent != 1.0:
            raise ValueError(f"rule {rule.name!r}: exponent is not supported by CP-SAT")
        if not float(rule.weight).is_integer():
            raise ValueError(f"rule {rule.name!r}: weight must be an integer for CP-SAT")
        hard = self.default_hard if rule.hard is None else rule.hard
        if hard and self.fractional:
            raise ValueError(f"rule {rule.name!r}: a fractional cost cannot be hard for CP-SAT")
        self.name = name
        self.default_level = "hard" if hard else "soft"
        self.default_weight = rule.weight
        self.label = rule.name
        self.scale = 1 if hard else SCALE

    def penalize(
        self, objective: ObjectiveBuilder, magnitude: Any, bound: int
    ) -> None:
        objective.add(self.name, self.scale * magnitude, bound=self.scale * bound)

    def reward(self, objective: ObjectiveBuilder, magnitude: Any, bound: int) -> None:
        objective.add_reward(
            self.name, self.scale * magnitude, bound=self.scale * bound
        )

    def penalize_outside(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        total: Any,
        top: int,
        low: int,
        high: int,
    ) -> None:
        """Shortfall below `low` plus excess above `high` (`high` < 0: no cap)."""
        if low > 0:
            shortfall = context.model.new_int_var(0, low, "shortfall")
            context.model.add(shortfall >= low - total)
            self.penalize(objective, shortfall, low)
        if 0 <= high < top:
            excess = context.model.new_int_var(0, top - high, "excess")
            context.model.add(excess >= total - high)
            self.penalize(objective, excess, top - high)

    def penalize_gap(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        values: Sequence[Any],
        top: int,
        tolerance: int,
        per: int = 1,
    ) -> None:
        """max(values) - min(values) beyond `tolerance`, divided by `per`."""
        model = context.model
        if len(values) < 2 or top <= tolerance:
            return
        highest = model.new_int_var(0, top, "highest")
        lowest = model.new_int_var(0, top, "lowest")
        for value in values:
            model.add(highest >= value)
            model.add(lowest <= value)
        excess = model.new_int_var(0, top - tolerance, "gap")
        model.add(excess >= highest - lowest - tolerance)
        if per == 1:
            self.penalize(objective, excess, top - tolerance)
            return
        # Rounded up to the fixed-point grid.
        bound = math.ceil(self.scale * (top - tolerance) / per)
        scaled = model.new_int_var(0, bound, "gap_scaled")
        model.add(per * scaled >= self.scale * excess)
        objective.add(self.name, scaled, bound=bound)

    def penalize_half_ssd(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        counts: Sequence[Any],
        top: int,
        total_top: int,
    ) -> None:
        """ssd / 2, where ssd = sum(c^2) - sum(c)^2 / n, for the engine's sqrt(ssd).

        ssd / 2 is the root's tangent at ssd = 1, less a constant. Both fall as
        the counts even out, but this one weighs a large imbalance more heavily
        against the other soft rules than the engine does.
        """
        model = context.model
        n = len(counts)
        if n < 2 or top == 0:
            return
        total = model.new_int_var(0, total_top, "total")
        model.add(total == sum(counts))
        total_square = model.new_int_var(0, total_top**2, "total_square")
        model.add_multiplication_equality(total_square, [total, total])
        # sum(c^2) <= max(c) * sum(c), so this bounds the variable.
        bound = math.ceil(self.scale * top * total_top / 2)
        half_ssd = model.new_int_var(0, bound, "half_ssd")
        squares = sum(context.squares(counts, top))
        model.add(2 * n * half_ssd >= self.scale * (n * squares - total_square))
        objective.add(self.name, half_ssd, bound=bound)

    def penalize_run_excess(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        days: Sequence[Any],
        limit: int,
    ) -> None:
        """len - limit per maximal run: a run holds that many windows of limit + 1."""
        for end in range(limit, len(days)):
            self.penalize(objective, context.hit(days[end - limit : end + 1]), 1)

    def penalize_run_shortfall(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        days: Sequence[Any],
        limit: int,
        waive_at_end: bool = True,
    ) -> None:
        """limit - len per maximal run shorter than `limit`."""
        horizon = len(days)
        for start in range(horizon):
            for length in range(1, limit):
                after = start + length
                if after > horizon or (after == horizon and waive_at_end):
                    break
                run = list(days[start:after])
                if start > 0:
                    run.append(days[start - 1].negated())
                if after < horizon:
                    run.append(days[after].negated())
                self.penalize(
                    objective, (limit - length) * context.hit(run), limit - length
                )


class RequiredSkill(RulePlugin):
    rule_type = domain.RequiredSkillRule
    default_hard = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        for shift in scope.shifts:
            skill = context.shifts[shift].required_skill
            if not skill:
                continue
            for worker in scope.workers:
                if skill not in context.workers[worker].skills:
                    self.penalize(objective, context.x[shift, worker], 1)


class DateRule(RulePlugin):
    """A per-(worker, shift) table of overlap minutes with a set of days."""

    dates: ClassVar[str]
    is_reward: ClassVar[bool] = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        per_day = getattr(self.rule, "per_day", False)
        for shift in scope.shifts:
            data = context.shifts[shift]
            terms = []
            for worker in scope.workers:
                dates = getattr(context.workers[worker], self.dates)
                minutes = date_overlap(data.start, data.end, dates)
                if minutes:
                    terms.append(
                        (1 if per_day else minutes, context.x[shift, worker])
                    )
            if terms:
                magnitude = sum(cost * literal for cost, literal in terms)
                bound = max(cost for cost, _ in terms)
                add = self.reward if self.is_reward else self.penalize
                add(objective, magnitude, bound)


class UnavailableEmployee(DateRule):
    rule_type = domain.UnavailableWorkerRule
    default_hard = True
    dates = "unavailable_dates"


class UndesiredDay(DateRule):
    rule_type = domain.UndesiredDayRule
    default_hard = False
    dates = "undesired_dates"


class DesiredDay(DateRule):
    rule_type = domain.DesiredDayRule
    default_hard = False
    dates = "desired_dates"
    is_reward = True


class DesiredShift(RulePlugin):
    rule_type = domain.DesiredShiftRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        wishes = {
            worker: {tuple(item) for item in context.workers[worker].desired_shifts}
            for worker in scope.workers
        }
        for shift in scope.shifts:
            wanted = (context.day[shift], context.shifts[shift].location)
            literals = [
                context.x[shift, worker]
                for worker in scope.workers
                if wanted in wishes[worker]
            ]
            if literals:
                self.reward(objective, sum(literals), 1)


class PairRule(RulePlugin):
    """A cost per pair of shifts held by one worker, by the two shifts' times."""

    def pair_cost(self, left: tuple[int, int], right: tuple[int, int]) -> int:
        raise NotImplementedError

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        groups: defaultdict[tuple[int, int], list[int]] = defaultdict(list)
        for shift in scope.shifts:
            data = context.shifts[shift]
            groups[data.start, data.end].append(shift)
        items = sorted((key, tuple(shifts)) for key, shifts in groups.items())
        for index, (left_times, left) in enumerate(items):
            for right_times, right in items[index:]:
                same = left == right
                top = len(left) * (len(left) - 1) // 2 if same else len(left) * len(right)
                unit = self.pair_cost(left_times, right_times) if top else 0
                if unit <= 0:
                    continue
                for worker in scope.workers:
                    self.penalize(
                        objective, unit * context.pairs(worker, left, right), unit * top
                    )


class OverlappingShift(PairRule):
    rule_type = domain.OverlappingShiftRule
    default_hard = True

    def pair_cost(self, left: tuple[int, int], right: tuple[int, int]) -> int:
        return max(0, min(left[1], right[1]) - max(left[0], right[0]))


class MaxOneShiftPerDay(PairRule):
    rule_type = domain.MaxOneShiftPerDayRule
    default_hard = True

    def pair_cost(self, left: tuple[int, int], right: tuple[int, int]) -> int:
        return int(left[0] // MINUTES_PER_DAY == right[0] // MINUTES_PER_DAY)


class MinimumRest(PairRule):
    rule_type = domain.MinimumRestRule
    default_hard = True

    def pair_cost(self, left: tuple[int, int], right: tuple[int, int]) -> int:
        early, late = (left, right) if left[0] <= right[0] else (right, left)
        if early[1] > late[0]:
            return 0
        return max(0, self.rule.min_gap_minutes - (late[0] - early[1]))


class BalanceAssignments(RulePlugin):
    rule_type = domain.BalanceAssignmentsRule
    default_hard = False
    fractional = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        """sum(load^2) / 2, which is ssd / 2 plus a constant once all shifts are assigned.

        So the sum(c)^2 term of `penalize_half_ssd` can go, and it is the one
        that costs: as a multiplication over all shifts, it left CP-SAT short of
        hard 0 on `large` in five of six 30 s runs, against one of four without.
        """
        loads = [context.load(worker) for worker in range(len(context.workers))]
        top = len(context.shifts)
        # A soft price on empty shifts is the one case where the total really
        # varies, and without the term an empty shift would look like a gain.
        if any(
            rule.name == "unassigned_shift" and rule.hard is False
            for rule in context.problem.model_input.rules
        ):
            self.penalize_half_ssd(context, objective, loads, top, top)
            return
        if len(loads) < 2 or top == 0:
            return
        half = self.scale // 2
        objective.add(
            self.name, half * sum(context.squares(loads, top)), bound=half * top**2
        )


class MaxConsecutiveDays(RulePlugin):
    rule_type = domain.MaxConsecutiveDaysRule
    default_hard = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        days = context.by_day(scope.shifts)
        for worker in scope.workers:
            self.penalize_run_excess(
                context, objective, context.presence(worker, days), self.rule.value
            )


class MinConsecutiveDays(RulePlugin):
    rule_type = domain.MinConsecutiveDaysRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        days = context.by_day(scope.shifts)
        for worker in scope.workers:
            self.penalize_run_shortfall(
                context, objective, context.presence(worker, days), self.rule.value
            )


class MaxConsecutiveOff(RulePlugin):
    rule_type = domain.MaxConsecutiveOffRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        """A run of off days between two worked days holds len - value windows."""
        scope = context.scope(self.rule)
        days = context.by_day(scope.shifts)
        size = self.rule.value + 1
        for worker in scope.workers:
            worked = context.presence(worker, days)
            before = self._seen(context, worked)
            after = self._seen(context, worked[::-1])[::-1]
            for start in range(context.horizon - size + 1):
                window = [day.negated() for day in worked[start : start + size]]
                self.penalize(
                    objective,
                    context.hit([before[start], *window, after[start + size - 1]]),
                    1,
                )

    @staticmethod
    def _seen(context: ScheduleContext, worked: Sequence[Any]) -> list[Any]:
        """Per position, whether any earlier position is worked."""
        seen = [context.false]
        for day in worked[:-1]:
            literal = context.model.new_bool_var("seen")
            context.model.add_max_equality(literal, [seen[-1], day])
            seen.append(literal)
        return seen


class RestAfterConsecutiveWork(RulePlugin):
    rule_type = domain.RestAfterConsecutiveWorkRule
    default_hard = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        days = context.by_day(scope.shifts)
        work_days, rest_days = self.rule.work_days, self.rule.rest_days
        for worker in scope.workers:
            worked = context.presence(worker, days)
            for gap in range(1, rest_days):
                for last in range(work_days - 1, context.horizon - gap - 1):
                    pattern = [
                        *worked[last - work_days + 1 : last + 1],
                        *(day.negated() for day in worked[last + 1 : last + gap + 1]),
                        worked[last + gap + 1],
                    ]
                    self.penalize(
                        objective,
                        (rest_days - gap) * context.hit(pattern),
                        rest_days - gap,
                    )


class WeeklyMinutes(RulePlugin):
    def anchor(self) -> int:
        return 0

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        for week in context.weeks(scope.shifts, self.anchor()):
            duration = {s: context.shifts[s].end - context.shifts[s].start for s in week}
            top = sum(duration.values())
            for worker in scope.workers:
                total = sum(duration[s] * context.x[s, worker] for s in week)
                self.penalize_week(context, objective, total, top)

    def penalize_week(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        total: Any,
        top: int,
    ) -> None:
        raise NotImplementedError


class MaxWeeklyMinutes(WeeklyMinutes):
    rule_type = domain.MaxWeeklyMinutesRule
    default_hard = True

    def anchor(self) -> int:
        return self.rule.week_anchor

    def penalize_week(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        total: Any,
        top: int,
    ) -> None:
        self.penalize_outside(context, objective, total, top, 0, self.rule.minutes)


class MinWeeklyMinutes(WeeklyMinutes):
    rule_type = domain.MinWeeklyMinutesRule
    default_hard = False

    def penalize_week(
        self,
        context: ScheduleContext,
        objective: ObjectiveBuilder,
        total: Any,
        top: int,
    ) -> None:
        self.penalize_outside(context, objective, total, top, self.rule.minutes, -1)


class MinWeeklyRestDays(RulePlugin):
    rule_type = domain.MinWeeklyRestDaysRule
    default_hard = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        scope = context.scope(self.rule)
        for week in context.weeks(scope.shifts):
            days = [day for day in context.by_day(week) if day]
            for worker in scope.workers:
                self.penalize_outside(
                    context,
                    objective,
                    sum(context.presence(worker, days)),
                    len(days),
                    0,
                    7 - self.rule.value,
                )


class AssignmentCost(RulePlugin):
    rule_type = domain.AssignmentCostRule
    default_hard = False
    fractional = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        scope = context.scope(rule)
        for shift in scope.shifts:
            data = context.shifts[shift]
            base = rule.cost_per_shift
            if rule.cost_per_hour > 0.0:
                base = rule.cost_per_hour * (data.end - data.start) / 60.0
            terms = []
            for worker in scope.workers:
                row = rule.shift_costs.get(context.workers[worker].id, {})
                terms.append((round(self.scale * row.get(data.id, base)), worker))
            magnitude = sum(cost * context.x[shift, worker] for cost, worker in terms)
            # Already scaled. A cost may be negative, and the term is 0 when the
            # shift goes elsewhere, so the bound is the width of that whole range.
            costs = [0, *(cost for cost, _ in terms)]
            objective.add(self.name, magnitude, bound=max(costs) - min(costs))


class UnassignedShift(RulePlugin):
    rule_type = domain.UnassignedShiftRule
    default_hard = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        self.penalize(objective, sum(context.unassigned), len(context.unassigned))


class BalanceMinMaxGap(RulePlugin):
    rule_type = domain.BalanceMinMaxGapRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        loads = [context.load(worker) for worker in range(len(context.workers))]
        self.penalize_gap(
            context, objective, loads, len(context.shifts), self.rule.tolerance
        )


class BalanceWeeklyMinutesGap(RulePlugin):
    rule_type = domain.BalanceWeeklyMinutesGapRule
    default_hard = False
    fractional = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        workers = range(len(context.workers))
        for week in context.weeks(range(len(context.shifts))):
            duration = {s: context.shifts[s].end - context.shifts[s].start for s in week}
            totals = [
                sum(duration[s] * context.x[s, worker] for s in week)
                for worker in workers
            ]
            self.penalize_gap(
                context,
                objective,
                totals,
                sum(duration.values()),
                self.rule.tolerance_minutes,
                per=60,
            )


class ShiftCoverage(RulePlugin):
    rule_type = domain.ShiftCoverageRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        # This rule scopes itself: unlike ScopedUnit, an empty list means all.
        rule = self.rule
        everyone = range(len(context.workers))
        counted = [
            worker
            for worker in sorted(set(rule.worker_indices or everyone) & set(everyone))
            if not rule.worker_skill
            or rule.worker_skill in context.workers[worker].skills
        ]
        every_shift = range(len(context.shifts))
        groups: defaultdict[tuple[int, str], list[int]] = defaultdict(list)
        for shift in sorted(set(rule.shift_indices or every_shift) & set(every_shift)):
            groups[context.day[shift], context.shifts[shift].location].append(shift)
        for shifts in groups.values():
            total = sum(context.x[s, w] for s in shifts for w in counted)
            self.penalize_outside(
                context, objective, total, len(shifts), rule.min_count, rule.max_count
            )


class LocationRun(RulePlugin):
    rule_type = domain.LocationRunRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        scope = context.scope(rule)
        days = context.by_day(scope.shifts, rule.location)
        for worker in scope.workers:
            present = context.presence(worker, days)
            self.penalize_run_shortfall(
                context, objective, present, rule.min_run, rule.waive_min_at_end
            )
            if rule.max_run >= 0:
                self.penalize_run_excess(context, objective, present, rule.max_run)


class RestAfterLocationRun(RulePlugin):
    rule_type = domain.RestAfterLocationRunRule
    default_hard = True

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        scope = context.scope(rule)
        at_location = context.by_day(scope.shifts, rule.location)
        days = context.by_day(scope.shifts)
        for worker in scope.workers:
            present = context.presence(worker, at_location)
            worked = context.presence(worker, days)
            for last in range(context.horizon - 1):
                for later in range(last + 1, min(last + rule.rest_days + 1, context.horizon)):
                    self.penalize(
                        objective,
                        context.hit(
                            [present[last], present[last + 1].negated(), worked[later]]
                        ),
                        1,
                    )


class LocationRunGap(RulePlugin):
    rule_type = domain.LocationRunGapRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        scope = context.scope(rule)
        days = context.by_day(scope.shifts, rule.location)
        for worker in scope.workers:
            present = context.presence(worker, days)
            for gap in range(1, rule.min_gap):
                for last in range(context.horizon - gap - 1):
                    pattern = [
                        present[last],
                        *(day.negated() for day in present[last + 1 : last + gap + 1]),
                        present[last + gap + 1],
                    ]
                    self.penalize(
                        objective,
                        (rule.min_gap - gap) * context.hit(pattern),
                        rule.min_gap - gap,
                    )


class WorkerLocationCount(RulePlugin):
    rule_type = domain.WorkerLocationCountRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        scope = context.scope(rule)
        shifts = [
            shift
            for shift in scope.shifts
            if not rule.location or context.shifts[shift].location == rule.location
        ]
        for worker in scope.workers:
            if rule.worker_skill and rule.worker_skill not in context.workers[worker].skills:
                continue
            total = sum(context.x[shift, worker] for shift in shifts)
            self.penalize_outside(
                context, objective, total, len(shifts), rule.min_count, rule.max_count
            )


class DayPattern(RulePlugin):
    rule_type = domain.DayPatternRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        tokens = self.rule.pattern.split(",")
        known = {shift.location for shift in context.shifts}
        scope = context.scope(self.rule)
        days = context.by_day(scope.shifts)
        for worker in scope.workers:
            for start in range(context.horizon - len(tokens) + 1):
                window: list[Any] = []
                for offset, token in enumerate(tokens):
                    day = start + offset
                    window.extend(
                        self._literals(context, worker, days[day], token, known)
                    )
                self.penalize(objective, context.hit(window), 1)

    @staticmethod
    def _literals(
        context: ScheduleContext,
        worker: int,
        day: tuple[int, ...],
        token: str,
        known: set[str],
    ) -> list[Any]:
        """What must hold for `token` to match `worker`'s `day`."""

        def only_at(location: str) -> Any:
            here = tuple(s for s in day if context.shifts[s].location == location)
            elsewhere = tuple(s for s in day if context.shifts[s].location != location)
            return context.only_at(worker, here, elsewhere)

        worked = context.any_of(worker, day)
        if token == "*":
            return []
        if token == "OFF":
            return [worked.negated()]
        if token == "WORK":
            return [worked]
        if len(token) > 1 and token[0] == "!":
            return [worked, only_at(token[1:]).negated()]
        return [only_at(token) if token in known else context.false]


class TargetDaysOff(RulePlugin):
    rule_type = domain.TargetDaysOffRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        scope = context.scope(rule)
        days = context.by_day(scope.shifts)
        targets = sorted({d for d in rule.target_days if 0 <= d < context.horizon})
        for worker in scope.workers:
            worked = context.presence(worker, [days[d] for d in targets])
            self.penalize_outside(
                context,
                objective,
                len(targets) - sum(worked),
                len(targets),
                rule.min_count,
                -1,
            )


class WorkerDayCountInRange(RulePlugin):
    rule_type = domain.WorkerDayCountInRuleRange
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        if rule.hi < rule.lo:
            return
        scope = context.scope(rule)
        days = context.by_day(scope.shifts, rule.location)
        window = days[rule.lo : rule.hi + 1]
        for worker in scope.workers:
            self.penalize_outside(
                context,
                objective,
                sum(context.presence(worker, window)),
                len(window),
                rule.min_count,
                rule.max_count,
            )


class BalanceLocation(RulePlugin):
    rule_type = domain.BalanceLocationRule
    default_hard = False

    @property
    def fractional(self) -> bool:
        return self.rule.metric == "ssd"

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        workers = [
            worker
            for worker in range(len(context.workers))
            if not rule.worker_skill
            or rule.worker_skill in context.workers[worker].skills
        ]
        if rule.off_target_days:
            days = context.by_day(range(len(context.shifts)))
            targets = sorted(
                {d for d in rule.off_target_days if 0 <= d < context.horizon}
            )
            top = len(targets)
            counts = [
                len(targets) - sum(context.presence(worker, [days[d] for d in targets]))
                for worker in workers
            ]
            total_top = top * len(workers)
        else:
            shifts = tuple(
                shift
                for shift in range(len(context.shifts))
                if not rule.location or context.shifts[shift].location == rule.location
            )
            top = total_top = len(shifts)
            counts = [context.count(worker, shifts) for worker in workers]
        if rule.metric == "gap":
            self.penalize_gap(context, objective, counts, top, rule.tolerance)
        else:
            self.penalize_half_ssd(context, objective, counts, top, total_top)


class WorkerPairSameShift(RulePlugin):
    rule_type = domain.WorkerPairSameShiftRule
    default_hard = False

    def build(self, context: ScheduleContext, objective: ObjectiveBuilder) -> None:
        rule = self.rule
        buckets: defaultdict[tuple[int, str], list[int]] = defaultdict(list)
        for shift, data in enumerate(context.shifts):
            buckets[context.day[shift], data.location].append(shift)

        def holds(worker: int, shifts: tuple[int, ...]) -> Any:
            if 0 <= worker < len(context.workers):
                return context.any_of(worker, shifts)
            return context.false

        for bucket in set(zip(rule.bucket_days, rule.bucket_locations)):
            shifts = tuple(buckets.get(bucket, ()))
            anchor, paired = holds(rule.anchor, shifts), holds(rule.paired, shifts)
            if rule.penalty:
                self.penalize(objective, context.hit([anchor, paired]), 1)
                continue
            differs = context.model.new_bool_var("pair_differs")
            context.model.add_bool_or([anchor.negated(), paired, differs])
            context.model.add_bool_or([anchor, paired.negated(), differs])
            self.penalize(objective, differs, 1)


PLUGINS: dict[str, type[RulePlugin]] = {
    plugin.rule_type.model_fields["name"].default: plugin
    for plugin in (
        RequiredSkill,
        UnavailableEmployee,
        OverlappingShift,
        MaxOneShiftPerDay,
        MinimumRest,
        UndesiredDay,
        DesiredDay,
        DesiredShift,
        BalanceAssignments,
        MaxConsecutiveDays,
        MinConsecutiveDays,
        MaxConsecutiveOff,
        RestAfterConsecutiveWork,
        MaxWeeklyMinutes,
        MinWeeklyMinutes,
        MinWeeklyRestDays,
        AssignmentCost,
        UnassignedShift,
        BalanceMinMaxGap,
        BalanceWeeklyMinutesGap,
        ShiftCoverage,
        LocationRun,
        RestAfterLocationRun,
        LocationRunGap,
        WorkerLocationCount,
        DayPattern,
        TargetDaysOff,
        WorkerDayCountInRange,
        BalanceLocation,
        WorkerPairSameShift,
    )
}


def registry_for(rules: Iterable[Any]) -> ConstraintRegistry[ScheduleContext]:
    """One plugin per rule; a repeated rule type gets a numbered name."""
    seen: Counter[str] = Counter()
    plugins = []
    for rule in rules:
        seen[rule.name] += 1
        count = seen[rule.name]
        plugins.append(
            PLUGINS[rule.name](rule, rule.name if count == 1 else f"{rule.name}#{count}")
        )
    return ConstraintRegistry(plugins)


REGISTRY = registry_for(plugin.rule_type() for plugin in PLUGINS.values())
