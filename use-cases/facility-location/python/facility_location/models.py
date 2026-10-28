"""엔진 도메인의 pydantic v2 미러 — JSON 직렬화/역직렬화용.

`_core` 의 Location/Facility/Consumer/FacilityLocationProblem 을 1:1 로
미러링한다. 엔진 형태 그대로를 저장/전송할 때(디버그 덤프, 회귀 스냅샷 등)
쓴다::

    from facility_location.models import FacilityLocationProblemModel

    model = FacilityLocationProblemModel.from_core(problem)  # 엔진 -> pydantic
    clone = FacilityLocationProblemModel.model_validate_json(model.model_dump_json())
    problem2 = clone.to_core()                               # pydantic -> 엔진

`pydantic` 은 선택 의존성이다 (`pip install facility_location[models]`).
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
    """위경도(도 단위). 거리는 1도 = 111 km 유클리드 근사."""

    model_config = ConfigDict(extra="forbid")

    latitude: float
    longitude: float

    def to_core(self) -> "_core.Location":
        return _core.Location(self.latitude, self.longitude)

    @classmethod
    def from_core(cls, location: "_core.Location") -> "LocationModel":
        return cls(latitude=location.latitude, longitude=location.longitude)


class FacilityModel(BaseModel):
    """설치 후보 시설: 설치비와 용량을 가진 problem fact."""

    model_config = ConfigDict(extra="forbid")

    id: str
    location: LocationModel
    setup_cost: int = Field(ge=0)
    capacity: int = Field(ge=0)


class ConsumerModel(BaseModel):
    """planning entity. facility 는 시설 인덱스(-1 = 미배정)."""

    model_config = ConfigDict(extra="forbid")

    id: str
    location: LocationModel
    demand: int = Field(ge=0)
    facility: int = -1


class FacilityLocationProblemModel(BaseModel):
    """planning solution 미러. score 는 읽기 전용 결과라 별도 필드로 둔다."""

    model_config = ConfigDict(extra="forbid")

    facilities: list[FacilityModel]
    consumers: list[ConsumerModel]
    south_west_corner: LocationModel = Field(default_factory=lambda: LocationModel(latitude=-90.0, longitude=-180.0))
    north_east_corner: LocationModel = Field(default_factory=lambda: LocationModel(latitude=90.0, longitude=180.0))
    # Java 솔루션의 ConstraintWeightOverrides 미러: 거리 가중 기본 6 (provider
    # 기본 5 를 솔루션이 6 으로 오버라이드), 설치비 가중은 provider 고정 2.
    setup_cost_weight: int = Field(default=2, ge=0)
    distance_from_facility_weight: int = Field(default=6, ge=0)
    score_hard: int | None = None
    score_soft: float | None = None

    def to_core(self) -> "_core.FacilityLocationProblem":
        """pydantic -> 엔진 객체 (score 필드는 엔진이 다시 계산하므로 무시)."""
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
        """엔진 객체 -> pydantic (현재 배정과 score 포함)."""
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
