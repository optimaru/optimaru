from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator


class EdgeModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    source: str
    target: str
    weight: int = Field(default=1, ge=0)


class VertexPairModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    first: str
    second: str


class MaxCutProblemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    vertices: list[str] = Field(description="Unique graph vertex ids.")
    edges: list[EdgeModel] = Field(description="Weighted undirected graph edges.")
    fixed_partition: dict[str, int] = Field(
        default_factory=dict,
        description="Vertices pinned to side 0 or 1.",
    )
    same_partition_pairs: list[VertexPairModel] = Field(default_factory=list)
    different_partition_pairs: list[VertexPairModel] = Field(default_factory=list)
    min_partition_size: int | None = Field(default=None, ge=0)
    max_partition_size: int | None = Field(default=None, ge=0)
    partition: dict[str, int] = Field(default_factory=dict, description="Solution side, 0 or 1, for each vertex.")
    cut_weight: int | None = Field(default=None, description="Total weight of edges crossing the partition.")
    score_hard: int | None = None
    score_soft: float | None = None

    @model_validator(mode="after")
    def valid_graph(self) -> MaxCutProblemModel:
        known = set(self.vertices)
        if len(known) != len(self.vertices):
            raise ValueError("vertices must be unique")
        if any(edge.source not in known or edge.target not in known for edge in self.edges):
            raise ValueError("every edge endpoint must be a vertex")
        pair_endpoints = {
            endpoint
            for pair in self.same_partition_pairs + self.different_partition_pairs
            for endpoint in (pair.first, pair.second)
        }
        unknown = (set(self.fixed_partition) | pair_endpoints) - known
        if unknown:
            raise ValueError(f"unknown vertex references: {sorted(unknown)}")
        if any(side not in (0, 1) for side in self.fixed_partition.values()):
            raise ValueError("fixed partition sides must be 0 or 1")
        maximum = len(self.vertices) if self.max_partition_size is None else self.max_partition_size
        minimum = 0 if self.min_partition_size is None else self.min_partition_size
        if minimum > maximum or maximum > len(self.vertices):
            raise ValueError("partition size bounds must fit the vertex count")
        return self

    def to_core(self) -> MaxCutProblemModel:
        return self.model_copy(deep=True)

    @classmethod
    def from_core(cls, problem: MaxCutProblemModel) -> MaxCutProblemModel:
        return problem.model_copy(deep=True)
