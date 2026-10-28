"""Class-based CP-SAT plugins for flight-crew scheduling."""

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


class RequiredSkill(domain.RequiredSkill, ConstraintPlugin[FlightCrewContext]):
    default_level = "hard"
    default_weight = 100

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for assignment, assignment_data in enumerate(
            context.problem.flight_assignments
        ):
            for employee, employee_data in enumerate(context.problem.employees):
                missing = int(
                    assignment_data.required_skill not in employee_data.skills
                )
                objective.add(
                    self.name,
                    missing * context.x[assignment, employee],
                    bound=missing,
                )


class EmployeeUnavailability(
    domain.EmployeeUnavailability, ConstraintPlugin[FlightCrewContext]
):
    default_level = "hard"
    default_weight = 10

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for assignment, flight in enumerate(context.flights):
            covered_days = {
                flight.departure_utc_date_time.date() + timedelta(days=offset)
                for offset in range(
                    (
                        flight.arrival_utc_date_time.date()
                        - flight.departure_utc_date_time.date()
                    ).days
                    + 1
                )
            }
            for employee, employee_data in enumerate(context.problem.employees):
                unavailable = int(
                    covered_days & set(employee_data.unavailable_days) != set()
                )
                objective.add(
                    self.name,
                    unavailable * context.x[assignment, employee],
                    bound=unavailable,
                )


class FlightConflict(domain.FlightConflict, ConstraintPlugin[FlightCrewContext]):
    default_level = "hard"
    default_weight = 10

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for left in range(len(context.flights)):
            for right in range(left + 1, len(context.flights)):
                overlap = (
                    context.flights[left].departure_utc_date_time
                    < context.flights[right].arrival_utc_date_time
                    and context.flights[right].departure_utc_date_time
                    < context.flights[left].arrival_utc_date_time
                )
                if not overlap:
                    continue
                for employee in range(len(context.problem.employees)):
                    objective.add(
                        self.name,
                        context.assigned_to_both(left, right, employee),
                        bound=1,
                    )


class TransferBetweenTwoFlights(
    domain.TransferBetweenTwoFlights, ConstraintPlugin[FlightCrewContext]
):
    default_level = "hard"

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for (left, right, _employee), arc in context.transitions.items():
            invalid = int(
                context.flights[left].arrival_airport
                != context.flights[right].departure_airport
            )
            objective.add(self.name, invalid * arc, bound=invalid)


class FirstAssignmentNotDepartingFromHome(
    domain.FirstAssignmentNotDepartingFromHome,
    ConstraintPlugin[FlightCrewContext],
):
    default_level = "soft"
    default_weight = 1_000

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for (assignment, employee), literal in context.first.items():
            invalid = int(
                context.flights[assignment].departure_airport
                != context.problem.employees[employee].home_airport
            )
            objective.add(self.name, invalid * literal, bound=invalid)


class LastAssignmentNotArrivingAtHome(
    domain.LastAssignmentNotArrivingAtHome,
    ConstraintPlugin[FlightCrewContext],
):
    default_level = "soft"
    default_weight = 1_000

    def build(self, context: FlightCrewContext, objective: ObjectiveBuilder) -> None:
        for (assignment, employee), literal in context.last.items():
            invalid = int(
                context.flights[assignment].arrival_airport
                != context.problem.employees[employee].home_airport
            )
            objective.add(self.name, invalid * literal, bound=invalid)


REGISTRY = ConstraintRegistry(
    [
        RequiredSkill(),
        FlightConflict(),
        TransferBetweenTwoFlights(),
        EmployeeUnavailability(),
        FirstAssignmentNotDepartingFromHome(),
        LastAssignmentNotArrivingAtHome(),
    ]
)
