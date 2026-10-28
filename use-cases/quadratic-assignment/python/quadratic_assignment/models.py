from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator


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
    fixed_assignments: dict[str, str] = Field(default_factory=dict)
    forbidden_assignments: list[ForbiddenAssignmentModel] = Field(default_factory=list)
    facility_demands: dict[str, int] = Field(default_factory=dict)
    location_capacities: dict[str, int] = Field(default_factory=dict)
    assignment: dict[str, str] = Field(default_factory=dict, description="Solution mapping from location to facility.")
    total_cost: int | None = Field(default=None, description="Sum of flow multiplied by assigned distance.")
    score_hard: int | None = None
    score_soft: float | None = None

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

    def to_core(self) -> QuadraticAssignmentProblemModel:
        return self.model_copy(deep=True)

    @classmethod
    def from_core(cls, problem: QuadraticAssignmentProblemModel) -> QuadraticAssignmentProblemModel:
        return problem.model_copy(deep=True)
