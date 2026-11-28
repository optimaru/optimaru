"""Instances with a known optimum: containers cut into boxes.

A container is cut by planes parallel to its sides (always the largest piece, mostly
across its longest side, at a random multiple of `step`) until it is `per_container`
boxes. The boxes of `containers` such cuts fit back into `containers` containers, with
every one of them full to the last unit: a perfect packing, and so the fewest containers
any loading can use. `slack` then takes a share of the volume out again (whole boxes),
which keeps the fit; the fewest containers is then no longer known exactly, only bounded
by what the volume needs (`optimum`).

Everything is drawn from `random.Random(seed)`: the same arguments give the same instance.
"""

from __future__ import annotations

import random
from math import ceil

from optimaru.container_loading.models import ContainerLoadingProblemModel, ContainerModel, ItemModel

Size = tuple[int, int, int]


def cut(size: Size, pieces: int, rng: random.Random, step: int) -> list[Size]:
    """Cut a box of `size` into `pieces` boxes with sides that are multiples of `step` (fewer when
    no piece can be cut any further)."""
    boxes = [size]
    while len(boxes) < pieces:
        boxes.sort(key=lambda box: box[0] * box[1] * box[2])
        # The largest piece that can still be cut.
        index = next((i for i in reversed(range(len(boxes))) if max(boxes[i]) >= 2 * step), None)
        if index is None:
            break
        box = list(boxes.pop(index))
        axes = [axis for axis in range(3) if box[axis] >= 2 * step]
        axis = max(axes, key=lambda a: box[a]) if rng.random() < 0.7 else rng.choice(axes)
        units = box[axis] // step
        first = max(1, min(units - 1, round(units * rng.uniform(0.3, 0.7)))) * step
        left, right = box.copy(), box.copy()
        left[axis], right[axis] = first, box[axis] - first
        boxes += [(left[0], left[1], left[2]), (right[0], right[1], right[2])]
    return boxes


def perfect_packing(
    containers: int,
    per_container: int,
    seed: int,
    *,
    size: Size = (12, 8, 8),
    step: int = 1,
    rotations: str = "all",
    slack: float = 0.0,
    spare: int | None = None,
) -> ContainerLoadingProblemModel:
    """`containers` containers of `size`, each cut into `per_container` boxes, shuffled, with
    `spare` more empty containers to choose from (half as many again by default, at least two).

    Without slack, the fewest containers that hold the boxes is `optimum(model)`."""
    rng = random.Random(seed)
    boxes = [box for _ in range(containers) for box in cut(size, per_container, rng, step)]
    rng.shuffle(boxes)
    # Take whole boxes out until `slack` of the volume is gone (never more).
    budget = slack * containers * size[0] * size[1] * size[2]
    kept = []
    for box in boxes:
        volume = box[0] * box[1] * box[2]
        if volume <= budget:
            budget -= volume
        else:
            kept.append(box)
    spare = max(2, containers // 2) if spare is None else spare
    return ContainerLoadingProblemModel(
        containers=[
            ContainerModel(id=f"container-{c + 1}", length=size[0], width=size[1], height=size[2])
            for c in range(containers + spare)
        ],
        items=[
            ItemModel(id=f"box-{i + 1}", length=box[0], width=box[1], height=box[2], rotations=rotations)
            for i, box in enumerate(kept)
        ],
    )


def optimum(model: ContainerLoadingProblemModel) -> int:
    """The fewest containers the volume of a `perfect_packing` instance's items needs.

    Without slack this is the optimum: the items fill exactly the containers they were cut
    from. With slack it is a lower bound: the cut shows that the number of containers cut is
    enough, and the volume may ask for one fewer, which may or may not be reachable."""
    first = model.containers[0]
    capacity = first.length * first.width * first.height
    return ceil(sum(item.length * item.width * item.height for item in model.items) / capacity)
