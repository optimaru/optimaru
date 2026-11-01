"""Class-based CP-SAT plugins for flight-crew scheduling.

Each plugin subclasses the domain spec of the same name and only adds ``build()``;
its default level and weight are read from ``flight_crew.constraint_info()`` (the
C++ components' kDefaultLevel / kDefaultWeight), never restated here.

Every plugin adds one term per employee (the engine's unit), idle ones included,
so a pick's ``offset`` / ``clip`` shape each employee on its own exactly as the
engine does (``terms_per_unit``), and a constraint picked several times adds one
set of terms per pick.
"""

from dataclasses import dataclass, field
from datetime import timedelta
from typing import Any

from cpsat_plugins import ConstraintPlugin, ConstraintRegistry, ObjectiveBuilder
from flight_crew import constraints as domain
from flight_crew.models import FlightCrewScheduleModel
from ortools.sat.python import cp_model


@dataclass
class FlightCrewContext:
    model: cp_model.CpModel
    problem: FlightCrewScheduleModel
    flights: list[Any]
    x: dict[tuple[int, int], Any]
    first: dict[tuple[int, int], Any]
    last: dict[tuple[int, int], Any]
    transitions: dict[tuple[int, int, int], Any]
    _and_cache: dict[tuple[int, int, int], Any] = field(default_factory=dict)

    def assigned_to_both(self, left: int, right: int, employee: int) -> Any:
        key = (left, right, employee)
        if key not in self._and_cache:
            both = self.model.new_bool_var(f"conflict_{left}_{right}_{employee}")
            self.model.add(both <= self.x[left, employee])
            self.model.add(both <= self.x[right, employee])
            self.model.add(both >= self.x[left, employee] + self.x[right, employee] - 1)
            self._and_cache[key] = both
        return self._and_cache[key]


class RequiredSkillCost(domain.RequiredSkillCost, ConstraintPlugin[FlightCrewContext]):
    terms_per_unit = True  # one term per employee: its seats without the skill

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for employee, employee_data in enumerate(context.problem.employees):
            missing = [
                int(assignment_data.required_skill not in employee_data.skills)
                for assignment_data in context.problem.flight_assignments
            ]
            objective.add(
                self.name,
                sum(
                    flag * context.x[assignment, employee]
                    for assignment, flag in enumerate(missing)
                ),
                bound=sum(missing),
            )


class EmployeeUnavailabilityCost(
    domain.EmployeeUnavailabilityCost, ConstraintPlugin[FlightCrewContext]
):
    terms_per_unit = True  # one term per employee: its seats on unavailable days

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        covered = [
            {
                flight.departure_utc_date_time.date() + timedelta(days=offset)
                for offset in range(
                    (
                        flight.arrival_utc_date_time.date()
                        - flight.departure_utc_date_time.date()
                    ).days
                    + 1
                )
            }
            for flight in context.flights
        ]
        for employee, employee_data in enumerate(context.problem.employees):
            unavailable = [
                int(days & set(employee_data.unavailable_days) != set())
                for days in covered
            ]
            objective.add(
                self.name,
                sum(
                    flag * context.x[assignment, employee]
                    for assignment, flag in enumerate(unavailable)
                ),
                bound=sum(unavailable),
            )


class FlightConflictCost(
    domain.FlightConflictCost, ConstraintPlugin[FlightCrewContext]
):
    terms_per_unit = True  # one term per employee: its overlapping pairs

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        overlapping = [
            (left, right)
            for left in range(len(context.flights))
            for right in range(left + 1, len(context.flights))
            if context.flights[left].departure_utc_date_time
            < context.flights[right].arrival_utc_date_time
            and context.flights[right].departure_utc_date_time
            < context.flights[left].arrival_utc_date_time
        ]
        for employee in range(len(context.problem.employees)):
            objective.add(
                self.name,
                sum(
                    context.assigned_to_both(left, right, employee)
                    for left, right in overlapping
                ),
                bound=len(overlapping),
            )


class TransferBetweenTwoFlightsCost(
    domain.TransferBetweenTwoFlightsCost, ConstraintPlugin[FlightCrewContext]
):
    terms_per_unit = True  # one term per employee: its broken connections

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        broken: dict[int, list[Any]] = {
            employee: [] for employee in range(len(context.problem.employees))
        }
        for (left, right, employee), arc in context.transitions.items():
            if (
                context.flights[left].arrival_airport
                != context.flights[right].departure_airport
            ):
                broken[employee].append(arc)
        # A chronological path over n duties uses at most n - 1 transitions.
        steps = max(0, len(context.flights) - 1)
        for arcs in broken.values():
            objective.add(self.name, sum(arcs), bound=min(len(arcs), steps))


class FirstAssignmentNotDepartingFromHomeCost(
    domain.FirstAssignmentNotDepartingFromHomeCost,
    ConstraintPlugin[FlightCrewContext],
):
    terms_per_unit = True  # one term per employee: 0 or 1 (one first duty)

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        _per_employee_end(
            self.name,
            context,
            objective,
            context.first,
            lambda flight: flight.departure_airport,
        )


class LastAssignmentNotArrivingAtHomeCost(
    domain.LastAssignmentNotArrivingAtHomeCost,
    ConstraintPlugin[FlightCrewContext],
):
    terms_per_unit = True  # one term per employee: 0 or 1 (one last duty)

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        _per_employee_end(
            self.name,
            context,
            objective,
            context.last,
            lambda flight: flight.arrival_airport,
        )


def _per_employee_end(name, context, objective, literals, airport_of) -> None:
    """One term per employee: its first (or last) duty is away from home."""
    for employee, employee_data in enumerate(context.problem.employees):
        away = [
            literal
            for (assignment, owner), literal in literals.items()
            if owner == employee
            and airport_of(context.flights[assignment]) != employee_data.home_airport
        ]
        objective.add(name, sum(away), bound=min(1, len(away)))


# Plugin order = flight_crew.list_constraints() order.
REGISTRY = ConstraintRegistry(
    [
        RequiredSkillCost(),
        FlightConflictCost(),
        TransferBetweenTwoFlightsCost(),
        EmployeeUnavailabilityCost(),
        FirstAssignmentNotDepartingFromHomeCost(),
        LastAssignmentNotArrivingAtHomeCost(),
    ]
)
