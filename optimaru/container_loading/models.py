"""Pydantic JSON contract for the container-loading domain.

The problem as JSON, mirroring `_core`'s Container / Item / ContainerLoadingProblem::

    from optimaru.container_loading.models import ContainerLoadingProblemModel

    model = ContainerLoadingProblemModel.model_validate(data)    # JSON -> pydantic
    solved = cl.solve(model, time_limit_seconds=5.0)             # a new, solved model
    for item in solved.items:
        print(item.id, item.container, item.x, item.y, item.z)

Lengths and weights are whole numbers in the caller's unit (the guide's examples use mm
and g). Coordinates: ``x`` runs along the container's ``length`` (0 = the closed end, the
door is at ``x = length``), ``y`` along its ``width``, ``z`` up. An item's ``x, y, z`` is
its corner nearest the origin, ``placed_length, placed_width, placed_height`` its size
along x, y and z after rotation.

The problem carries no constraint weights (``extra="forbid"``): weights come only from
constraint selections, and the defaults are the C++ components'
(``container_loading.constraint_info()``).
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field, model_validator

from .._constraint import ParameterUse
from .._models import SolveRequest, StrictModel, solve_model_request
from . import _core

__all__ = [
    "ContainerLoadingProblemModel",
    "ContainerLoadingRequest",
    "ContainerModel",
    "ItemModel",
    "parameter_uses",
    "solve_request",
    "with_initial_solution",
]

#: The largest length and the largest weight the engine takes (`_core` refuses more).
MAX_LENGTH = 1_000_000
MAX_WEIGHT = 1_000_000_000
#: The largest weight limit (`max_weight`, `max_load_on_top`): what a 64-bit integer holds.
MAX_LIMIT = 2**63 - 1

_ROTATIONS = {"all": _core.Rotation.ALL, "upright": _core.Rotation.UPRIGHT, "fixed": _core.Rotation.FIXED}
_ROTATION_NAMES = {value: name for name, value in _ROTATIONS.items()}


class ItemModel(StrictModel):
    """One box. The fields from `container` on are outputs: a solve fills them in."""

    id: str = Field(min_length=1, description="Unique item identifier.")
    length: int = Field(gt=0, le=MAX_LENGTH, description="Size along x as given, before any rotation.")
    width: int = Field(gt=0, le=MAX_LENGTH, description="Size along y as given.")
    height: int = Field(gt=0, le=MAX_LENGTH, description="Size along z (up) as given.")
    weight: int = Field(default=0, ge=0, le=MAX_WEIGHT, description="Weight, in the caller's unit.")
    rotations: Literal["all", "upright", "fixed"] = Field(
        default="all",
        description=(
            "Which orientations are allowed: all = the six axis-parallel ones; upright = the height "
            "stays vertical (as given, or turned by 90 degrees); fixed = as given."
        ),
    )
    max_load_on_top: int | None = Field(
        default=None, ge=0, le=MAX_LIMIT, description="Weight this item can carry; 0 = nothing on top. None = no limit."
    )
    unload_order: int | None = Field(
        default=None,
        ge=-(10**9),
        le=10**9,
        description="Lower leaves first (a route's stop number). None = no order.",
    )
    group: str | None = Field(
        default=None, description="Items of one group belong in one container (the items of an order)."
    )

    container: str | None = Field(
        default=None, description="Output: id of the container it stands in; None = not placed."
    )
    sequence: int | None = Field(
        default=None, ge=0, description="Output: 0-based place in its container's loading order."
    )
    x: int | None = Field(default=None, ge=0, description="Output: distance of its near corner from the closed end.")
    y: int | None = Field(default=None, ge=0, description="Output: position of its near corner across the width.")
    z: int | None = Field(default=None, ge=0, description="Output: height of its base above the floor.")
    placed_length: int | None = Field(default=None, gt=0, description="Output: size along x after rotation.")
    placed_width: int | None = Field(default=None, gt=0, description="Output: size along y after rotation.")
    placed_height: int | None = Field(default=None, gt=0, description="Output: size along z after rotation.")


class ContainerModel(StrictModel):
    """One physical space: a pallet, a rack bay, a cargo space, one carton.

    The fields from `items` on are outputs: a solve fills them in."""

    id: str = Field(min_length=1, description="Unique container identifier.")
    length: int = Field(gt=0, le=MAX_LENGTH, description="Inner size along x: from the closed end to the door.")
    width: int = Field(gt=0, le=MAX_LENGTH, description="Inner size along y.")
    height: int = Field(gt=0, le=MAX_LENGTH, description="Inner size along z (up).")
    max_weight: int | None = Field(default=None, ge=0, le=MAX_LIMIT, description="Weight it may hold. None = no limit.")
    cost: float = Field(
        default=1.0, ge=0, allow_inf_nan=False, description="What using this container costs (1 = count containers)."
    )
    initial_items: list[str] = Field(
        default_factory=list,
        description=(
            "Warm start: item ids in the loading order to start from. The solve keeps them as its "
            "starting point and constructs only the items no container lists."
        ),
    )

    items: list[str] | None = Field(default=None, description="Output: the placed items, in loading order.")
    unplaced_items: list[str] | None = Field(
        default=None,
        description=(
            "Output: items the solve gave to this container that found no place in it. They count "
            "in the hard score (their weight too, against max_weight), and a solved model loads "
            "them back after `items`."
        ),
    )
    loaded_weight: int | None = Field(
        default=None,
        ge=0,
        description="Output: weight of the placed items (container_weight_cost also counts the unplaced ones).",
    )
    loaded_volume: int | None = Field(default=None, ge=0, description="Output: volume of the placed items.")
    fill_ratio: float | None = Field(default=None, ge=0, description="Output: loaded_volume / the container's volume.")
    used_length: int | None = Field(
        default=None, ge=0, description="Output: largest x + placed_length of a placed item (0 when empty)."
    )

    def loading_order(self) -> list[str]:
        """The loading order this container goes into a solve with: `items` (and the
        `unplaced_items` after them) when a solve has filled them in, else `initial_items`.

        Where an unplaced item stood in the order is not kept: it is tried again after the
        placed ones, which stand where they stood. So a solved model in which everything is
        placed is exactly the solution it was; one with unplaced items can only come back
        with fewer of them, though not necessarily with a lower hard score: an item placed
        now may stand on a fragile item or in front of an earlier stop."""
        if self.items is None:
            return list(self.initial_items)
        return [*self.items, *(self.unplaced_items or [])]


class ContainerLoadingProblemModel(StrictModel):
    """The containers and the items to load into them. The score fields, `containers_used`
    and `unplaced_items` are outputs."""

    containers: list[ContainerModel] = Field(min_length=1, description="The spaces that may be used.")
    items: list[ItemModel] = Field(description="The boxes; each goes into one container.")
    min_support_ratio: float = Field(
        default=0.8,
        gt=0,
        le=1,
        description="Share of an item's base that must rest on the floor or on other items.",
    )
    score_hard: int | None = Field(
        default=None,
        description="Negative weighted hard score (unplaced items, weight and stacking limits, unloading order).",
    )
    score_soft: float | None = Field(
        default=None, description="Negative weighted soft score (cost of the containers used, split groups)."
    )
    containers_used: int | None = Field(default=None, ge=0, description="Containers that hold a placed item.")
    unplaced_items: list[str] | None = Field(default=None, description="Ids of the items that are not placed.")

    @model_validator(mode="after")
    def validate_ids(self) -> ContainerLoadingProblemModel:
        container_ids = [container.id for container in self.containers]
        if len(container_ids) != len(set(container_ids)):
            raise ValueError("container ids must be unique")
        item_ids = [item.id for item in self.items]
        known = set(item_ids)
        if len(item_ids) != len(known):
            raise ValueError("item ids must be unique")
        listed: set[str] = set()
        for container in self.containers:
            for item_id in container.loading_order():
                if item_id not in known:
                    raise ValueError(f"container {container.id!r}: unknown item {item_id!r}")
                if item_id in listed:
                    raise ValueError(
                        f"container {container.id!r}: item {item_id!r} appears twice in the loading orders"
                    )
                listed.add(item_id)
        return self

    def to_core(self) -> _core.ContainerLoadingProblem:
        """pydantic -> engine object. Each container's list is its `loading_order()`: what a
        solve filled in when there is one, else `initial_items`. The other outputs are
        ignored; the engine derives them again."""
        index = {item.id: i for i, item in enumerate(self.items)}
        return _core.ContainerLoadingProblem(
            containers=[
                _core.Container(
                    container.id,
                    container.length,
                    container.width,
                    container.height,
                    max_weight=container.max_weight,
                    cost=container.cost,
                    items=[index[item_id] for item_id in container.loading_order()],
                )
                for container in self.containers
            ],
            items=[
                _core.Item(
                    item.id,
                    item.length,
                    item.width,
                    item.height,
                    weight=item.weight,
                    rotations=_ROTATIONS[item.rotations],
                    max_load_on_top=item.max_load_on_top,
                    unload_order=item.unload_order,
                    group=item.group,
                )
                for item in self.items
            ],
            min_support_ratio=self.min_support_ratio,
        )

    @classmethod
    def from_core(cls, problem: _core.ContainerLoadingProblem) -> ContainerLoadingProblemModel:
        """engine object -> pydantic, with the outputs.

        It decodes `problem` first (`update_placements`), so the positions and the
        per-container outputs describe the loading orders the problem holds now, solved or
        not, and `to_core()` gives those orders back. The score fields are `problem.score`
        as a solve or `calculate_score` last set it (under a constraint selection: the
        selection's score). `initial_items` is left empty: the lists are in `items`."""
        _core.update_placements(problem)
        items = problem.items
        containers = problem.containers
        return cls(
            containers=[
                ContainerModel(
                    id=container.id,
                    length=container.length,
                    width=container.width,
                    height=container.height,
                    max_weight=container.max_weight,
                    cost=container.cost,
                    items=[items[i].id for i in container.placed_items],
                    unplaced_items=[items[i].id for i in container.unplaced_items],
                    loaded_weight=container.loaded_weight,
                    loaded_volume=container.loaded_volume,
                    fill_ratio=container.fill_ratio,
                    used_length=container.used_length,
                )
                for container in containers
            ],
            items=[
                ItemModel(
                    id=item.id,
                    length=item.length,
                    width=item.width,
                    height=item.height,
                    weight=item.weight,
                    rotations=_ROTATION_NAMES[item.rotations],
                    max_load_on_top=item.max_load_on_top,
                    unload_order=item.unload_order,
                    group=item.group,
                    **(
                        {
                            "container": containers[item.container].id,
                            "sequence": item.sequence,
                            "x": item.x,
                            "y": item.y,
                            "z": item.z,
                            "placed_length": item.placed_length,
                            "placed_width": item.placed_width,
                            "placed_height": item.placed_height,
                        }
                        if item.is_placed()
                        else {}
                    ),
                )
                for item in items
            ],
            min_support_ratio=problem.min_support_ratio,
            score_hard=problem.score.hard,
            score_soft=problem.score.soft,
            containers_used=problem.containers_used(),
            unplaced_items=[items[i].id for i in problem.unplaced_items()],
        )


def parameter_uses(problem: ContainerLoadingProblemModel) -> list[ParameterUse]:
    """The parameters `problem` sets away from their defaults that act only through a cost.

    `solve` refuses a model whose selection holds none of a parameter's costs
    (`optimaru._constraint.require_costs`); the table is "Parameters and their costs" in
    docs/use-cases/README.md. The sizes and `min_support_ratio` are read by the decoder,
    whatever the selection, and `cost` is `used_container_cost`'s own scale."""
    uses = [
        ParameterUse(f"container {container.id}", "max_weight", None, ("container_weight_cost",))
        for container in problem.containers
        if container.max_weight is not None
    ]
    for item in problem.items:
        where = f"item {item.id}"
        if item.max_load_on_top is not None:
            uses.append(ParameterUse(where, "max_load_on_top", None, ("stacking_load_cost",)))
        if item.unload_order is not None:
            uses.append(ParameterUse(where, "unload_order", None, ("unloading_order_cost",)))
        if item.group is not None:
            uses.append(ParameterUse(where, "group", None, ("group_split_cost",)))
    return uses


def with_initial_solution(problem, solution):
    """Return a copy of `problem` whose search starts from `solution`.

    `solution` is a solved problem model or a dict of the same shape. Containers and items
    are matched by id: each container's `initial_items` becomes the `items` it has in the
    solution (its placed items, in loading order), replacing what `problem` had; a container
    the solution leaves out starts empty. Items the solution placed nowhere are constructed
    around the seeded loading orders before the search starts. The copy carries no outputs.

    Raises ValueError on an unknown container or item, or an item listed twice.
    """
    if isinstance(solution, BaseModel):
        solution = solution.model_dump(mode="json")
    if not isinstance(solution, dict) or not isinstance(solution.get("containers"), list):
        raise ValueError("solution must be a problem model or a dict with a 'containers' list")
    known_containers = {container.id for container in problem.containers}
    known_items = {item.id for item in problem.items}
    orders: dict[str, list[str]] = {}
    seen: set[str] = set()
    for entry in solution["containers"]:
        container_id = entry.get("id") if isinstance(entry, dict) else None
        if not isinstance(container_id, str) or container_id not in known_containers:
            raise ValueError(f"solution references unknown container: {container_id!r}")
        if container_id in orders:
            raise ValueError(f"solution lists container {container_id!r} more than once")
        order = entry.get("items") or []
        if not isinstance(order, list):
            raise ValueError(f"container {container_id!r}: 'items' must be a list of item ids")
        for item_id in order:
            if not isinstance(item_id, str) or item_id not in known_items:
                raise ValueError(f"container {container_id!r}: unknown item {item_id!r}")
            if item_id in seen:
                raise ValueError(f"solution lists item {item_id!r} more than once")
            seen.add(item_id)
        orders[container_id] = list(order)
    return _without_outputs(problem, orders)


def _without_outputs(problem: ContainerLoadingProblemModel, orders: dict[str, list[str]]):
    """`problem` with `orders` as its initial loading orders (by container id) and no outputs."""
    return ContainerLoadingProblemModel(
        containers=[
            ContainerModel(
                id=container.id,
                length=container.length,
                width=container.width,
                height=container.height,
                max_weight=container.max_weight,
                cost=container.cost,
                initial_items=orders.get(container.id, []),
            )
            for container in problem.containers
        ],
        items=[
            ItemModel(
                id=item.id,
                length=item.length,
                width=item.width,
                height=item.height,
                weight=item.weight,
                rotations=item.rotations,
                max_load_on_top=item.max_load_on_top,
                unload_order=item.unload_order,
                group=item.group,
            )
            for item in problem.items
        ],
        min_support_ratio=problem.min_support_ratio,
    )


class ContainerLoadingRequest(SolveRequest):
    """The container-loading problem with the settings of one solve (see `SolveRequest` for those)."""

    model_input: ContainerLoadingProblemModel


def solve_request(request: ContainerLoadingRequest, *, constraints=None) -> ContainerLoadingProblemModel:
    """Solve a pydantic request and return the solved problem model.

    This is ``solve(request.model_input, ...)`` with the request's settings as the keywords
    of the same names (see `SolveRequest`), and ``constraints`` the selection ``solve``
    takes. The request is not changed.
    """
    from . import SolverConfig, solve

    return solve_model_request(solve, SolverConfig, request, constraints=constraints)
