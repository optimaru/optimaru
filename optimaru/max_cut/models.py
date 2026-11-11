"""Pydantic models of the max-cut problem — the canonical JSON contract.

`MaxCutProblemModel` is the JSON contract (vertices by id); `to_core()` builds the
engine object (`optimaru.max_cut._core.MaxCutProblem`, vertices by index) and
`from_core()` builds the model back, partition and results included::

    core = MaxCutProblemModel.model_validate(data).to_core()
    max_cut.solve(core, time_limit_seconds=5)
    result = MaxCutProblemModel.from_core(core)

`max_cut.solve(model)` does the same round trip and returns the result as a new
model; the model passed in is not changed.
"""

from __future__ import annotations

from pydantic import BaseModel, Field, model_validator

from .._models import SolveRequest, StrictModel, solve_model_request
from . import _core

__all__ = [
    "EdgeModel",
    "MaxCutProblemModel",
    "MaxCutRequest",
    "VertexPairModel",
    "solve_request",
    "with_initial_solution",
]


class EdgeModel(StrictModel):
    source: str
    target: str
    weight: int = Field(default=1, ge=0)


class VertexPairModel(StrictModel):
    first: str
    second: str


class MaxCutProblemModel(StrictModel):
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
    warm_start: bool = Field(
        default=False,
        description="Start the search from `partition`; vertices it leaves out take the default start.",
    )

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
        # The engine stores one side per vertex, so a partition entry must name
        # a vertex and hold side 0 or 1.
        if stray := set(self.partition) - known:
            raise ValueError(f"partition references unknown vertices: {sorted(stray)}")
        if any(side not in (0, 1) for side in self.partition.values()):
            raise ValueError("partition sides must be 0 or 1")
        return self

    def is_complete(self) -> bool:
        """Whether `partition` gives every vertex a side (scoring needs a complete partition)."""
        return set(self.partition) >= set(self.vertices)

    def to_core(self) -> _core.MaxCutProblem:
        """pydantic -> engine object: ids become vertex indices (positions in `vertices`).

        `fixed_partition` becomes the per-vertex `fixed_side` (-1 = free), `partition`
        the per-vertex side (`assignment()`, -1 = left out), the size bounds stay None
        when not given, repeated edges and pairs stay repeated; `warm_start` is copied.
        The result fields are ignored (the engine recomputes them).
        """
        self.valid_graph()  # the fields stay mutable after validation
        index = {vertex: position for position, vertex in enumerate(self.vertices)}
        return _core.MaxCutProblem(
            list(self.vertices),
            [_core.Edge(index[edge.source], index[edge.target], edge.weight) for edge in self.edges],
            [self.fixed_partition.get(vertex, -1) for vertex in self.vertices],
            [_core.VertexPair(index[pair.first], index[pair.second]) for pair in self.same_partition_pairs],
            [_core.VertexPair(index[pair.first], index[pair.second]) for pair in self.different_partition_pairs],
            self.min_partition_size,
            self.max_partition_size,
            [self.partition.get(vertex, -1) for vertex in self.vertices],
            self.warm_start,
        )

    @classmethod
    def from_core(cls, problem: _core.MaxCutProblem) -> MaxCutProblemModel:
        """engine object -> pydantic: the facts, the partition (vertices on side -1 are
        left out), `warm_start` and the results (`cut_weight` and the score once the core
        was scored by `calculate_score` or a solve, else None)."""
        ids = list(problem.vertex_ids)
        model = cls(
            vertices=ids,
            edges=[
                EdgeModel(source=ids[edge.source], target=ids[edge.target], weight=edge.weight)
                for edge in problem.edges
            ],
            fixed_partition={ids[index]: side for index, side in enumerate(problem.fixed_side) if side >= 0},
            same_partition_pairs=[
                VertexPairModel(first=ids[pair.first], second=ids[pair.second]) for pair in problem.same_partition_pairs
            ],
            different_partition_pairs=[
                VertexPairModel(first=ids[pair.first], second=ids[pair.second])
                for pair in problem.different_partition_pairs
            ],
            min_partition_size=problem.min_partition_size,
            max_partition_size=problem.max_partition_size,
        )
        model._update_from_core(problem)
        return model

    def _update_from_core(self, problem: _core.MaxCutProblem) -> None:
        """Write a core's partition, results and `warm_start` back into this model, in place."""
        self.partition = {
            vertex: side for vertex, side in zip(self.vertices, problem.assignment(), strict=True) if side >= 0
        }
        if problem.scored:
            self.cut_weight = problem.cut_weight()
            self.score_hard = problem.score.hard
            self.score_soft = problem.score.soft
        else:
            self.cut_weight = self.score_hard = self.score_soft = None
        self.warm_start = problem.warm_start


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape; vertices are matched by id. A vertex the
    solution leaves out takes the default start: its `fixed_partition` side, otherwise the alternating side. Anything
    `solve` returns is accepted, feasible or not. With exact size bounds the search only swaps sides, so a seed of the
    wrong size, complete or filled in, keeps that size and its hard penalty. A side that contradicts `fixed_partition`
    raises ValueError: `solve` never returns one, and the search never moves a fixed vertex.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("partition"), dict):
        raise ValueError("solution must be a problem model or a dict with a 'partition' mapping")
    known = set(problem.vertices)
    for vertex, side in solution["partition"].items():
        if vertex not in known:
            raise ValueError(f"solution references unknown vertex: {vertex!r}")
        if type(side) is not int or side not in (0, 1):
            raise ValueError(f"vertex {vertex!r} has side {side!r}, expected 0 or 1")
        if problem.fixed_partition.get(vertex, side) != side:
            raise ValueError(
                f"vertex {vertex!r} is fixed to side {problem.fixed_partition[vertex]} but seeded on side {side}; "
                "the search never moves a fixed vertex"
            )
    seeded = problem.model_copy(deep=True)
    seeded.partition = dict(solution["partition"])
    seeded.cut_weight = seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded


class MaxCutRequest(SolveRequest):
    """The max-cut problem with the settings of one solve (see `SolveRequest` for those)."""

    model_input: MaxCutProblemModel


def solve_request(request: MaxCutRequest, *, constraints=None) -> MaxCutProblemModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)
