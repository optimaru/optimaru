"""Pydantic models of the quadratic assignment problem — the canonical JSON contract.

`QuadraticAssignmentProblemModel` is the JSON contract (facilities and locations
by id); `to_core()` builds the engine object
(`optimaru.quadratic_assignment._core.QuadraticAssignmentProblem`, by index) and
`from_core()` builds the model back, assignment and results included::

    core = QuadraticAssignmentProblemModel.model_validate(data).to_core()
    quadratic_assignment.solve(core, time_limit_seconds=5)
    result = QuadraticAssignmentProblemModel.from_core(core)

`quadratic_assignment.solve(model)` does the same round trip and returns the result
as a new model; the model passed in is not changed.
"""

from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core


class ForbiddenAssignmentModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    facility: str
    location: str


class QuadraticAssignmentProblemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    facilities: list[str] = Field(description="Facilities that must each be assigned once.")
    locations: list[str] = Field(description="Locations that must each receive one facility.")
    flows: list[list[int]] = Field(description="Facility-to-facility flow matrix.")
    distances: list[list[int]] = Field(description="Location-to-location distance matrix.")
    assignment_costs: list[list[int]] | None = Field(
        default=None,
        description="Optional direct facility-to-location assignment cost matrix.",
    )
    fixed_assignments: dict[str, str] = Field(
        default_factory=dict,
        description="Location -> facility that location must hold (scored by pinned_assignments_cost).",
    )
    forbidden_assignments: list[ForbiddenAssignmentModel] = Field(
        default_factory=list,
        description="Facility-location pairs that must not be used (scored by forbidden_assignments_cost).",
    )
    facility_demands: dict[str, int] = Field(
        default_factory=dict,
        description="Demand per facility; a facility not listed has demand 0.",
    )
    location_capacities: dict[str, int] = Field(
        default_factory=dict,
        description="Capacity per location; a location not listed has no capacity limit.",
    )
    assignment: dict[str, str] = Field(default_factory=dict, description="Solution mapping from location to facility.")
    total_cost: int | None = Field(
        default=None,
        description="Flow-distance plus direct assignment cost of the assignment, raw.",
    )
    score_hard: int | None = None
    score_soft: float | None = None
    warm_start: bool = Field(default=False, description="Start the search from `assignment`, a full bijection.")

    @model_validator(mode="after")
    def square_matrices(self) -> QuadraticAssignmentProblemModel:
        size = len(self.facilities)
        if size != len(self.locations) or any(len(row) != size for row in self.flows + self.distances):
            raise ValueError("facilities, locations, flows, and distances must have the same square size")
        if self.assignment_costs is not None and (
            len(self.assignment_costs) != size or any(len(row) != size for row in self.assignment_costs)
        ):
            raise ValueError("assignment_costs must be a facility-by-location square matrix")
        facilities = set(self.facilities)
        locations = set(self.locations)
        if len(facilities) != size or len(locations) != size:
            raise ValueError("facility and location ids must be unique")
        if unknown := set(self.fixed_assignments) - locations:
            raise ValueError(f"unknown fixed locations: {sorted(unknown)}")
        if unknown := set(self.fixed_assignments.values()) - facilities:
            raise ValueError(f"unknown fixed facilities: {sorted(unknown)}")
        if any(row.facility not in facilities or row.location not in locations for row in self.forbidden_assignments):
            raise ValueError("forbidden assignments must reference known facilities and locations")
        if set(self.facility_demands) - facilities or set(self.location_capacities) - locations:
            raise ValueError("demand and capacity keys must reference known ids")
        if any(value < 0 for value in (*self.facility_demands.values(), *self.location_capacities.values())):
            raise ValueError("demands and capacities must be non-negative")
        return self

    def missing_locations(self) -> list[str]:
        """The locations `assignment` leaves empty, in location order."""
        return [location for location in self.locations if location not in self.assignment]

    def to_core(self) -> _core.QuadraticAssignmentProblem:
        """pydantic -> engine object: ids become indices.

        `facility_demands` / `location_capacities` become the facility's `demand` /
        the location's `capacity` (None where the dict has no key: demand 0 / no
        limit), `fixed_assignments` a `FixedAssignment` list in dict order,
        `forbidden_assignments` a `ForbiddenAssignment` list as given (repeated rows
        kept; scoring counts a pair once). `assignment` is written into the
        facilities' `location` (a location it leaves out stays empty); an unknown
        id or a facility at two locations raises ValueError, with the words of
        `with_initial_solution`. `warm_start` is copied; the result fields are
        ignored (the engine recomputes them).
        """
        self.square_matrices()  # the fields stay mutable after validation
        facility_index = {facility: index for index, facility in enumerate(self.facilities)}
        location_index = {location: index for index, location in enumerate(self.locations)}
        facilities = [_core.Facility(facility, self.facility_demands.get(facility)) for facility in self.facilities]
        seen: dict[str, str] = {}
        for location, facility in self.assignment.items():
            if location not in location_index:
                raise ValueError(f"assignment references unknown location: {location!r}")
            if not isinstance(facility, str) or facility not in facility_index:
                raise ValueError(f"location {location!r} references unknown facility: {facility!r}")
            if facility in seen:
                raise ValueError(f"facility {facility!r} is assigned to both {seen[facility]!r} and {location!r}")
            seen[facility] = location
            facilities[facility_index[facility]].location = location_index[location]
        return _core.QuadraticAssignmentProblem(
            facilities,
            [_core.Location(location, self.location_capacities.get(location)) for location in self.locations],
            [list(row) for row in self.flows],
            [list(row) for row in self.distances],
            None if self.assignment_costs is None else [list(row) for row in self.assignment_costs],
            [
                _core.FixedAssignment(location_index[location], facility_index[facility])
                for location, facility in self.fixed_assignments.items()
            ],
            [
                _core.ForbiddenAssignment(facility_index[row.facility], location_index[row.location])
                for row in self.forbidden_assignments
            ],
            self.warm_start,
        )

    @classmethod
    def from_core(cls, problem: _core.QuadraticAssignmentProblem) -> QuadraticAssignmentProblemModel:
        """engine object -> pydantic: the facts, the assignment (in location order;
        an empty location is left out), `warm_start` and — when the assignment is a
        full bijection — the results: `total_cost` from the assignment, and
        `score_hard` / `score_soft` from the score last computed on the core (None
        while the core has not been scored)."""
        facilities = problem.facilities
        locations = problem.locations
        model = cls(
            facilities=[facility.id for facility in facilities],
            locations=[location.id for location in locations],
            flows=[list(row) for row in problem.flows],
            distances=[list(row) for row in problem.distances],
            assignment_costs=None
            if problem.assignment_costs is None
            else [list(row) for row in problem.assignment_costs],
            fixed_assignments={
                locations[fixed.location].id: facilities[fixed.facility].id for fixed in problem.fixed_assignments
            },
            forbidden_assignments=[
                ForbiddenAssignmentModel(facility=facilities[row.facility].id, location=locations[row.location].id)
                for row in problem.forbidden_assignments
            ],
            facility_demands={facility.id: facility.demand for facility in facilities if facility.demand is not None},
            location_capacities={
                location.id: location.capacity for location in locations if location.capacity is not None
            },
            warm_start=problem.warm_start,
        )
        model._update_from_core(problem)
        return model

    def _update_from_core(self, problem: _core.QuadraticAssignmentProblem) -> None:
        """Write a core's assignment, results and `warm_start` into this model, in place."""
        ids = [facility.id for facility in problem.facilities]
        self.assignment = {
            location: ids[facility]
            for location, facility in zip(self.locations, problem.assignment(), strict=True)
            if facility >= 0
        }
        if problem.is_bijection():
            self.total_cost = problem.total_cost()
            score = problem.score
            self.score_hard = None if score is None else score.hard
            self.score_soft = None if score is None else score.soft
        else:
            self.total_cost = self.score_hard = self.score_soft = None
        self.warm_start = problem.warm_start


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape. Its `assignment` must map every location to a
    different facility: the search only swaps facilities, so a partial assignment raises ValueError.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("assignment"), dict):
        raise ValueError("solution must be a problem model or a dict with an 'assignment' mapping")
    assignment = solution["assignment"]
    facilities = set(problem.facilities)
    locations = set(problem.locations)
    seen: dict[str, str] = {}
    for location, facility in assignment.items():
        if location not in locations:
            raise ValueError(f"solution references unknown location: {location!r}")
        if not isinstance(facility, str) or facility not in facilities:
            raise ValueError(f"location {location!r} references unknown facility: {facility!r}")
        if facility in seen:
            raise ValueError(f"facility {facility!r} is assigned to both {seen[facility]!r} and {location!r}")
        seen[facility] = location
    if missing := [location for location in problem.locations if location not in assignment]:
        raise ValueError(f"assignment must cover every location; missing: {missing}")
    seeded = problem.model_copy(deep=True)
    seeded.assignment = dict(assignment)
    seeded.total_cost = seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded
