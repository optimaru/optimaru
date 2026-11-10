"""Pydantic v2 mirror of the engine domain — for JSON serialization/deserialization.

Mirrors `_core`'s Location/Facility/Consumer/FacilityLocationProblem 1:1. Use it
to store or transfer the engine shape as-is (debug dumps, regression snapshots,
...)::

    from optimaru.facility_location.models import FacilityLocationProblemModel

    model = FacilityLocationProblemModel.from_core(problem)  # engine -> pydantic
    clone = FacilityLocationProblemModel.model_validate_json(model.model_dump_json())
    problem2 = clone.to_core()                               # pydantic -> engine

`pydantic` is an optional dependency (`pip install facility_location[models]`).
"""

from pydantic import Field, model_validator

from .._models import StrictModel, entities_by_id, require_same_order, solution_dict
from . import _core

__all__ = [
    "ConsumerModel",
    "FacilityLocationProblemModel",
    "FacilityModel",
    "LocationModel",
    "with_initial_solution",
]


class LocationModel(StrictModel):
    """Latitude/longitude (degrees). Distance uses the 1 degree = 111 km Euclidean approximation."""

    latitude: float
    longitude: float

    def to_core(self) -> "_core.Location":
        return _core.Location(self.latitude, self.longitude)

    @classmethod
    def from_core(cls, location: "_core.Location") -> "LocationModel":
        return cls(latitude=location.latitude, longitude=location.longitude)


class FacilityModel(StrictModel):
    """Candidate facility: a problem fact with a setup cost and a capacity."""

    id: str
    location: LocationModel
    setup_cost: int = Field(ge=0)
    capacity: int = Field(ge=0)


class ConsumerModel(StrictModel):
    """Facility is a facility index (-1 = unassigned)."""

    id: str
    location: LocationModel
    demand: int = Field(ge=0)
    facility: int = -1


class FacilityLocationProblemModel(StrictModel):
    """Solution mirror. score is a read-only result, kept as a separate field.

    The canonical problem carries no constraint weights: weights
    come only from constraint selections, and the defaults (setup 2, distance 6)
    are the C++ components' (``facility_location.constraint_info()``).
    """

    facilities: list[FacilityModel]
    consumers: list[ConsumerModel]
    south_west_corner: LocationModel = Field(default_factory=lambda: LocationModel(latitude=-90.0, longitude=-180.0))
    north_east_corner: LocationModel = Field(default_factory=lambda: LocationModel(latitude=90.0, longitude=180.0))
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def _check_indices(self):
        """`facility` is a raw position the engine reads without a range check."""
        value_count = len(self.facilities)
        for consumer in self.consumers:
            if not -1 <= consumer.facility < value_count:
                raise ValueError(
                    f"consumer {consumer.id!r}: facility {consumer.facility!r} is out of range -1..{value_count - 1}"
                )
        return self

    def to_core(self) -> "_core.FacilityLocationProblem":
        """pydantic -> engine object (the score field is ignored; the engine recomputes it)."""
        self._check_indices()  # the fields stay mutable after validation
        return _core.FacilityLocationProblem(
            [_core.Facility(f.id, f.location.to_core(), f.setup_cost, f.capacity) for f in self.facilities],
            [self._consumer_to_core(c) for c in self.consumers],
            self.south_west_corner.to_core(),
            self.north_east_corner.to_core(),
        )

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
            score_hard=problem.score.hard,
            score_soft=problem.score.soft,
        )


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem (`FacilityLocationProblemModel.from_core`), or a
    plain dict of that shape (the JSON the API returned). Consumers are matched by id.
    A consumer left out becomes unassigned and the construction heuristic places it,
    and only it, so a complete solution starts the search exactly from it. `problem`
    is not mutated.

    `facility` is a position in `facilities`, so it is only valid for the same
    facilities in the same order: ValueError if `solution` lists different ones.

    Raises ValueError naming the consumer for an unknown or repeated id and for a
    `facility` outside -1..facility_count - 1 (the engine does not range-check).
    """
    if not isinstance(problem, FacilityLocationProblemModel):
        raise TypeError(f"problem must be a FacilityLocationProblemModel: {type(problem)!r}")
    solved = solution_dict(solution)
    require_same_order(solved, "facilities", [f.id for f in problem.facilities])
    consumers = entities_by_id(solved, "consumers", [c.id for c in problem.consumers])
    value_count = len(problem.facilities)
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for consumer in seeded.consumers:
        value = consumers.get(consumer.id, {}).get("facility", -1)
        if not isinstance(value, int) or isinstance(value, bool) or not -1 <= value < value_count:
            raise ValueError(f"consumer {consumer.id!r}: facility {value!r} is out of range -1..{value_count - 1}")
        consumer.facility = value
    return seeded
