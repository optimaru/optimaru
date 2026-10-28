"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Location/Facility/Consumer/FacilityLocationProblem 1:1. Use it
to store or transfer the engine shape as-is (debug dumps, regression snapshots,
...)::

    from facility_location.models import FacilityLocationProblemModel

    model = FacilityLocationProblemModel.from_core(problem)  # engine -> pydantic
    clone = FacilityLocationProblemModel.model_validate_json(model.model_dump_json())
    problem2 = clone.to_core()                               # pydantic -> engine

`pydantic` is an optional dependency (`pip install facility_location[models]`).
"""

from pydantic import BaseModel, ConfigDict, Field

from . import _core

__all__ = [
    "ConsumerModel",
    "FacilityLocationProblemModel",
    "FacilityModel",
    "LocationModel",
]


class LocationModel(BaseModel):
    """Latitude/longitude (degrees). Distance uses the 1 degree = 111 km Euclidean approximation."""

    model_config = ConfigDict(extra="forbid")

    latitude: float
    longitude: float

    def to_core(self) -> "_core.Location":
        return _core.Location(self.latitude, self.longitude)

    @classmethod
    def from_core(cls, location: "_core.Location") -> "LocationModel":
        return cls(latitude=location.latitude, longitude=location.longitude)


class FacilityModel(BaseModel):
    """Candidate facility: a problem fact with a setup cost and a capacity."""

    model_config = ConfigDict(extra="forbid")

    id: str
    location: LocationModel
    setup_cost: int = Field(ge=0)
    capacity: int = Field(ge=0)


class ConsumerModel(BaseModel):
    """Planning entity. facility is a facility index (-1 = unassigned)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    location: LocationModel
    demand: int = Field(ge=0)
    facility: int = -1


class FacilityLocationProblemModel(BaseModel):
    """Planning solution mirror. score is a read-only result, kept as a separate field."""

    model_config = ConfigDict(extra="forbid")

    facilities: list[FacilityModel]
    consumers: list[ConsumerModel]
    south_west_corner: LocationModel = Field(default_factory=lambda: LocationModel(latitude=-90.0, longitude=-180.0))
    north_east_corner: LocationModel = Field(default_factory=lambda: LocationModel(latitude=90.0, longitude=180.0))
    # Mirrors the Java solution's ConstraintWeightOverrides: distance weight defaults
    # to 6 (the solution overrides the provider's 5); setup cost weight is the provider's fixed 2.
    setup_cost_weight: int = Field(default=2, ge=0)
    distance_from_facility_weight: int = Field(default=6, ge=0)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.FacilityLocationProblem":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        problem = _core.FacilityLocationProblem(
            [_core.Facility(f.id, f.location.to_core(), f.setup_cost, f.capacity) for f in self.facilities],
            [self._consumer_to_core(c) for c in self.consumers],
            self.south_west_corner.to_core(),
            self.north_east_corner.to_core(),
        )
        problem.setup_cost_weight = self.setup_cost_weight
        problem.distance_from_facility_weight = self.distance_from_facility_weight
        return problem

    @staticmethod
    def _consumer_to_core(c: ConsumerModel) -> "_core.Consumer":
        consumer = _core.Consumer(c.id, c.location.to_core(), c.demand)
        consumer.facility = c.facility
        return consumer

    @classmethod
    def from_core(cls, problem: "_core.FacilityLocationProblem") -> "FacilityLocationProblemModel":
        """engine object -> pydantic (including the current assignment and score)."""
        return cls(
            facilities=[
                FacilityModel(
                    id=f.id,
                    location=LocationModel.from_core(f.location),
                    setup_cost=f.setup_cost,
                    capacity=f.capacity,
                )
                for f in problem.facilities
            ],
            consumers=[
                ConsumerModel(
                    id=c.id,
                    location=LocationModel.from_core(c.location),
                    demand=c.demand,
                    facility=c.facility,
                )
                for c in problem.consumers
            ],
            south_west_corner=LocationModel.from_core(problem.south_west_corner),
            north_east_corner=LocationModel.from_core(problem.north_east_corner),
            setup_cost_weight=problem.setup_cost_weight,
            distance_from_facility_weight=problem.distance_from_facility_weight,
            score_hard=problem.score.hard,
            score_soft=problem.score.soft,
        )
