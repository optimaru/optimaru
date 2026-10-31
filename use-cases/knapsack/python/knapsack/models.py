from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator


class ItemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1)
    weight: int = Field(gt=0)
    value: int = Field(ge=0)
    resource_usage: dict[str, int] = Field(
        default_factory=dict,
        description="Additional resource consumption for multidimensional knapsack constraints.",
    )
    requires: list[str] = Field(default_factory=list, description="Items that must also be selected with this item.")
    selected: bool = False


class SelectionGroupModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1)
    item_ids: list[str] = Field(min_length=1)
    min_selected: int = Field(default=0, ge=0)
    max_selected: int | None = Field(default=None, ge=0)

    @model_validator(mode="after")
    def valid_bounds(self) -> SelectionGroupModel:
        if len(set(self.item_ids)) != len(self.item_ids):
            raise ValueError("selection group item ids must be unique")
        maximum = len(self.item_ids) if self.max_selected is None else self.max_selected
        if self.min_selected > maximum or maximum > len(self.item_ids):
            raise ValueError("selection group bounds must fit the number of items")
        return self


class KnapsackProblemModel(BaseModel):
    model_config = ConfigDict(extra="forbid")
    capacity: int = Field(gt=0, description="Maximum total selected weight.")
    items: list[ItemModel] = Field(description="Candidate items with weight and value.")
    resource_capacities: dict[str, int] = Field(
        default_factory=dict,
        description="Capacity by additional resource for multidimensional knapsack variants.",
    )
    mandatory_items: list[str] = Field(default_factory=list)
    forbidden_items: list[str] = Field(default_factory=list)
    selection_groups: list[SelectionGroupModel] = Field(default_factory=list)
    total_weight: int | None = Field(default=None, description="Selected weight in the solution.")
    total_value: int | None = Field(default=None, description="Selected value in the solution.")
    score_hard: int | None = None
    score_soft: float | None = None
    warm_start: bool = Field(
        default=False,
        description="Start the search from `selected` as it stands; False means not selected and nothing is filled in.",
    )

    @model_validator(mode="after")
    def unique_ids(self) -> KnapsackProblemModel:
        ids = {item.id for item in self.items}
        if len(ids) != len(self.items):
            raise ValueError("item ids must be unique")
        referenced = set(self.mandatory_items) | set(self.forbidden_items)
        referenced.update(required for item in self.items for required in item.requires)
        referenced.update(item_id for group in self.selection_groups for item_id in group.item_ids)
        if unknown := referenced - ids:
            raise ValueError(f"unknown item references: {sorted(unknown)}")
        if overlap := set(self.mandatory_items) & set(self.forbidden_items):
            raise ValueError(f"items cannot be both mandatory and forbidden: {sorted(overlap)}")
        if any(value < 0 for value in self.resource_capacities.values()):
            raise ValueError("resource capacities must be non-negative")
        if any(value < 0 for item in self.items for value in item.resource_usage.values()):
            raise ValueError("resource usage must be non-negative")
        return self

    def to_core(self) -> KnapsackProblemModel:
        return self.model_copy(deep=True)

    @classmethod
    def from_core(cls, problem: KnapsackProblemModel) -> KnapsackProblemModel:
        return problem.model_copy(deep=True)


def _greedy_selection(problem, decided=None):
    """Take items by value density while the capacity allows, skipping the ids in `decided`, which are kept as given."""
    decided = decided or {}
    order = sorted(
        range(len(problem.items)), key=lambda i: problem.items[i].value / problem.items[i].weight, reverse=True
    )
    selection = [decided.get(item.id, False) for item in problem.items]
    load = sum(item.weight for item, selected in zip(problem.items, selection, strict=True) if selected)
    for index in order:
        if problem.items[index].id not in decided and load + problem.items[index].weight <= problem.capacity:
            selection[index] = True
            load += problem.items[index].weight
    return selection


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape; items are matched by id. An explicit False
    means not selected and stays that way. `selected` cannot express "undecided", so an item the solution leaves out
    is run through the greedy selection here, around the seeded load, and `solve` then fills in nothing.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("items"), list):
        raise ValueError("solution must be a problem model or a dict with an 'items' list")
    known = {item.id for item in problem.items}
    decided: dict[str, bool] = {}
    seen: set[str] = set()
    for entry in solution["items"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or entry["id"] not in known:
            raise ValueError(f"solution references unknown item: {entry!r}")
        item_id = entry["id"]
        if item_id in seen:
            raise ValueError(f"solution lists item {item_id!r} more than once")
        seen.add(item_id)
        if "selected" not in entry:
            continue
        if not isinstance(entry["selected"], bool):
            raise ValueError(f"item {item_id!r} has invalid selected value {entry['selected']!r}")
        decided[item_id] = entry["selected"]
    seeded = problem.model_copy(deep=True)
    for item, selected in zip(seeded.items, _greedy_selection(seeded, decided), strict=True):
        item.selected = selected
    seeded.total_weight = seeded.total_value = seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded
