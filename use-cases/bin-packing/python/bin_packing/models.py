"""Pydantic JSON contract for the bin-packing domain."""

from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core

__all__ = ["BinPackingProblemModel", "ConflictModel", "ItemModel", "with_initial_solution"]


class ItemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: str = Field(min_length=1, description="Unique item identifier.")
    weight: int = Field(gt=0, description="One-dimensional item weight or size.")
    bin: int = Field(default=-1, ge=-1, description="Assigned zero-based bin index; -1 means unsolved.")
    color: str | None = Field(default=None, description="Optional color; equal colors may be forbidden in one bin.")


class ConflictModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    first: str
    second: str


class BinPackingProblemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    bin_capacity: int = Field(gt=0, description="Maximum load of each identical bin.")
    items: list[ItemModel] = Field(description="Items that must each be assigned exactly once.")
    capacity_weight: int = Field(default=1, ge=0, description="Default weight of the bin_capacity plugin.")
    used_bin_weight: int = Field(default=1, ge=0, description="Default weight of the minimize_bins plugin.")
    color_weight: int = Field(default=1, ge=0)
    conflict_weight: int = Field(default=1, ge=0)
    conflicts: list[ConflictModel] = Field(default_factory=list)
    score_hard: int | None = Field(default=None, description="Negative weighted capacity violation score.")
    score_soft: float | None = Field(default=None, description="Negative weighted bin-count objective score.")
    bins_used: int | None = Field(default=None, ge=0, description="Number of non-empty bins in the solution.")
    bin_loads: list[int] | None = Field(default=None, description="Total item weight in each zero-based bin.")
    warm_start: bool = Field(
        default=False,
        description="Start the search from the bins on `items`; items still at -1 are first-fitted around them.",
    )

    @model_validator(mode="after")
    def validate_ids(self) -> BinPackingProblemModel:
        ids = [item.id for item in self.items]
        if len(ids) != len(set(ids)):
            raise ValueError("item ids must be unique")
        known = set(ids)
        if any(
            pair.first not in known or pair.second not in known or pair.first == pair.second for pair in self.conflicts
        ):
            raise ValueError("conflict endpoints must be two different item ids")
        return self

    def to_core(self) -> _core.BinPackingProblem:
        return _core.BinPackingProblem(
            bin_capacity=self.bin_capacity,
            items=[_core.Item(item.id, item.weight, item.bin, item.color) for item in self.items],
            capacity_weight=self.capacity_weight,
            used_bin_weight=self.used_bin_weight,
            color_weight=self.color_weight,
            conflict_weight=self.conflict_weight,
            conflicts=[(pair.first, pair.second) for pair in self.conflicts],
            warm_start=self.warm_start,
        )

    @classmethod
    def from_core(cls, problem: _core.BinPackingProblem) -> BinPackingProblemModel:
        return cls(
            bin_capacity=problem.bin_capacity,
            items=[ItemModel(id=item.id, weight=item.weight, bin=item.bin, color=item.color) for item in problem.items],
            capacity_weight=problem.capacity_weight,
            used_bin_weight=problem.used_bin_weight,
            color_weight=problem.color_weight,
            conflict_weight=problem.conflict_weight,
            conflicts=[ConflictModel(first=left, second=right) for left, right in problem.conflicts],
            score_hard=problem.score.hard,
            score_soft=problem.score.soft,
            bins_used=problem.bins_used(),
            bin_loads=problem.bin_loads(),
            warm_start=problem.warm_start,
        )


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape; items are matched by id. An item the solution
    leaves out, or holds at -1, is first-fitted around the seeded bins before the search starts. Bins run from 0 to
    one below the item count, which is every bin a solve can return.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("items"), list):
        raise ValueError("solution must be a problem model or a dict with an 'items' list")
    known = {item.id for item in problem.items}
    bins: dict[str, int] = {}
    for entry in solution["items"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or entry["id"] not in known:
            raise ValueError(f"solution references unknown item: {entry!r}")
        item_id = entry["id"]
        if item_id in bins:
            raise ValueError(f"solution assigns item {item_id!r} more than once")
        bin_index = entry.get("bin", -1)
        if type(bin_index) is not int or not -1 <= bin_index < len(problem.items):
            raise ValueError(f"item {item_id!r} has bin {bin_index!r}, outside -1..{len(problem.items) - 1}")
        bins[item_id] = bin_index
    seeded = problem.model_copy(deep=True)
    for item in seeded.items:
        item.bin = bins.get(item.id, -1)
    seeded.bins_used = seeded.bin_loads = seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded
