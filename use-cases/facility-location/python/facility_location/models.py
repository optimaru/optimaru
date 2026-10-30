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
    "with_initial_solution",
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


def _solution_dict(solution) -> dict:
    if isinstance(solution, BaseModel):
        return solution.model_dump()
    if isinstance(solution, dict):
        return solution
    raise ValueError(f"solution must be a solved model or a dict of that shape: {type(solution)!r}")


def _entities_by_id(solved: dict, name: str, ids: list, key: str = "id") -> dict:
    """`solved[name]` by id. Rejects an id repeated on either side and an id outside `ids`."""
    entities = solved.get(name)
    if not isinstance(entities, list):
        raise ValueError(f"solution has no {name!r} list")
    known = set(ids)
    if len(known) != len(ids):
        raise ValueError(f"problem repeats an id in {name!r}: {next(i for i in ids if ids.count(i) > 1)!r}")
    by_id: dict = {}
    for entity in entities:
        entity_id = entity.get(key) if isinstance(entity, dict) else None
        if not isinstance(entity_id, (str, int)) or entity_id not in known:
            raise ValueError(f"solution has an unknown id in {name!r}: {entity_id!r}")
        if entity_id in by_id:
            raise ValueError(f"solution repeats an id in {name!r}: {entity_id!r}")
        by_id[entity_id] = entity
    return by_id


def _require_same_order(solved: dict, name: str, ids: list, key: str = "id") -> None:
    """A positional value only means the same thing over the same list. `solved` may omit the list."""
    values = solved.get(name)
    if values is None:
        return
    if not isinstance(values, list) or [v.get(key) if isinstance(v, dict) else None for v in values] != ids:
        raise ValueError(f"solution {name!r} list differs from the problem's (same entries, same order required)")


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is what a solve returns (`FacilityLocationProblemModel.from_core`), or a
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
    solved = _solution_dict(solution)
    _require_same_order(solved, "facilities", [f.id for f in problem.facilities])
    consumers = _entities_by_id(solved, "consumers", [c.id for c in problem.consumers])
    value_count = len(problem.facilities)
    seeded = problem.model_copy(deep=True)
    seeded.score_hard = seeded.score_soft = None
    for consumer in seeded.consumers:
        value = consumers.get(consumer.id, {}).get("facility", -1)
        if not isinstance(value, int) or isinstance(value, bool) or not -1 <= value < value_count:
            raise ValueError(f"consumer {consumer.id!r}: facility {value!r} is out of range -1..{value_count - 1}")
        consumer.facility = value
    return seeded
