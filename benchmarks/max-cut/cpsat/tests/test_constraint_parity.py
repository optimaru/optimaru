"""C++ engine vs CP-SAT: the same partition gives the same raw cost per constraint.

For every partition of small feature-rich instances (CP-SAT's whole search
space) and 60 seeded random (instance, partition) pairs with up to 10 vertices,
each constraint is selected alone at weight 1, the partition is pinned in CP-SAT
(assumptions on the side literals, no symmetry breaking) and the CP-SAT term must
equal the engine's ``constraint_costs()`` total for that constraint (the sum of
its per-side costs) — 0 exactly when the engine reports no violation, and
``-cut`` for the ``cut_weight_cost`` reward.

Then the optimum: CP-SAT's optimum equals the brute-force optimum of the
engine's score, per constraint and for the default set; and the catalog and
the knobs (defaults, hard=False, weights, the reward) agree.
"""

from __future__ import annotations

import random
from itertools import product

import pytest
from max_cut import constraint_costs, constraint_info, list_constraints
from max_cut.constraints import (
    CutWeightCost,
    FixedPartitionCost,
    PartitionImbalanceCost,
    PartitionSizeBoundsCost,
    SamePartitionPairsCost,
    get_constraints,
    score,
)
from max_cut.models import MaxCutProblemModel as Model
from ortools.sat.python import cp_model

from max_cut_cpsat import build, catalog, solve


def _pairs(*pairs: str) -> list[dict]:
    return [{"first": pair[0], "second": pair[1]} for pair in pairs]


# Every fact: weighted, zero-weight, parallel and self-loop edges, fixed vertices,
# same / different pairs with repeats and self-pairs, both size bounds.
EVERYTHING = {
    "vertices": list("abcdefg"),
    "edges": [
        {"source": "a", "target": "b", "weight": 4},
        {"source": "b", "target": "a", "weight": 1},
        {"source": "a", "target": "c", "weight": 3},
        {"source": "c", "target": "d", "weight": 5},
        {"source": "d", "target": "e", "weight": 2},
        {"source": "e", "target": "f", "weight": 6},
        {"source": "f", "target": "g", "weight": 0},
        {"source": "g", "target": "a", "weight": 7},
        {"source": "c", "target": "c", "weight": 9},
        {"source": "b", "target": "e"},
    ],
    "fixed_partition": {"g": 0, "b": 1},
    "same_partition_pairs": _pairs("ad", "ad", "ee", "fb"),
    "different_partition_pairs": _pairs("cf", "gg", "de"),
    "min_partition_size": 3,
    "max_partition_size": 4,
}

# The web demo (8 vertices, exact size 4 + 4).
WEB_DEMO = {
    "vertices": list("ABCDEFGH"),
    "fixed_partition": {"A": 0},
    "same_partition_pairs": _pairs("BE"),
    "different_partition_pairs": _pairs("CF"),
    "min_partition_size": 4,
    "max_partition_size": 4,
    "edges": [
        {"source": source, "target": target, "weight": weight}
        for source, target, weight in [
            ("A", "B", 7),
            ("A", "C", 4),
            ("A", "H", 6),
            ("B", "D", 8),
            ("B", "E", 3),
            ("C", "D", 5),
            ("C", "F", 9),
            ("D", "G", 6),
            ("E", "F", 4),
            ("E", "H", 7),
            ("F", "G", 5),
            ("G", "H", 8),
            ("B", "G", 2),
            ("C", "E", 3),
        ]  # fmt: skip
    ],
}

# The 0.1.x triangle (nothing but the cut).
TRIANGLE = {
    "vertices": ["A", "B", "C"],
    "edges": [
        {"source": "A", "target": "B", "weight": 2},
        {"source": "B", "target": "C", "weight": 3},
        {"source": "A", "target": "C", "weight": 4},
    ],
}

INSTANCES = {"everything": EVERYTHING, "web-demo": WEB_DEMO, "triangle": TRIANGLE}


def _random_instance(rng: random.Random) -> dict:
    n = rng.randint(1, 10)
    vertices = [f"v{index}" for index in range(n)]
    edges = [
        {"source": vertices[a], "target": vertices[b], "weight": rng.randint(0, 9)}
        for a in range(n)
        for b in range(a + 1, n)
        if rng.random() < 0.45
    ]
    edges += [
        {"source": rng.choice(vertices), "target": rng.choice(vertices), "weight": rng.randint(1, 5)}
        for _ in range(rng.randint(0, 2))
    ]
    pairs = lambda: [{"first": rng.choice(vertices), "second": rng.choice(vertices)} for _ in range(rng.randint(0, n))]
    problem = {
        "vertices": vertices,
        "edges": edges,
        "fixed_partition": {vertex: rng.randint(0, 1) for vertex in vertices if rng.random() < 0.25},
        "same_partition_pairs": pairs(),
        "different_partition_pairs": pairs(),
    }
    low = rng.randint(0, n // 2)
    high = rng.randint(low, n)
    if rng.random() < 0.7:
        problem["min_partition_size"] = low
    if rng.random() < 0.7:
        problem["max_partition_size"] = high
    return problem


def _with_partition(problem: dict, sides) -> Model:
    model = Model.model_validate(problem)
    model.partition = {vertex: int(side) for vertex, side in zip(model.vertices, sides, strict=True)}
    return model


def _engine_raw(problem: dict, sides, name: str) -> float:
    return sum(constraint_costs(_with_partition(problem, sides))[name]["costs"])


def _single(name: str):
    """The constraint alone at weight 1 (the opt-in imbalance included), at its default level."""
    spec = next(spec for spec in get_constraints() if spec.name == name)
    return type(spec)(weight=1)


def _default_level(name: str) -> str:
    return next(row.default_level for row in constraint_info() if row.name == name)


class _Pinned:
    """One constraint's CP-SAT model, re-solved with the side literals pinned."""

    def __init__(self, problem: dict, name: str) -> None:
        self.built = build(Model.model_validate(problem), [_single(name)], symmetry_breaking=False)
        self.built.objective.minimize()
        self.level = _default_level(name)
        self.sides = [self.built.context.side[vertex] for vertex in problem["vertices"]]

    def raw(self, sides) -> int:
        model = self.built.model
        model.clear_assumptions()
        model.add_assumptions([literal if side else literal.Not() for literal, side in zip(self.sides, sides)])
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert solver.solve(model) == cp_model.OPTIMAL
        return -self.built.objective.score(solver)[self.level]


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_pinned_partitions_cost_the_same_in_both_solvers(instance, name):
    problem = INSTANCES[instance]
    pinned = _Pinned(problem, name)
    nonzero = 0
    for sides in product((0, 1), repeat=len(problem["vertices"])):
        engine = _engine_raw(problem, sides, name)
        cpsat = pinned.raw(sides)
        assert cpsat == engine, f"{name} {sides}: CP-SAT {cpsat} != engine {engine}"
        nonzero += engine != 0
    if instance == "everything":
        assert nonzero > 0, f"{name} is never non-zero on {instance}: the parity check proves nothing"


def test_random_instances_cost_the_same_in_both_solvers():
    """60 seeded random instances, one random partition each, every constraint."""
    rng = random.Random("max-cut parity")
    nonzero = dict.fromkeys(list_constraints(), 0)
    for _ in range(60):
        problem = _random_instance(rng)
        sides = [rng.randint(0, 1) for _ in problem["vertices"]]
        raw = constraint_costs(_with_partition(problem, sides))
        for name in list_constraints():
            engine = sum(raw[name]["costs"])
            assert _Pinned(problem, name).raw(sides) == engine, f"{name} {problem} {sides}"
            nonzero[name] += engine != 0
    assert all(count > 0 for count in nonzero.values()), nonzero


def test_raw_costs_match_by_hand():
    """a b c d e f g = 1 1 0 1 0 0 1 on EVERYTHING, every term by hand, in both solvers."""
    sides = [1, 1, 0, 1, 0, 0, 1]
    expected = {
        "fixed_partition_cost": 1,  # g (fixed 0) on 1; b (fixed 1) on 1
        "same_partition_pairs_cost": 1,  # (a, d) x2 together, (e, e) never split, (f, b) split
        "different_partition_pairs_cost": 2,  # (c, f) both 0, (g, g) always joined, (d, e) split
        "partition_size_bounds_cost": 0,  # 3 + 4 within [3, 4]
        "partition_imbalance_cost": 1,  # |3 - 4|
        "cut_weight_cost": -11,  # a-c 3, c-d 5, d-e 2, b-e 1, f-g 0 (the self-loop c-c never)
    }
    for name, value in expected.items():
        assert _engine_raw(EVERYTHING, sides, name) == value, name
        assert _Pinned(EVERYTHING, name).raw(sides) == value, name


def _brute_force(problem: dict, selection) -> tuple:
    best = None
    for sides in product((0, 1), repeat=len(problem["vertices"])):
        value = tuple(score(constraint_costs(_with_partition(problem, sides)), selection))
        best = value if best is None or value > best else best
    return best


RANDOM = _random_instance(random.Random(7))


@pytest.mark.parametrize("instance", [*INSTANCES, "random"])
@pytest.mark.parametrize("name", list_constraints())
def test_each_constraint_matches_brute_force(instance, name):
    problem = RANDOM if instance == "random" else INSTANCES[instance]
    selection = [_single(name)]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, selection))


@pytest.mark.parametrize("instance", [*INSTANCES, "random"])
def test_the_default_set_matches_brute_force(instance):
    problem = RANDOM if instance == "random" else INSTANCES[instance]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, get_constraints()))


# --- The catalog and the knobs ---------------------------------------------------


def test_cpsat_defaults_are_the_engine_defaults():
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info(), in the same order."""
    engine = {row.name: row for row in constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == ("soft" if row.reward else "either")
        assert entry["params"] == {"weight": None, "hard": None, "exponent": 1.0}
    assert [entry["default_enabled"] for entry in entries] == [True] * 4 + [False, True]


def test_demoting_a_hard_constraint_scores_like_the_engine():
    """hard=False moves the term to soft in both solvers; the CP-SAT result rescores identically."""
    selection = [
        PartitionSizeBoundsCost(hard=False) if spec.name == "partition_size_bounds_cost" else spec
        for spec in get_constraints()
    ]
    solved = solve(Model.model_validate(EVERYTHING), time_limit_seconds=10, constraints=selection)
    engine = score(constraint_costs(solved), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(EVERYTHING, selection))


def test_reweighting_and_promoting_score_like_the_engine():
    selection = [
        FixedPartitionCost(hard=False, weight=3),
        SamePartitionPairsCost(weight=2),
        PartitionImbalanceCost(weight=2, hard=True),
        CutWeightCost(weight=2),
    ]
    solved = solve(Model.model_validate(RANDOM), time_limit_seconds=10, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(RANDOM, selection))


def test_symmetry_breaking_keeps_the_optimum():
    for problem in (TRIANGLE, RANDOM, {**EVERYTHING, "fixed_partition": {}}):
        free = solve(Model.model_validate(problem), time_limit_seconds=10, symmetry_breaking=False)
        broken = solve(Model.model_validate(problem), time_limit_seconds=10)
        assert (free.score_hard, free.score_soft) == (broken.score_hard, broken.score_soft)


def test_the_reward_refuses_hard_true_in_cpsat_too():
    with pytest.raises(ValueError, match="is a reward"):
        solve(Model.model_validate(TRIANGLE), time_limit_seconds=1, constraints=[CutWeightCost(hard=True)])


def test_exponents_and_fractional_weights_stay_unsupported():
    with pytest.raises(ValueError, match="exponent is not supported"):
        solve(Model.model_validate(TRIANGLE), time_limit_seconds=1, constraints=[FixedPartitionCost(exponent=2)])
    with pytest.raises(ValueError, match="must be an integer"):
        solve(Model.model_validate(TRIANGLE), time_limit_seconds=1, constraints=[CutWeightCost(weight=0.5)])


def test_selections_by_name_and_mapping_work():
    solved = solve(
        Model.model_validate(TRIANGLE),
        time_limit_seconds=5,
        constraints=["fixed_partition_cost", {"name": "cut_weight_cost", "weight": 1}],
    )
    assert (solved.score_hard, solved.score_soft) == (0, 7.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        solve(Model.model_validate(TRIANGLE), time_limit_seconds=1, constraints=["maximize_cut_weight"])
