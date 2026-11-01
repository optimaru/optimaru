"""The C++ port computes exactly what the 0.1.5 pure-Python package computed.

`reference_max_cut` is the frozen 0.1.5 scorer and start partition (an oracle,
plain data in and out). On every sample — the web demo (`fixtures/web_demo.json`,
a copy of the web app's sample) and the package's own test fixtures — and on
seeded random instances, the C++ `constraint_costs` totals equal the oracle's per
constraint (under the 0.2.0 names), the default score equals the oracle's, a
selection at integral weights scores like the oracle, and the zero-budget solve
returns the oracle's start partition.

0.1.5 listed one 0/1 term per fixed vertex / same-side pair / different-side pair;
0.2.0 reports one cost per side for those (the size bounds keep their two terms,
the imbalance and the cut their one). The totals are what must match; the
documented differences (the exponent shapes per side, hard rounds per side, a hard
reward is refused, a partial partition is not scored) have their own tests below.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import pytest
import reference_max_cut as ref

import max_cut
from max_cut import with_initial_solution
from max_cut.constraints import (
    Constraint,
    CutWeightCost,
    FixedPartitionCost,
    PartitionSizeBoundsCost,
    get_constraints,
    score,
)
from max_cut.models import MaxCutProblemModel as Model

FIXTURES = Path(__file__).parent / "fixtures"
NEW_NAMES = [ref.RENAMED[name] for name in ref.OLD_NAMES]
OLD_OF = {new: old for old, new in ref.RENAMED.items()}


def _samples() -> dict[str, dict]:
    path = [{"source": "A", "target": "B", "weight": 2}, {"source": "B", "target": "C", "weight": 3}]
    return {
        "web-demo": json.loads((FIXTURES / "web_demo.json").read_text()),
        # test_max_cut.py
        "triangle": {
            "vertices": ["A", "B", "C"],
            "edges": [*path, {"source": "A", "target": "C", "weight": 4}],
            "partition": {"A": 0, "B": 1, "C": 0},
        },
        "contract": {
            "vertices": ["A", "B", "C"],
            "edges": [],
            "partition": {"A": 0, "B": 1, "C": 1},
            "fixed_partition": {"A": 1},
            "same_partition_pairs": [{"first": "A", "second": "B"}],
            "different_partition_pairs": [{"first": "B", "second": "C"}],
            "min_partition_size": 1,
            "max_partition_size": 2,
        },
        # test_initial_solution.py
        "path": {
            "vertices": ["A", "B", "C", "D"],
            "edges": [*path, {"source": "C", "target": "D", "weight": 4}],
            "fixed_partition": {"A": 1, "B": 1, "C": 1},
            "min_partition_size": 2,
            "max_partition_size": 2,
        },
        # benchmarks/max-cut/cpsat tests/test_exact.py
        "exact": {
            "vertices": ["A", "B", "C", "D"],
            "edges": [{"source": "A", "target": "C", "weight": 5}, {"source": "B", "target": "D", "weight": 4}],
            "fixed_partition": {"A": 1},
            "same_partition_pairs": [{"first": "A", "second": "B"}],
            "different_partition_pairs": [{"first": "A", "second": "C"}],
            "min_partition_size": 2,
            "max_partition_size": 2,
        },
        # Self-loops, parallel and zero-weight edges, self-pairs, repeats, a one-sided bound.
        "edge-cases": {
            "vertices": ["A", "B", "C", "D", "E"],
            "edges": [
                {"source": "A", "target": "A", "weight": 5},
                {"source": "A", "target": "B", "weight": 1},
                {"source": "B", "target": "A", "weight": 2},
                {"source": "C", "target": "D", "weight": 0},
                {"source": "D", "target": "E"},
            ],
            "fixed_partition": {"E": 0, "B": 1},
            "same_partition_pairs": [{"first": "C", "second": "C"}, {"first": "A", "second": "D"}] * 2,
            "different_partition_pairs": [{"first": "B", "second": "B"}, {"first": "D", "second": "E"}],
            "max_partition_size": 3,
        },
    }


def _with_partition(problem: dict, sides: list[int], *, warm_start: bool = False) -> dict:
    data = json.loads(json.dumps(problem))
    data["partition"] = dict(zip(data["vertices"], sides, strict=True))
    data["warm_start"] = warm_start
    return data


def _dump(data: dict) -> dict:
    return Model.model_validate(data).model_dump()


def _assignments(problem: dict, rng: random.Random, count: int) -> list[list[int]]:
    n = len(problem["vertices"])
    fixed = problem.get("fixed_partition", {})
    states = [[index % 2 for index in range(n)], [0] * n, [1] * n]
    states.append([fixed.get(vertex, index % 2) for index, vertex in enumerate(problem["vertices"])])
    states += [[rng.randint(0, 1) for _ in range(n)] for _ in range(count)]
    return states


def _random_instance(rng: random.Random) -> dict:
    n = rng.randint(0, 14)
    vertices = [f"v{index}" for index in range(n)]
    edges = []
    if n:
        density = rng.uniform(0.1, 0.7)
        edges = [
            {"source": vertices[a], "target": vertices[b], "weight": rng.randint(0, 9)}
            for a in range(n)
            for b in range(a + 1, n)
            if rng.random() < density
        ]
        edges += [
            {"source": rng.choice(vertices), "target": rng.choice(vertices), "weight": rng.randint(1, 5)}
            for _ in range(rng.randint(0, 3))
        ]  # self-loops and parallel edges
    pairs = lambda: [{"first": rng.choice(vertices), "second": rng.choice(vertices)} for _ in range(rng.randint(0, n))]
    problem = {
        "vertices": vertices,
        "edges": edges,
        "fixed_partition": {vertex: rng.randint(0, 1) for vertex in vertices if rng.random() < 0.2},
        "same_partition_pairs": pairs() if n else [],
        "different_partition_pairs": pairs() if n else [],
    }
    if n and rng.random() < 0.7:
        low = rng.randint(0, n // 2)
        high = rng.randint(low, n)
        if rng.random() < 0.8:
            problem["min_partition_size"] = low
        if rng.random() < 0.8:
            problem["max_partition_size"] = high
    return problem


def _cases() -> list[tuple[str, dict]]:
    """Every sample under several partitions, plus 60 seeded random instances x 6 partitions."""
    rng = random.Random(20261031)
    cases = []
    for name, problem in _samples().items():
        if problem.get("partition") and set(problem["partition"]) == set(problem["vertices"]):
            cases.append((f"{name}/as-given", json.loads(json.dumps(problem))))
        for k, sides in enumerate(_assignments(problem, rng, 8)):
            cases.append((f"{name}/{k}", _with_partition(problem, sides)))
    for seed in range(60):
        problem = _random_instance(rng)
        for k, sides in enumerate(_assignments(problem, rng, 2)):
            cases.append((f"random{seed}/{k}", _with_partition(problem, sides)))
    return cases


CASES = _cases()


def test_there_are_enough_cases():
    assert sum(name.startswith("random") for name, _ in CASES) >= 50 * 6
    assert {name.split("/")[0] for name, _ in CASES} >= set(_samples())


@pytest.mark.parametrize(("name", "data"), CASES, ids=[name for name, _ in CASES])
def test_constraint_costs_and_default_score_equal_the_oracle(name, data):
    dumped = _dump(data)
    expected = ref.constraint_costs(dumped)
    model = Model.model_validate(data)
    raw = max_cut.constraint_costs(model)
    assert list(raw) == NEW_NAMES
    for new_name, entry in raw.items():
        old_terms = expected[OLD_OF[new_name]]
        assert sum(entry["costs"]) == sum(old_terms), f"{name}: {new_name}"
    # Same length and order where 0.1.5 already had per-unit lists.
    for new_name in ("partition_size_bounds_cost", "partition_imbalance_cost", "cut_weight_cost"):
        assert raw[new_name]["costs"] == [float(value) for value in expected[OLD_OF[new_name]]]
    result = max_cut.calculate_score(model)
    assert (result.hard, result.soft) == ref.calculate_score(dumped)
    assert score(raw) == ref.calculate_score(dumped)
    assert model.cut_weight == ref.cut_weight(dumped)


def _old_selection(specs: list[Constraint]) -> list[dict]:
    return [
        {"name": OLD_OF[spec.name], "weight": spec.weight, "hard": spec.hard, "exponent": spec.exponent}
        for spec in specs
    ]


def _random_selection(rng: random.Random) -> list[Constraint]:
    """A random subset with integral weights and level flips (exponent 1: where 0.1.5 and 0.2.0 agree)."""
    picked = [spec for spec in get_constraints() if rng.random() < 0.7] or [get_constraints()[-1]]
    selection = []
    for spec in picked:
        weight = rng.choice([None, 0, 1, 2, 5])
        hard = rng.choice([None, False]) if spec.name == "cut_weight_cost" else rng.choice([None, True, False])
        selection.append(type(spec)(weight=weight, hard=hard))
    return selection


def test_selections_at_integral_weights_score_like_the_oracle():
    """Integral weights, any level, exponent 1: the per-side rounding cannot differ from 0.1.5's."""
    rng = random.Random(7)
    checked = 0
    for name, data in CASES[::3]:
        dumped = _dump(data)
        raw = max_cut.constraint_costs(Model.model_validate(data))
        for _ in range(3):
            selection = _random_selection(rng)
            assert score(raw, selection) == ref.score_costs(ref.constraint_costs(dumped), _old_selection(selection)), (
                name,
                selection,
            )
            checked += 1
    assert checked >= 300


@pytest.mark.parametrize("name", list(_samples()))
def test_the_zero_budget_solve_returns_the_oracle_start(name):
    """No warm start, a complete warm start and a partial one: the 0.1.5 start partition."""
    problem = _samples()[name]
    plain = Model.model_validate({**problem, "partition": {}})
    max_cut.solve(plain, time_limit_seconds=0)
    assert plain.partition == ref.start_partition(_dump({**problem, "partition": {}}))
    preset = Model.model_validate(_with_partition(problem, [1] * len(problem["vertices"])))
    max_cut.solve(preset, time_limit_seconds=0)
    assert preset.partition == ref.start_partition(preset.model_dump())  # warm_start False: ignored
    vertices = problem["vertices"]
    fixed = problem.get("fixed_partition", {})
    partial = {vertex: fixed.get(vertex, 1) for vertex in vertices[::2]}
    seeded = with_initial_solution(Model.model_validate({**problem, "partition": {}}), {"partition": partial})
    expected = ref.start_partition(seeded.model_dump())
    max_cut.solve(seeded, time_limit_seconds=0)
    assert seeded.partition == expected
    assert (seeded.score_hard, seeded.score_soft) == ref.calculate_score({**seeded.model_dump()})


def test_a_warm_start_against_a_fixed_side_is_kept_like_0_1_5():
    """A raw body (not with_initial_solution) may seed a fixed vertex on the other side: kept, penalized."""
    problem = _samples()["exact"]
    data = _with_partition(problem, [0, 0, 1, 1], warm_start=True)  # A is fixed to 1
    model = Model.model_validate(data)
    expected = ref.start_partition(_dump(data))
    max_cut.solve(model, time_limit_seconds=0)
    assert model.partition == expected
    assert (model.score_hard, model.score_soft) == ref.calculate_score(model.model_dump())


# --- The documented 0.2.0 differences ---------------------------------------


def test_the_exponent_shapes_each_side_not_each_term():
    """0.1.5 shaped every 0/1 term (an exponent had no effect); 0.2.0 shapes each side's count."""
    data = _with_partition(_samples()["path"], [0, 0, 0, 1])  # A, B, C fixed to 1, all on side 0
    dumped = _dump(data)
    raw = max_cut.constraint_costs(Model.model_validate(data))
    assert raw["fixed_partition_cost"]["costs"] == [3.0, 0.0]
    assert ref.score_costs(ref.constraint_costs(dumped), [{"name": "fixed_partition", "exponent": 2.0}]) == (-3, 0.0)
    assert score(raw, [FixedPartitionCost(exponent=2)]) == (-9, 0.0)


def test_fractional_hard_weights_round_per_side():
    data = _with_partition(_samples()["path"], [1, 1, 1, 0])  # 3 + 1 on [2, 2]: size [1, 1]
    dumped = _dump(data)
    raw = max_cut.constraint_costs(Model.model_validate(data))
    old = ref.score_costs(ref.constraint_costs(dumped), [{"name": "partition_size_bounds", "weight": 0.4}])
    assert old == (-1, 0.0)  # 0.1.5 rounded the hard total once: round(0.8)
    assert score(raw, [PartitionSizeBoundsCost(weight=0.4)]) == (0, 0.0)  # 0.2.0: round(0.4) per side


def test_a_hard_reward_is_refused_where_0_1_5_scored_it():
    data = _samples()["triangle"]
    dumped = _dump(data)
    assert ref.score_costs(ref.constraint_costs(dumped), [{"name": "maximize_cut_weight", "hard": True}]) == (5, 0.0)
    with pytest.raises(ValueError, match="is a reward"):
        score(max_cut.constraint_costs(Model.model_validate(data)), [CutWeightCost(hard=True)])


def test_a_partial_partition_is_refused_where_0_1_5_scored_it():
    data = _samples()["web-demo"]  # no partition at all
    # 0.1.5: a missing vertex is its own "side" for the pairs and side 0 for the sizes:
    # fixed A 1 + different (C, F) 1 + sizes (8 - 4) + (4 - 0) = 10.
    assert ref.calculate_score(_dump(data)) == (-10, 0.0)
    with pytest.raises(ValueError, match="partition is partial"):
        max_cut.calculate_score(Model.model_validate(data))
