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
