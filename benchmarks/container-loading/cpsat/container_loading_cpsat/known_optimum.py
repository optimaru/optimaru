"""How many containers the engine needs where the fewest is known.

Containers of 1200 x 800 x 1000 are cut into boxes with sides in steps of 100
(`instances.perfect_packing`): the boxes fit back into as many containers as were cut, and
no loading uses fewer. Each row is one family of such instances, three seeds each, with
what the construction alone needs and what the solve needs after the budget.

    uv run container-loading-known-optimum --seconds 10            # the table, as Markdown
    uv run container-loading-known-optimum --seconds 1 --families 2   # a quick look

A family with slack has that share of the volume taken out again, so it is not full to
the last unit: closer to a real load. The "volume needs" column is then a lower bound
(the containers the volume fills, rounded up), and the number cut is known to be enough.
"""

from __future__ import annotations

import argparse
import time

from optimaru import container_loading

from . import instances as generator

#: (containers cut, boxes per container, slack, rotations)
FAMILIES = [
    (4, 15, 0.0, "fixed"),
    (4, 15, 0.0, "all"),
    (4, 30, 0.0, "all"),
    (10, 20, 0.0, "fixed"),
    (10, 20, 0.0, "all"),
    (20, 25, 0.0, "all"),
    (4, 15, 0.1, "all"),
    (10, 20, 0.1, "fixed"),
    (10, 20, 0.1, "all"),
    (20, 25, 0.1, "all"),
    (40, 50, 0.1, "all"),
]
SEEDS = (1, 2, 3)


def measure(containers: int, per_container: int, slack: float, rotations: str, seed: int, seconds: float) -> dict:
    """One instance: the fewest containers, what the construction uses, what the solve uses."""
    model = generator.perfect_packing(
        containers, per_container, seed, size=(1200, 800, 1000), step=100, rotations=rotations, slack=slack
    )
    constructed = container_loading.solve(
        model.to_core(), config=container_loading.SolverConfig(stop={"time_limit_seconds": 0})
    )
    started = time.perf_counter()
    solved = container_loading.solve(model.to_core(), time_limit_seconds=seconds, random_seed=seed)
    return {
        "items": len(model.items),
        "optimum": generator.optimum(model),
        "constructed": constructed.containers_used(),
        "constructed_unplaced": len(constructed.unplaced_items()),
        "solved": solved.containers_used(),
        "solved_unplaced": len(solved.unplaced_items()),
        "seconds": time.perf_counter() - started,
    }


def _spread(values: list[int]) -> str:
    return str(values[0]) if len(set(values)) == 1 else "/".join(str(value) for value in values)


def _used(runs: list[dict], which: str) -> str:
    """The containers used in each run, with the items left without a place, if any."""
    unplaced = sum(run[f"{which}_unplaced"] for run in runs)
    return _spread([run[which] for run in runs]) + (f" ({unplaced} unplaced)" if unplaced else "")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="container-loading-known-optimum")
    parser.add_argument("--seconds", type=float, default=10.0, help="the budget of each solve")
    parser.add_argument("--families", type=int, default=len(FAMILIES), help="only the first N families")
    args = parser.parse_args(argv)
    print("| cut containers x boxes | slack | rotations | items | volume needs | construction | solve |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for containers, per_container, slack, rotations in FAMILIES[: args.families]:
        runs = [measure(containers, per_container, slack, rotations, seed, args.seconds) for seed in SEEDS]
        print(
            f"| {containers} x {per_container} | {slack:.0%} | {rotations} | "
            f"{_spread([run['items'] for run in runs])} | {_spread([run['optimum'] for run in runs])} | "
            f"{_used(runs, 'constructed')} | {_used(runs, 'solved')} |",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
