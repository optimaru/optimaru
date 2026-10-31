"""Pydantic models of the knapsack problem — the canonical JSON contract.

`KnapsackProblemModel` is the JSON contract (items by id); `to_core()` builds the
engine object (`knapsack._core.KnapsackProblem`, items by index) and
`from_core()` builds the model back, selection and results included::

    core = KnapsackProblemModel.model_validate(data).to_core()
    knapsack.solve(core, time_limit_seconds=5)
    result = KnapsackProblemModel.from_core(core)

`knapsack.solve(model)` does the same round trip and writes the result back into
the model in place.
"""

from __future__ import annotations

from pydantic import BaseModel, ConfigDict, Field, model_validator

from . import _core


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

    def to_core(self) -> _core.KnapsackProblem:
        """pydantic -> engine object: ids become item indices.

        `resource_capacities` becomes the resource list in dict order (the order of
        nothing but the declaration), `requires` the item's `required_items`,
        repeated mandatory / forbidden / required entries stay repeated, a group's
        `max_selected=None` stays None (the group size). `selected` becomes 1 / 0 and
        `warm_start` is copied; the result fields are ignored (the engine recomputes
        them).
        """
        self.unique_ids()  # the fields stay mutable after validation
        index = {item.id: position for position, item in enumerate(self.items)}
        return _core.KnapsackProblem(
            self.capacity,
            [
                _core.Item(
                    item.id,
                    item.weight,
                    item.value,
                    dict(item.resource_usage),
                    [index[required] for required in item.requires],
                    1 if item.selected else 0,
                )
                for item in self.items
            ],
            [_core.Resource(resource, capacity) for resource, capacity in self.resource_capacities.items()],
            [index[item_id] for item_id in self.mandatory_items],
            [index[item_id] for item_id in self.forbidden_items],
            [
                _core.SelectionGroup(
                    group.id, [index[item_id] for item_id in group.item_ids], group.min_selected, group.max_selected
                )
                for group in self.selection_groups
            ],
            self.warm_start,
        )

    @classmethod
    def from_core(cls, problem: _core.KnapsackProblem) -> KnapsackProblemModel:
        """engine object -> pydantic: the facts, the selection (an undecided item, -1, is
        not selected), `warm_start` and the results (`total_weight`, `total_value` from
        the selection; the score last computed on the core, 0hard/0soft if it was never
        scored)."""
        ids = [item.id for item in problem.items]
        return cls(
            capacity=problem.capacity,
            items=[
                ItemModel(
                    id=item.id,
                    weight=item.weight,
                    value=item.value,
                    resource_usage=dict(item.resource_usage),
                    requires=[ids[required] for required in item.required_items],
                    selected=item.selected == 1,
                )
                for item in problem.items
            ],
            resource_capacities={resource.id: resource.capacity for resource in problem.resources},
            mandatory_items=[ids[index] for index in problem.mandatory_items],
            forbidden_items=[ids[index] for index in problem.forbidden_items],
            selection_groups=[
                SelectionGroupModel(
                    id=group.id,
                    item_ids=[ids[index] for index in group.items],
                    min_selected=group.min_selected,
                    max_selected=group.max_selected,
                )
                for group in problem.selection_groups
            ],
            total_weight=problem.total_weight(),
            total_value=problem.total_value(),
            score_hard=problem.score.hard,
            score_soft=problem.score.soft,
            warm_start=problem.warm_start,
        )

    def _update_from_core(self, problem: _core.KnapsackProblem) -> None:
        """Write a solved core's selection, results and `warm_start` back into this model, in place."""
        for item, selected in zip(self.items, problem.assignment(), strict=True):
            item.selected = selected == 1
        self.total_weight = problem.total_weight()
        self.total_value = problem.total_value()
        self.score_hard = problem.score.hard
        self.score_soft = problem.score.soft
        self.warm_start = problem.warm_start


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape; items are matched by id. An explicit False
    means not selected and stays that way. `selected` cannot express "undecided", so an item the solution leaves out
    is run through the greedy selection here (`knapsack._core.greedy_selection`: value density order, weight capacity
    only), around the seeded load, and `solve` then fills in nothing.
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
    fixed = [int(decided[item.id]) if item.id in decided else -1 for item in seeded.items]
    for item, selected in zip(seeded.items, _core.greedy_selection(seeded.to_core(), fixed), strict=True):
        item.selected = selected
    seeded.total_weight = seeded.total_value = seeded.score_hard = seeded.score_soft = None
    seeded.warm_start = True
    return seeded
