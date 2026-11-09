"""The C++ domain computes exactly what the pure-Python reference computes.

`reference_bin_packing` is an independent scorer and construction (an oracle,
plain data in and out). On every sample — the web demo (`fixtures/web_demo.json`,
a copy of the web app's sample) and the package's own test fixtures — and on
seeded random instances and assignments, the C++ `constraint_costs` totals equal
the oracle's per constraint (under the engine names), the default score equals the
oracle's, and the zero-budget construction returns the oracle's bins.

The per-unit lists differ by design (the engine lists every constraint per bin
0..n-1; the reference lists capacity per bin 0..max bin, colour excess per
(bin, colour) and conflicts per pair), so they are compared as totals; at exponent 1 the
scores are identical.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import pytest
import reference_bin_packing as ref

import bin_packing as bp
from bin_packing.constraints import (
    BinCapacityCost,
    ColorConflictCost,
    ItemConflictCost,
    UsedBinsCost,
    score,
)
from bin_packing.models import BinPackingProblemModel as Model

FIXTURES = Path(__file__).parent / "fixtures"
SPEC_OF = {
    "bin_capacity": BinCapacityCost,
    "color_compatibility": ColorConflictCost,
    "item_conflicts": ItemConflictCost,
    "minimize_bins": UsedBinsCost,
}


def _samples() -> dict[str, dict]:
    abcd = [{"id": "A", "weight": 8}, {"id": "B", "weight": 7}, {"id": "C", "weight": 3}, {"id": "D", "weight": 2}]
    twenty = [48, 30, 19, 36, 36, 27, 42, 42, 42, 42, 42, 42, 24, 24, 51, 49, 55, 45, 33, 17]
    return {
        "web-demo": json.loads((FIXTURES / "web_demo.json").read_text()),
        # test_bin_packing.py
        "abcd": {"bin_capacity": 10, "items": abcd},
        "abcd-manual": {
            "bin_capacity": 10,
            "items": [dict(item, bin=bin_index) for item, bin_index in zip(abcd, [0, 0, 1, 1], strict=True)],
        },
        "colors": {
            "bin_capacity": 20,
            "items": [
                {"id": "A", "weight": 3, "bin": 0, "color": "red"},
                {"id": "B", "weight": 4, "bin": 0, "color": "red"},
                {"id": "C", "weight": 5, "bin": 0, "color": "blue"},
            ],
            "conflicts": [{"first": "A", "second": "C"}],
        },
        "twenty": {"bin_capacity": 100, "items": [{"id": str(i), "weight": w} for i, w in enumerate(twenty)]},
        # test_initial_solution.py
        "infeasible": {"bin_capacity": 5, "items": [{"id": "A", "weight": 9}, {"id": "B", "weight": 2}]},
        "partial-seed": {
            "bin_capacity": 10,
            "items": [dict(item, bin=b) for item, b in zip(abcd, [-1, 0, -1, 0], strict=True)],
            "warm_start": True,
        },
        "gapped-seed": {
            "bin_capacity": 10,
            "items": [dict(item, bin=b) for item, b in zip(abcd, [3, 1, 1, 3], strict=True)],
            "warm_start": True,
        },
        # bp_test.cpp / bp_plugin_test.cpp: every constraint positive at once
        "every-term": {
            "bin_capacity": 10,
            "items": [
                {"id": "A", "weight": 8, "bin": 0, "color": "red"},
                {"id": "B", "weight": 7, "bin": 0, "color": "red"},
                {"id": "C", "weight": 3, "bin": 0},
                {"id": "D", "weight": 2, "bin": 1},
            ],
            "conflicts": [{"first": "A", "second": "B"}, {"first": "B", "second": "A"}],
        },
    }


def _with_bins(problem: dict, bins: list[int]) -> dict:
    data = json.loads(json.dumps(problem))
    for item, bin_index in zip(data["items"], bins, strict=True):
        item["bin"] = bin_index
    return data


def _cpp_totals(data: dict) -> dict[str, float]:
    costs = bp.constraint_costs(Model.model_validate(data).to_core())
    return {name: sum(entry["costs"]) for name, entry in costs.items()}


def _cpp_score(data: dict) -> tuple[int, float]:
    result = bp.calculate_score(Model.model_validate(data).to_core())
    return result.hard, result.soft


def _assert_parity(data: dict) -> None:
    dump = Model.model_validate(data).model_dump()
    assert _cpp_totals(data) == ref.totals(dump)
    assert _cpp_score(data) == ref.default_score(dump)


def _random_instance(rng: random.Random) -> dict:
    n = rng.randrange(1, 13)
    colors = [None, None, "red", "green", "blue"][: rng.randrange(1, 6)]
    items = [{"id": f"i{k}", "weight": rng.randrange(1, 12), "color": rng.choice(colors)} for k in range(n)]
    conflicts = []
    if n >= 2:
        for _ in range(rng.randrange(0, 2 * n)):
            a, b = rng.sample(range(n), 2)
            conflicts.append({"first": f"i{a}", "second": f"i{b}"})
    return {"bin_capacity": rng.randrange(5, 25), "items": items, "conflicts": conflicts}


def _random_bins(n: int, rng: random.Random, unassigned: float) -> list[int]:
    top = rng.randrange(1, n + 1)
    return [-1 if rng.random() < unassigned else rng.randrange(top) for _ in range(n)]


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_as_given(name):
    _assert_parity(_samples()[name])


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_on_seeded_assignments(name):
    problem = _samples()[name]
    rng = random.Random(f"bin-packing:{name}")
    for trial in range(20):
        _assert_parity(_with_bins(problem, _random_bins(len(problem["items"]), rng, 0.15 if trial % 3 == 0 else 0.0)))


def test_seeded_random_instances_and_assignments():
    rng = random.Random(2026)
    for _ in range(200):
        problem = _random_instance(rng)
        for trial in range(3):
            _assert_parity(_with_bins(problem, _random_bins(len(problem["items"]), rng, 0.2 if trial == 0 else 0.0)))


def test_the_score_at_the_default_weights_on_solved_states():
    """A short C++ solve of every sample: its stored score is the oracle's default score."""
    for name, problem in _samples().items():
        core = Model.model_validate(problem).to_core()
        config = bp.SolverConfig()
        config.step_count_limit = 1000
        config.random_seed = 42
        bp.solve(core, config=config)
        dump = Model.from_core(core).model_dump()
        assert (core.score.hard, core.score.soft) == ref.default_score(dump), name
        assert {k: sum(v["costs"]) for k, v in bp.constraint_costs(core).items()} == ref.totals(dump), name


def test_exponent_one_scores_equal_for_any_selection():
    """At exponent 1 the regrouped units cannot change a score: any weights / levels, both sides."""
    rng = random.Random(7)
    for _ in range(100):
        problem = _random_instance(rng)
        data = _with_bins(problem, _random_bins(len(problem["items"]), rng, 0.1))
        picked = rng.sample(list(SPEC_OF), rng.randrange(1, 5))
        picks = [(name, rng.choice([None, 0, 1, 2, 5]), rng.choice([None, True, False])) for name in picked]
        expected = ref.score(ref.constraint_costs(Model.model_validate(data).model_dump()), [(*p, 1.0) for p in picks])
        levels = {None: None, True: "hard", False: "soft"}
        specs = [SPEC_OF[name](weight=weight, level=levels[hard]) for name, weight, hard in picks]
        actual = score(bp.constraint_costs(Model.model_validate(data).to_core()), specs)
        assert actual == expected


@pytest.mark.parametrize("name", list(_samples()))
def test_zero_budget_construction_matches_the_oracle(name):
    """A zero-budget solve returns the reference's best-fit construction (plain, or the completed seed)."""
    problem = _samples()[name]
    for warm in (False, True):
        data = dict(problem, warm_start=warm)
        expected = ref.construct(Model.model_validate(data).model_dump())
        if max(expected, default=-1) >= len(data["items"]):
            continue  # the reference leaves 0..n-1 here; the engine stays inside (bp_test.cpp)
        core = Model.model_validate(data).to_core()
        config = bp.SolverConfig()
        config.spent_limit_seconds = 0
        bp.solve(core, config=config)
        assert core.assignment() == expected, (name, warm)


def test_zero_budget_construction_matches_the_oracle_on_random_instances():
    rng = random.Random(11)
    checked = 0
    for _ in range(150):
        problem = _random_instance(rng)
        warm = rng.random() < 0.5
        data = _with_bins(problem, _random_bins(len(problem["items"]), rng, 0.5)) if warm else problem
        data = dict(data, warm_start=warm)
        expected = ref.construct(Model.model_validate(data).model_dump())
        if max(expected, default=-1) >= len(data["items"]):
            continue
        core = Model.model_validate(data).to_core()
        config = bp.SolverConfig()
        config.spent_limit_seconds = 0
        bp.solve(core, config=config)
        assert core.assignment() == expected
        checked += 1
    assert checked >= 100
