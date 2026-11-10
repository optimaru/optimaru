"""Search-quality regression check on generated instances (no download needed).

A search review (finding SQ-1) found the C++ search 1-2 bins behind a pure-Python
simulated-annealing baseline on instances with colours and conflicts: EmptyBinMove
checks the capacity only, so emptying a bin into the others nearly always broke a
colour or a conflict, and late acceptance rejected the move. RepackMove closes that
gap. This script keeps the review's instances and, with ``--check``, fails when the
engine uses more bins than the recorded baseline on one of them or ends infeasible
anywhere.

    uv run --project cpsat python -m bench.quality --seconds 2 --seeds 1,2,3 --check
    uv run --project cpsat python -m bench.quality --extra --seconds 10   # the full table

The default set is the review's four coloured instances and their plain twins
(the same weights, no colours or conflicts), so a change that helps the coloured
ones at the plain ones' expense shows too. ``--extra`` adds the instances the
README table also covers: the web demo, the web demo six times over, the
benchmark's generated instances and larger ones.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
import time
from collections.abc import Callable
from pathlib import Path

from optimaru import bin_packing
from optimaru.bin_packing.models import BinPackingProblemModel

WEB_DEMO = Path(__file__).resolve().parents[3] / "tests" / "bin_packing" / "fixtures" / "web_demo.json"

# Generator seeds: 20261031 is the review's instance; 11, 22 and 33 are the ones that
# show the gap is not specific to one instance.
REVIEW_SEEDS = {"sq1": 20261031, "g11": 11, "g22": 22, "g33": 33}

# The recorded baseline (the pure-Python simulated annealing, seeds 1-3 at 2 s and
# 10 s, measured when RepackMove was added in commit
# 5e3f7d3a5f85ed7c5263be34c126f94b4cb2afa8, whose README table lists the runs):
# the most bins it used on each coloured review instance. --check fails above.
BASELINE_BINS = {"sq1": 44, "g11": 43, "g22": 45, "g33": 43}


def review_instance(
    seed: int, *, count: int = 120, conflicts: int = 10, coloured: bool = True
) -> dict:
    """The review recipe: weights 10..60 on capacity 100, about 40% of the items in one
    of 4 colours, `conflicts` random pairs. ``coloured=False`` draws the same numbers and
    drops the colours and conflicts (the plain twin)."""
    rng = random.Random(seed)
    for _ in range(60):  # the recipe's burn-in, kept so the instances stay the same
        rng.randint(1, 10)
    for j in range(1, 60):
        rng.sample(range(j), min(j, rng.choice([0, 1, 1, 2])))
    items = []
    for index in range(count):
        item = {"id": f"I{index:03d}", "weight": rng.randint(10, 60)}
        if rng.random() < 0.4:
            colour = rng.choice(["red", "blue", "green", "yellow"])
            if coloured:
                item["color"] = colour
        items.append(item)
    pairs = []
    for _ in range(conflicts):
        first, second = rng.sample(range(count), 2)
        if coloured:
            pairs.append({"first": items[first]["id"], "second": items[second]["id"]})
    return {"bin_capacity": 100, "items": items, "conflicts": pairs}


def uniform_instance(
    count: int, capacity: int, low: int, high: int, seed: int, colors: int = 0
) -> dict:
    """The recipe of ``bin_packing_cpsat.benchmark`` (Falkenauer-style "u" weights;
    with `colors`, every third item coloured and count // 10 conflict pairs)."""
    rng = random.Random(seed)
    items = []
    for index in range(count):
        item = {"id": f"I{index + 1:03d}", "weight": rng.randint(low, high)}
        if colors and index % 3 == 0:
            item["color"] = f"c{rng.randrange(colors)}"
        items.append(item)
    pairs = []
    if colors:
        for _ in range(count // 10):
            first, second = rng.sample(range(count), 2)
            pairs.append({"first": items[first]["id"], "second": items[second]["id"]})
    return {"bin_capacity": capacity, "items": items, "conflicts": pairs}


def triplet_instance(count: int, seed: int) -> dict:
    """Falkenauer-style "t" instance: capacity 1000, items in triplets that fill a bin
    exactly (so count / 3 bins is optimal), shuffled."""
    rng = random.Random(seed)
    weights = []
    for _ in range(count // 3):
        first = rng.randint(380, 490)
        second = rng.randint(250, (1000 - first) // 2)
        weights += [first, second, 1000 - first - second]
    rng.shuffle(weights)
    items = [
        {"id": f"T{index:04d}", "weight": weight}
        for index, weight in enumerate(weights)
    ]
    return {"bin_capacity": 1000, "items": items, "conflicts": []}


def web_demo(copies: int = 1) -> dict:
    demo = json.loads(WEB_DEMO.read_text())
    if copies == 1:
        return demo
    items, pairs = [], []
    for copy in range(copies):
        items += [{**item, "id": f"{item['id']}_{copy}"} for item in demo["items"]]
        pairs += [
            {"first": f"{pair['first']}_{copy}", "second": f"{pair['second']}_{copy}"}
            for pair in demo["conflicts"]
        ]
    return {"bin_capacity": demo["bin_capacity"], "items": items, "conflicts": pairs}


def instances(extra: bool = False) -> dict[str, Callable[[], dict]]:
    """name -> instance builder (a JSON dict). The review set; ``extra`` adds the rest."""
    built: dict[str, Callable[[], dict]] = {}
    for name, seed in REVIEW_SEEDS.items():
        built[name] = lambda seed=seed: review_instance(seed)
        built[f"plain-{name}"] = lambda seed=seed: review_instance(seed, coloured=False)
    if extra:
        built["colours-only-sq1"] = lambda: {
            **review_instance(20261031),
            "conflicts": [],
        }
        built["review-250"] = lambda: review_instance(4242, count=250, conflicts=25)
        built["web-demo"] = web_demo
        built["web-demo-x6"] = lambda: web_demo(6)
        built["bench-small"] = lambda: uniform_instance(
            12, 100, 20, 60, seed=0, colors=3
        )
        built["bench-medium"] = lambda: uniform_instance(
            30, 150, 20, 100, seed=1, colors=4
        )
        built["bench-large"] = lambda: uniform_instance(60, 150, 20, 100, seed=2)
        built["u500-coloured"] = lambda: uniform_instance(
            500, 150, 20, 100, seed=7, colors=4
        )
        built["u250"] = lambda: uniform_instance(250, 150, 20, 100, seed=3)
        built["u500"] = lambda: uniform_instance(500, 150, 20, 100, seed=4)
        built["u1000"] = lambda: uniform_instance(1000, 150, 20, 100, seed=5)
        built["t120"] = lambda: triplet_instance(120, seed=6)
    return built


def lower_bound(data: dict) -> int:
    return math.ceil(
        sum(item["weight"] for item in data["items"]) / data["bin_capacity"]
    )


def run(data: dict, seconds: float, seed: int) -> dict:
    core = BinPackingProblemModel.model_validate(data).to_core()
    started = time.perf_counter()
    score = bin_packing.solve(
        core, time_limit_seconds=seconds, random_seed=seed, parallel_solver_count=1
    ).score
    costs = bin_packing.constraint_costs(core)
    return {
        "seed": seed,
        "bins": int(costs["used_bins_cost"]["costs"][0]),
        "hard": int(score.hard),
        "seconds": time.perf_counter() - started,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--seeds", default="1,2,3", help="comma-separated random seeds")
    parser.add_argument(
        "--extra", action="store_true", help="also the instances beyond the review set"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when a review run uses more bins than the recorded baseline",
    )
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    seeds = [int(seed) for seed in args.seeds.split(",")]

    results = []
    for name, build in instances(args.extra).items():
        data = build()
        results.append(
            {
                "instance": name,
                "items": len(data["items"]),
                "conflicts": len(data["conflicts"]),
                "lower_bound": lower_bound(data),
                "baseline_bins": BASELINE_BINS.get(name),
                "runs": [run(data, args.seconds, seed) for seed in seeds],
            }
        )

    if args.as_json:
        print(json.dumps(results, indent=2))
    else:
        print(
            f"Generated instances - {args.seconds:g}s per run, one thread, seeds {args.seeds}"
        )
        print(
            f"{'instance':<17} {'items':>5} {'LB':>4} {'base':>6}  bins per seed (* = hard < 0)"
        )
        for result in results:
            bins = ", ".join(
                f"{r['bins']}{'*' if r['hard'] < 0 else ''}" for r in result["runs"]
            )
            reference = (
                result["baseline_bins"] if result["baseline_bins"] is not None else "-"
            )
            print(
                f"{result['instance']:<17} {result['items']:>5} {result['lower_bound']:>4} {reference!s:>6}  {bins}"
            )

    failed = []
    if args.check:
        for result in results:
            for r in result["runs"]:
                where = f"{result['instance']} seed {r['seed']}"
                if r["hard"] < 0:
                    failed.append(f"{where}: infeasible ({r['hard']}hard)")
                elif (
                    result["baseline_bins"] is not None
                    and r["bins"] > result["baseline_bins"]
                ):
                    failed.append(
                        f"{where}: {r['bins']} bins, the baseline used {result['baseline_bins']}"
                    )
    for line in failed:
        print("FAIL", line, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
