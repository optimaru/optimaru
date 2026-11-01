"""Constraint assembly specs — an API for picking the cost function declaratively.

The default path (`fcs.solve(schedule)`) enables all six constraints at their default
weights (the Java provider's weights). This module is the alternative path next to it:
pick constraints, change weights, change levels, and pass the list to
`solve(schedule, constraints=...)`.

The shortest route is :func:`get_constraints`. It returns the default list, so
a "default with a small tweak" declaration fits on one line::

    import flight_crew as fcs
    from flight_crew.constraints import (
        RequiredSkillCost, flight_conflict_cost, get_constraints,
    )

    fcs.solve(schedule, constraints=get_constraints())          # defaults as-is
    fcs.solve(schedule, constraints=get_constraints([           # picked
        flight_conflict_cost, RequiredSkillCost(weight=500),
    ]))

Each constraint has a default instance at module level (`required_skill_cost`,
`flight_conflict_cost`, `transfer_between_two_flights_cost`,
`employee_unavailability_cost`, `first_assignment_not_departing_from_home_cost`,
`last_assignment_not_arriving_at_home_cost`), so you can also pass a list of just a
few. Only listed constraints are scored — leave one out and its term disappears from
the score.

Every spec takes ``(weight=None, level=None, exponent=1.0)``:

- ``weight``   linear multiplier on the constraint cost. ``None`` = the component
               default (required_skill_cost 100, flight_conflict_cost 10,
               transfer_between_two_flights_cost 1, employee_unavailability_cost 10,
               the two home-airport rules 1000); ``fcs.constraint_info()`` reports them.
- ``level``    ``None`` = the default level, otherwise ``Level.HARD`` or
               ``Level.SOFT`` (or the names ``"hard"`` and ``"soft"``). This use
               case has no medium level.
- ``exponent`` sign-preserving power shaping per employee: ``weight * sign(c) *
               |c| ** exponent``. 2.0 = squared, so concentrated violations grow much
               costlier.

Names (`name`) are the C++ component names in snake_case
(``RequiredSkillCost`` -> ``required_skill_cost``). The core derives them mechanically,
so there is no hand-maintained alias table — check them with `fcs.list_constraints()`.
Module-level instance names use the same snake_case, so they match the strings
`list_constraints()` prints.
"""

from ._constraint import Constraint, Level, Score, score_costs, select

__all__ = [
    "Constraint",
    "Level",
    "Score",
    "score",
    "RequiredSkillCost",
    "FlightConflictCost",
    "TransferBetweenTwoFlightsCost",
    "EmployeeUnavailabilityCost",
    "FirstAssignmentNotDepartingFromHomeCost",
    "LastAssignmentNotArrivingAtHomeCost",
    "required_skill_cost",
    "flight_conflict_cost",
    "transfer_between_two_flights_cost",
    "employee_unavailability_cost",
    "first_assignment_not_departing_from_home_cost",
    "last_assignment_not_arriving_at_home_cost",
    "get_constraints",
]


class RequiredSkillCost(Constraint):
    """Crew assignments lacking the skill the seat requires. Default: hard, weight 100.

    The heaviest hard rule of the provider — a crew member who cannot take the seat is
    worse than any scheduling inconvenience."""

    name = "required_skill_cost"
    label_en = "Required skill"
    label_ko = "필수 자격"
    description_en = (
        "Assignments whose crew member does not hold the skill the seat requires, such as an attendant "
        "on a pilot seat. One unit of cost is one such assignment; 0 means every seat is flown by a "
        "qualified crew member."
    )
    description_ko = (
        "좌석에 필요한 자격이 없는 승무원이 배정된 경우를 셉니다(예: 조종사 좌석에 객실 승무원). "
        "그런 배정 하나가 비용 1이며, 0이면 모든 좌석을 자격을 갖춘 승무원이 맡습니다."
    )


class FlightConflictCost(Constraint):
    """Pairs of the same crew member's assignments that overlap in time. Default: hard, weight 10.

    The plain sum over pairs **within** one itinerary, so the exact delta an assignment
    brings in or out is its overlap count with the other members."""

    name = "flight_conflict_cost"
    label_en = "Flight conflict"
    label_ko = "항공편 시간 겹침"
    description_en = (
        "Pairs of one crew member's flights whose times overlap (a flight landing exactly when the next "
        "departs does not overlap). One unit of cost is one overlapping pair; 0 means nobody is booked "
        "on two flights at once."
    )
    description_ko = (
        "한 승무원이 맡은 항공편 가운데 비행 시간이 겹치는 쌍을 셉니다(앞 항공편이 도착하는 순간 다음 "
        "항공편이 출발하면 겹치지 않습니다). 겹치는 쌍 하나가 비용 1이며, 0이면 아무도 두 항공편에 "
        "동시에 배정되지 않습니다."
    )


class TransferBetweenTwoFlightsCost(Constraint):
    """Arrival airport differing from the next departure airport. Default: hard, weight 1.

    Someone has to move the crew in between by plane or car. "Next" follows the Java
    stream exactly — assignments with the same departure time do not start a pair, and
    all ties at the later end count."""

    name = "transfer_between_two_flights_cost"
    label_en = "Transfer between two flights"
    label_ko = "항공편 간 이동"
    description_en = (
        "Consecutive flights of one crew member where the next one departs from a different airport "
        "than the previous one arrived at, so the crew has to be moved in between. One unit of cost is "
        "one such pair; 0 means every itinerary is a connected chain."
    )
    description_ko = (
        "한 승무원의 연속한 두 항공편에서 앞 항공편의 도착 공항과 다음 항공편의 출발 공항이 달라 그 "
        "사이에 이동이 필요한 경우를 셉니다. 그런 쌍 하나가 비용 1이며, 0이면 모든 일정이 끊김 없이 "
        "이어집니다."
    )


class EmployeeUnavailabilityCost(Constraint):
    """Assignments touching a date the crew member is unavailable. Default: hard, weight 10.

    Caught if any calendar date the flight spans, from departure date to arrival date
    inclusive, is unavailable."""

    name = "employee_unavailability_cost"
    label_en = "Crew unavailability"
    label_ko = "승무원 근무 불가일"
    description_en = (
        "Assignments whose flight touches a day the crew member is unavailable, from the departure "
        "date through the arrival date. One unit of cost is one such assignment; 0 means nobody flies "
        "on a day they are unavailable."
    )
    description_ko = (
        "비행 기간(출발일부터 도착일까지)에 승무원이 근무할 수 없는 날이 하루라도 포함된 배정을 "
        "셉니다. 그런 배정 하나가 비용 1이며, 0이면 아무도 근무할 수 없는 날에 비행하지 않습니다."
    )


class FirstAssignmentNotDepartingFromHomeCost(Constraint):
    """The first assignment not departing from the home airport. Default: soft, weight 1000.

    It means getting there on one's own time. All ties at the earliest departure
    time count."""

    name = "first_assignment_not_departing_from_home_cost"
    label_en = "First flight not from home"
    label_ko = "첫 항공편 거점 외 출발"
    description_en = (
        "Crew members whose earliest flight departs from an airport other than their home airport, so "
        "they must travel there first; flights tied on the earliest departure all count. One unit of "
        "cost is one such flight; 0 means every roster starts at home."
    )
    description_ko = (
        "승무원의 가장 이른 항공편이 거점 공항이 아닌 곳에서 출발해 먼저 그곳까지 가야 하는 경우를 "
        "셉니다. 가장 이른 출발 시각이 같은 항공편은 모두 셉니다. 그런 항공편 하나가 비용 1이며, "
        "0이면 모든 승무원이 거점 공항에서 일정을 시작합니다."
    )


class LastAssignmentNotArrivingAtHomeCost(Constraint):
    """The last assignment not arriving at the home airport. Default: soft, weight 1000.

    The roster ends with the crew member stuck away from home. All ties at the latest
    departure time count."""

    name = "last_assignment_not_arriving_at_home_cost"
    label_en = "Last flight not to home"
    label_ko = "마지막 항공편 거점 외 도착"
    description_en = (
        "Crew members whose latest flight lands at an airport other than their home airport, so the "
        "roster ends away from home; flights tied on the latest departure all count. One unit of cost "
        "is one such flight; 0 means every roster ends at home."
    )
    description_ko = (
        "승무원의 가장 늦은 항공편이 거점 공항이 아닌 곳에 도착해 일정이 거점 밖에서 끝나는 경우를 "
        "셉니다. 가장 늦은 출발 시각이 같은 항공편은 모두 셉니다. 그런 항공편 하나가 비용 1이며, "
        "0이면 모든 승무원이 거점 공항에서 일정을 마칩니다."
    )


#: Instances at the default weights (weight=None). Reference them directly when picking.
required_skill_cost = RequiredSkillCost()
flight_conflict_cost = FlightConflictCost()
transfer_between_two_flights_cost = TransferBetweenTwoFlightsCost()
employee_unavailability_cost = EmployeeUnavailabilityCost()
first_assignment_not_departing_from_home_cost = FirstAssignmentNotDepartingFromHomeCost()
last_assignment_not_arriving_at_home_cost = LastAssignmentNotArrivingAtHomeCost()

# The default set, in list_constraints() order.
_DEFAULTS = (
    required_skill_cost,
    flight_conflict_cost,
    transfer_between_two_flights_cost,
    employee_unavailability_cost,
    first_assignment_not_departing_from_home_cost,
    last_assignment_not_arriving_at_home_cost,
)


def get_constraints(constraints: list[Constraint] | None = None) -> list[Constraint]:
    """Return the cost function. Without arguments, all six default constraints.

    When `constraints` is given, that list is used as-is — only the listed
    constraints are scored. To change just one default, mix in the default instances::

        get_constraints([flight_conflict_cost, RequiredSkillCost(weight=500)])

    Same contract as the C++ applyConstraints: an empty list (most likely a filter
    that came out empty) and a constraint named twice raise ValueError.
    """
    return select(constraints, _DEFAULTS)


def score(costs: dict, constraints: list[Constraint] | None = None) -> Score:
    """Apply specs to raw constraint costs and produce a score — pure Python.

    ``costs`` is what `fcs.constraint_costs(schedule)` returns: per constraint, the
    **raw** cost of each employee (before weight, exponent and level rounding).
    Fetch it once and rescore with different weights without re-solving::

        raw = fcs.constraint_costs(schedule)
        score(raw)                     # default set
        score(raw, picked)             # picked list

    No solver and no search are involved. The arithmetic matches the engine —
    each employee contributes ``weight * shape(cost, exponent)`` and hard
    contributions are **rounded per employee** before summing (the same order as
    the engine; rounding the total afterwards gives different values).

    When ``constraints`` is given, only those constraints are scored — the same
    semantics as `get_constraints`. Unknown names raise ValueError.
    """
    return score_costs(costs, get_constraints(constraints))
