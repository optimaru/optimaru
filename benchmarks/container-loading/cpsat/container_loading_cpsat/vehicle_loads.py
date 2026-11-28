"""Vehicle loads: one cargo space, many stops, the engine alone.

Two kinds of generated truck, each with every box's `unload_order` its stop:

- `catalog_load`: cartons drawn from a catalog of nine sizes (a few pallet-sized crates
  among them), most kept upright, a tenth with a `max_load_on_top`. Whether every order
  and limit can be met is not known; the families used here are ones the engine packs in
  full when the orders are left out.
- `cut_load`: the cargo space cut into one slab per stop (the last stop at the closed
  end), each slab cut into boxes, then boxes with nothing on top taken out until half the
  space is left. The positions it was cut into are a loading that meets every order and
  limit (`cut_witness`), so hard 0 is reachable.

    uv run container-loading-vehicle-loads --seconds 5 10 30          # the table, as Markdown
    uv run container-loading-vehicle-loads --seconds 5 --warehouse    # with a warehouse system's weights

Each row is three seeds, one thread per solve, `--workers` solves at a time. "Pairs" are
`unloading_order_cost`'s raw cost, "unplaced" `unplaced_item_cost`'s; "first hard 0" is when
the solve first met every hard constraint (`on_new_best`), per seed that did.
"""

from __future__ import annotations

import argparse
import random
import time
from concurrent.futures import ProcessPoolExecutor

from optimaru.container_loading import constraints as picks
from optimaru.container_loading.models import ContainerLoadingProblemModel, ContainerModel, ItemModel

#: Inner cargo spaces in mm: length, width, height.
TRUCKS = {"2.5t": (4300, 1800, 1900), "5t": (6200, 2350, 2400)}
#: Carton sizes in mm and how often each is drawn.
CARTONS = [
    ((300, 200, 200), 0.25),
    ((400, 300, 250), 0.2),
    ((400, 300, 300), 0.15),
    ((500, 400, 300), 0.15),
    ((600, 400, 400), 0.1),
    ((600, 500, 450), 0.07),
    ((800, 600, 500), 0.05),
    ((1000, 600, 600), 0.02),
    ((1200, 800, 1000), 0.01),
]
#: (name, generator, truck, stops, boxes)
FAMILIES = [
    ("catalog 2.5t, 12 stops, 120 boxes", "catalog", "2.5t", 12, 120),
    ("catalog 5t, 15 stops, 200 boxes", "catalog", "5t", 15, 200),
    ("catalog 5t, 20 stops, 250 boxes", "catalog", "5t", 20, 250),
    ("catalog 5t, 25 stops, 220 boxes", "catalog", "5t", 25, 220),
    ("catalog 5t, 20 stops, 300 boxes", "catalog", "5t", 20, 300),
    ("cut 2.5t, 10 stops", "cut", "2.5t", 10, 120),
    ("cut 5t, 15 stops", "cut", "5t", 15, 200),
    ("cut 5t, 20 stops", "cut", "5t", 20, 300),
]
SEEDS = (1, 2, 3)


def _weight(rng: random.Random, length: int, width: int, height: int) -> int:
    """Grams, at 80 to 250 kg per cubic metre."""
    return max(300, 10 * int(length * width * height / 1e6 * rng.uniform(80, 250) / 10))


def _truck(truck: str) -> list[ContainerModel]:
    length, width, height = TRUCKS[truck]
    return [ContainerModel(id="truck", length=length, width=width, height=height)]


def catalog_load(seed: int, truck: str, stops: int, boxes: int) -> ContainerLoadingProblemModel:
    """`boxes` cartons of the catalog for `stops` stops, in one truck."""
    rng = random.Random(seed)
    sizes, shares = zip(*CARTONS)
    items = []
    for i in range(boxes):
        length, width, height = (side + 10 * rng.randint(-3, 3) for side in rng.choices(sizes, shares)[0])
        item = ItemModel(
            id=f"b{i + 1}",
            length=length,
            width=width,
            height=height,
            weight=_weight(rng, length, width, height),
            rotations="upright" if rng.random() < 0.85 else "all",
            unload_order=rng.randint(1, stops),
        )
        if rng.random() < 0.1:
            item.max_load_on_top = rng.choice([0, 5000, 20000])
        items.append(item)
    return ContainerLoadingProblemModel(containers=_truck(truck), items=items)


Box = tuple[tuple[int, int, int], tuple[int, int, int], int]  # corner, size, stop


def _cut(corner: tuple[int, int, int], size: tuple[int, int, int], pieces: int, rng: random.Random) -> list:
    """Cut a box into about `pieces` boxes, in steps of 50, never a side below 200 by a cut (a slab
    thinner than that stays as thin)."""
    boxes = [(corner, size)]
    while len(boxes) < pieces:
        boxes.sort(key=lambda box: box[1][0] * box[1][1] * box[1][2])
        index = next((i for i in reversed(range(len(boxes))) if max(boxes[i][1]) >= 400), None)
        if index is None:
            break
        at, box = boxes.pop(index)
        axes = [axis for axis in range(3) if box[axis] >= 400]
        axis = max(axes, key=lambda a: box[a]) if rng.random() < 0.7 else rng.choice(axes)
        units = box[axis] // 50
        first = max(4, min(units - 4, round(units * rng.uniform(0.3, 0.7)))) * 50
        left, right, beyond = list(box), list(box), list(at)
        left[axis], right[axis] = first, box[axis] - first
        beyond[axis] += first
        boxes += [(at, tuple(left)), (tuple(beyond), tuple(right))]
    return boxes


def cut_witness(seed: int, truck: str, stops: int, boxes: int, fill: float = 0.5) -> list[Box]:
    """The boxes of `cut_load`, where they were cut: a loading with every stop clear of the later ones."""
    rng = random.Random(seed)
    length, width, height = TRUCKS[truck]
    shares = [rng.uniform(0.5, 1.5) for _ in range(stops)]
    per_slab = max(2, round(boxes / fill / stops))
    placed: list[Box] = []
    x = 0
    for k, stop in enumerate(range(stops, 0, -1)):
        slab = length - x if k == stops - 1 else max(200, 10 * round(length * shares[k] / sum(shares) / 10))
        if slab <= 0:  # the slabs before it took the whole length: too many stops for the truck
            raise ValueError(f"{stops} stops leave no room for the last slab in a length of {length}")
        placed += [(at, size, stop) for at, size in _cut((x, 0, 0), (slab, width, height), per_slab, rng)]
        x += slab

    def load() -> float:
        return sum(s[0] * s[1] * s[2] for _, s, _ in placed) / (length * width * height)

    while load() > fill:
        placed.pop(rng.choice([i for i in range(len(placed)) if not _under_another(placed, i)]))
    return placed


def cut_load(seed: int, truck: str, stops: int, boxes: int, fill: float = 0.5) -> ContainerLoadingProblemModel:
    """`cut_witness`'s boxes, shuffled, upright, a fifth of those with nothing on top fragile."""
    placed = cut_witness(seed, truck, stops, boxes, fill)
    rng = random.Random(seed + 1_000_003)
    on_top = [not _under_another(placed, i) for i in range(len(placed))]
    order = list(range(len(placed)))
    rng.shuffle(order)
    items = []
    for n, i in enumerate(order):
        _, (length, width, height), stop = placed[i]
        item = ItemModel(
            id=f"b{i + 1}",
            length=length,
            width=width,
            height=height,
            weight=_weight(rng, length, width, height),
            rotations="upright",
            unload_order=stop,
        )
        if on_top[i] and rng.random() < 0.2:
            item.max_load_on_top = rng.choice([0, 5000])
        items.append(item)
    return ContainerLoadingProblemModel(containers=_truck(truck), items=items)


def _under_another(placed: list[Box], i: int) -> bool:
    (ax, ay, az), (al, aw, ah), _ = placed[i]
    return any(
        j != i and bz == az + ah and bx < ax + al and ax < bx + bl and by < ay + aw and ay < by + bw
        for j, ((bx, by, bz), (bl, bw, bh), _) in enumerate(placed)
    )


def warehouse_weights() -> list:
    """The weights a warehouse system sends: an unplaced item 1,000, an unloading pair 10, weight
    and stacking 1 per gram, load balance 1e-4 per mille and loading length 1e-6 per mm."""
    return [
        picks.UnplacedItemCost(weight=1000),
        picks.ContainerWeightCost(weight=1),
        picks.StackingLoadCost(weight=1),
        picks.UnloadingOrderCost(weight=10),
        picks.UsedContainerCost(),
        picks.GroupSplitCost(),
        picks.UsedLengthCost(weight=1e-6),
        picks.LoadBalanceCost(weight=1e-4),
    ]


def model_of(kind: str, seed: int, truck: str, stops: int, boxes: int) -> ContainerLoadingProblemModel:
    return (catalog_load if kind == "catalog" else cut_load)(seed, truck, stops, boxes)


def measure(task: tuple) -> dict:
    """One solve: what is left after `seconds`, and when hard 0 was first met."""
    from optimaru import container_loading

    kind, truck, stops, boxes, seed, seconds, warehouse = task
    problem = model_of(kind, seed, truck, stops, boxes).to_core()
    first: list[float] = []
    started = time.perf_counter()

    def on_new_best(step: int, score) -> None:
        if not first and score.hard == 0:
            first.append(time.perf_counter() - started)

    config = container_loading.SolverConfig(stop={"time_limit_seconds": seconds}, run={"random_seed": seed})
    container_loading.solve(
        problem, config=config, on_new_best=on_new_best, constraints=warehouse_weights() if warehouse else None
    )
    costs = container_loading.constraint_costs(problem)
    if problem.score.hard == 0 and not first:
        first.append(0.0)
    return {
        "hard0": problem.score.hard == 0,
        "pairs": sum(costs["unloading_order_cost"]["costs"]),
        "unplaced": sum(costs["unplaced_item_cost"]["costs"]),
        "first": first[0] if first else None,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="container-loading-vehicle-loads")
    parser.add_argument("--seconds", type=float, nargs="+", default=[10.0], help="the budgets of the solves")
    parser.add_argument("--workers", type=int, default=4, help="solves at a time, one thread each")
    parser.add_argument("--warehouse", action="store_true", help="score with warehouse_weights()")
    parser.add_argument("--families", type=int, default=len(FAMILIES), help="only the first N families")
    args = parser.parse_args(argv)
    tasks = [
        (kind, truck, stops, boxes, seed, seconds, args.warehouse)
        for _, kind, truck, stops, boxes in FAMILIES[: args.families]
        for seconds in args.seconds
        for seed in SEEDS
    ]
    with ProcessPoolExecutor(args.workers) as pool:
        results = list(pool.map(measure, tasks))
    print("| truck | budget | hard 0 | pairs left (mean) | unplaced (mean) | first hard 0 (s) |")
    print("| --- | --- | --- | --- | --- | --- |")
    row = 0
    for name, *_ in FAMILIES[: args.families]:
        for seconds in args.seconds:
            runs = results[row : row + len(SEEDS)]
            row += len(SEEDS)
            firsts = "/".join(f"{run['first']:.1f}" for run in runs if run["first"] is not None) or "-"
            print(
                f"| {name} | {seconds:g} s | {sum(run['hard0'] for run in runs)}/{len(runs)} | "
                f"{sum(run['pairs'] for run in runs) / len(runs):.0f} | "
                f"{sum(run['unplaced'] for run in runs) / len(runs):.1f} | {firsts} |",
                flush=True,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
