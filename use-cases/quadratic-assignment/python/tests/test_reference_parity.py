"""The C++ port computes exactly what the 0.1.5 pure-Python package computed.

`reference_quadratic_assignment` is the frozen 0.1.5 scorer (an oracle, plain
data in and out). On every sample — the web demo (`fixtures/web_demo.json`, a
copy of the web app's sample) and the package's own test fixtures, under every
permutation of the small ones — and on seeded random instances, the C++
`constraint_costs` totals equal the oracle's per constraint (under the 0.2.0
names), the default score and `total_cost` equal the oracle's, and the shaped
totals agree for any exponent.

0.1.5 listed `fixed_assignments` once per fixed location and
`forbidden_assignments` once per assignment entry; 0.2.0 lists both per location
(location order), zeros included. The non-zero terms are the same, so totals and
shaped totals match; the documented differences (hard rounds per unit, a
non-injective assignment is refused) have their own tests below.
"""

from __future__ import annotations

import itertools
import json
import random
from pathlib import Path

import pytest
import reference_quadratic_assignment as ref

import quadratic_assignment as qap
from quadratic_assignment.constraints import (
    AssignmentCost,
    Constraint,
    FlowDistanceCost,
    ForbiddenAssignmentsCost,
    LocationCapacityCost,
    PinnedAssignmentsCost,
    get_constraints,
    score,
)
from quadratic_assignment.models import QuadraticAssignmentProblemModel as Model

FIXTURES = Path(__file__).parent / "fixtures"
SPECS = {
    "pinned_assignments_cost": PinnedAssignmentsCost,
    "forbidden_assignments_cost": ForbiddenAssignmentsCost,
    "location_capacity_cost": LocationCapacityCost,
    "assignment_cost": AssignmentCost,
    "flow_distance_cost": FlowDistanceCost,
}
OLD_OF = {new: old for old, new in ref.RENAMED.items()}


def _samples() -> dict[str, dict]:
    three = {
        "facilities": ["F1", "F2", "F3"],
        "locations": ["L1", "L2", "L3"],
        "flows": [[0, 3, 1], [3, 0, 2], [1, 2, 0]],
        "distances": [[0, 1, 4], [1, 0, 2], [4, 2, 0]],
    }
    return {
        "web-demo": json.loads((FIXTURES / "web_demo.json").read_text()),
        # test_quadratic_assignment.py
        "manual": {
            "facilities": ["F1", "F2"],
            "locations": ["L1", "L2"],
            "flows": [[0, 3], [2, 0]],
            "distances": [[0, 4], [5, 0]],
        },
        "side": {
            "facilities": ["F1", "F2"],
            "locations": ["L1", "L2"],
            "flows": [[0, 3], [2, 0]],
            "distances": [[0, 4], [5, 0]],
            "assignment_costs": [[7, 1], [2, 8]],
            "fixed_assignments": {"L1": "F2"},
            "forbidden_assignments": [{"facility": "F2", "location": "L2"}],
            "facility_demands": {"F1": 5, "F2": 2},
            "location_capacities": {"L1": 3, "L2": 10},
        },
        # test_initial_solution.py
        "three": three,
        "three-fixed": {**three, "fixed_assignments": {"L1": "F1"}},
        "three-fixed-twice": {**three, "fixed_assignments": {"L1": "F1", "L2": "F1"}},
        # benchmarks/quadratic-assignment/cpsat tests/test_exact.py
        "exact-contracts": {
            "facilities": ["F1", "F2"],
            "locations": ["L1", "L2"],
            "flows": [[0, 1], [1, 0]],
            "distances": [[0, 1], [1, 0]],
            "assignment_costs": [[100, 1], [1, 100]],
            "fixed_assignments": {"L1": "F2"},
            "forbidden_assignments": [{"facility": "F1", "location": "L1"}],
            "facility_demands": {"F1": 5, "F2": 2},
            "location_capacities": {"L1": 2, "L2": 5},
        },
        # Repeated forbidden rows, a facility pinned twice, negative entries, the diagonal.
        "repeats": {
            "facilities": ["A", "B", "C", "D", "E"],
            "locations": ["v", "w", "x", "y", "z"],
            "flows": [[2, 1, 0, -3, 4], [1, 0, 5, 2, 0], [0, 5, 1, 1, 1], [-3, 2, 1, 0, 2], [4, 0, 1, 2, 3]],
            "distances": [[1, 2, 3, 4, 5], [2, 0, 1, -2, 3], [3, 1, 2, 1, 1], [4, -2, 1, 0, 2], [5, 3, 1, 2, 4]],
            "assignment_costs": [
                [3, -2, 0, 1, 4],
                [0, 0, 5, 1, 2],
                [-4, 1, 1, 1, 0],
                [2, 2, 2, 2, 2],
                [9, 0, -1, 3, 1],
            ],
            "fixed_assignments": {"z": "C", "v": "C", "x": "A"},
            "forbidden_assignments": [
                {"facility": "B", "location": "w"},
                {"facility": "B", "location": "w"},
                {"facility": "D", "location": "y"},
                {"facility": "E", "location": "v"},
                {"facility": "D", "location": "y"},
            ],
            "facility_demands": {"E": 7, "A": 3, "B": 0},
            "location_capacities": {"y": 2, "v": 0, "w": 9},
        },
    }


def _with_assignment(problem: dict, permutation, *, order=None) -> dict:
    """The problem with facility permutation[i] at location i (assignment keys in `order`)."""
    data = json.loads(json.dumps(problem))
    pairs = dict(zip(problem["locations"], permutation, strict=True))
    keys = order if order is not None else problem["locations"]
    data["assignment"] = {location: pairs[location] for location in keys}
    return data


def _dump(data: dict) -> dict:
    return Model.model_validate(data).model_dump()


def _cpp_costs(data: dict) -> dict[str, dict]:
    return qap.constraint_costs(Model.model_validate(data).to_core())


def _cpp_totals(data: dict) -> dict[str, float]:
    return {name: sum(entry["costs"]) for name, entry in _cpp_costs(data).items()}


def _ref_totals(data: dict) -> dict[str, float]:
    return {ref.RENAMED[name]: sum(terms) for name, terms in ref.constraint_costs(_dump(data)).items()}


def _assert_parity(data: dict) -> None:
    """Per-constraint raw totals, the order, the default score and total_cost equal the oracle."""
    totals = _cpp_totals(data)
    expected = _ref_totals(data)
    assert list(totals) == list(expected)
    assert totals == expected
    result = qap.calculate_score(Model.model_validate(data).to_core())
    assert (result.hard, result.soft) == ref.calculate_score(_dump(data))
    model = Model.model_validate(data)
    qap.calculate_score(model)
    assert model.total_cost == ref.total_cost(_dump(data))
    assert (model.score_hard, model.score_soft) == ref.calculate_score(_dump(data))


def _permutations(problem: dict, rng: random.Random, count: int):
    facilities = list(problem["facilities"])
    if len(facilities) <= 6:
        yield from itertools.permutations(facilities)
        return
    yield tuple(facilities)
    for _ in range(count):
        perm = facilities[:]
        rng.shuffle(perm)
        yield tuple(perm)


def test_the_catalog_maps_one_to_one():
    assert [ref.RENAMED[name] for name in ref.OLD_NAMES] == qap.list_constraints()


# ----------------------------------------------------------------------------- samples


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_matches_the_oracle_under_every_permutation(name):
    problem = _samples()[name]
    rng = random.Random(f"parity:{name}")
    for perm in _permutations(problem, rng, 40):
        _assert_parity(_with_assignment(problem, perm))


@pytest.mark.parametrize("name", list(_samples()))
def test_the_assignment_key_order_does_not_matter(name):
    """0.1.5 listed forbidden terms in assignment-dict order; the totals never depended on it."""
    problem = _samples()[name]
    rng = random.Random(f"order:{name}")
    for _ in range(5):
        perm = list(problem["facilities"])
        rng.shuffle(perm)
        order = list(problem["locations"])
        rng.shuffle(order)
        _assert_parity(_with_assignment(problem, perm, order=order))


@pytest.mark.parametrize("name", list(_samples()))
def test_a_solved_sample_matches_the_oracle(name):
    model = Model.model_validate(_samples()[name])
    config = qap.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = 500
    config.random_seed = 42
    qap.solve(model, config=config)
    data = model.model_dump()
    _assert_parity(data)
    assert (model.score_hard, model.score_soft) == ref.calculate_score(data)
    assert model.total_cost == ref.total_cost(data)


def test_the_identity_start_matches_the_oracle():
    """The zero-budget solve returns the 0.1.5 starting point (facility i at location i)."""
    for name, problem in _samples().items():
        model = Model.model_validate(problem)
        qap.solve(model, time_limit_seconds=0)
        identity = _with_assignment(problem, problem["facilities"])
        assert model.assignment == identity["assignment"], name
        assert (model.score_hard, model.score_soft) == ref.calculate_score(_dump(identity)), name


# ----------------------------------------------------------------------------- random instances


def _random_instance(rng: random.Random) -> dict:
    """A valid model with every feature: negative entries, repeated forbidden rows, a facility
    pinned to several locations, partial demand / capacity maps, an optional direct-cost matrix."""
    n = rng.randint(1, 8)
    facilities = [f"f{index}" for index in range(n)]
    locations = [f"l{index}" for index in range(n)]
    data = {
        "facilities": facilities,
        "locations": locations,
        "flows": [[rng.randint(-3, 9) for _ in facilities] for _ in facilities],
        "distances": [[rng.randint(-3, 9) for _ in locations] for _ in locations],
        "fixed_assignments": {
            location: rng.choice(facilities) for location in rng.sample(locations, rng.randint(0, max(1, n // 2)))
        },
        "forbidden_assignments": [
            {"facility": rng.choice(facilities), "location": rng.choice(locations)}
            for _ in range(rng.randint(0, 2 * n))
        ],
        "facility_demands": {facility: rng.randint(0, 9) for facility in facilities if rng.random() < 0.6},
        "location_capacities": {location: rng.randint(0, 9) for location in locations if rng.random() < 0.6},
    }
    if data["forbidden_assignments"] and rng.random() < 0.5:
        data["forbidden_assignments"].append(dict(data["forbidden_assignments"][0]))  # a repeated row
    if rng.random() < 0.7:
        data["assignment_costs"] = [[rng.randint(-5, 9) for _ in locations] for _ in facilities]
    Model.model_validate(data)  # the generator only builds valid problems
    return data


def _random_permutation(problem: dict, rng: random.Random) -> list[str]:
    perm = list(problem["facilities"])
    rng.shuffle(perm)
    return perm


SEEDS = range(60)


@pytest.mark.parametrize("seed", SEEDS)
def test_random_instances_match_the_oracle(seed):
    rng = random.Random(seed)
    problem = _random_instance(rng)
    for _ in range(6):
        _assert_parity(_with_assignment(problem, _random_permutation(problem, rng)))


@pytest.mark.parametrize("seed", SEEDS)
def test_scores_match_the_oracle_at_integer_weights(seed):
    """score() re-prices like 0.1.5 score_costs at integer weights and exponent 1, whatever the levels."""
    rng = random.Random(1000 + seed)
    problem = _random_instance(rng)
    data = _with_assignment(problem, _random_permutation(problem, rng))
    raw = _cpp_costs(data)
    old = ref.constraint_costs(_dump(data))
    specs = get_constraints()
    rng.shuffle(specs)
    picked: list[Constraint] = []
    for spec in specs[: rng.randint(1, len(specs))]:
        picked.append(type(spec)(weight=rng.choice([None, 0, 1, 2, 5]), hard=rng.choice([None, True, False])))
    expected = ref.score_costs(old, [(OLD_OF[spec.name], spec.weight, spec.hard, 1.0) for spec in picked])
    assert tuple(score(raw, picked)) == pytest.approx(expected)
    # ... and the engine scores the same picks the same way.
    core = Model.model_validate(data).to_core()
    zero = qap.SolverConfig()
    zero.spent_limit_seconds = 0
    core.warm_start = True
    qap.Solver(zero).solve_with_constraints(core, picked)
    assert (core.score.hard, core.score.soft) == pytest.approx(expected)


@pytest.mark.parametrize("seed", range(25))
@pytest.mark.parametrize("exponent", [0.5, 2.0, 3.0])
def test_every_constraint_shapes_like_the_oracle(seed, exponent):
    """The per-location lists hold the oracle's non-zero terms (plus zeros), and the objectives one
    total each as 0.1.5 did: every exponent shapes the same values. (Soft level, so per-unit vs
    once-at-the-end hard rounding does not enter.)"""
    rng = random.Random(3000 + seed)
    problem = _random_instance(rng)
    data = _with_assignment(problem, _random_permutation(problem, rng))
    raw = _cpp_costs(data)
    old = ref.constraint_costs(_dump(data))
    for name, spec_class in SPECS.items():
        picked = [spec_class(exponent=exponent, hard=False, weight=2)]
        expected = ref.score_costs(old, [(OLD_OF[name], 2, False, exponent)])
        assert tuple(score(raw, picked)) == pytest.approx(expected), name
        nonzero = sorted(value for value in raw[name]["costs"] if value != 0)
        assert nonzero == sorted(float(value) for value in old[OLD_OF[name]] if value != 0), name


# ----------------------------------------------------------------------------- documented 0.2.0 changes


def test_fractional_hard_weights_now_round_per_unit():
    """0.1.5 rounded the hard total once (0.5 + 0.5 -> -1); the engine rounds each location (-2)."""
    data = _with_assignment(_samples()["three-fixed-twice"], ["F2", "F3", "F1"])  # both pins broken
    old = ref.constraint_costs(_dump(data))
    assert old["fixed_assignments"] == [1, 1]
    assert ref.score_costs(old, [("fixed_assignments", 0.5, None, 1.0)]) == (-1, 0.0)
    raw = _cpp_costs(data)
    assert raw["pinned_assignments_cost"]["costs"] == [1.0, 1.0, 0.0]
    assert score(raw, [PinnedAssignmentsCost(weight=0.5)]) == (-2, 0.0)


def test_unit_lists_are_per_location_now():
    """0.1.5: one term per FIXED location / per assignment ENTRY; 0.2.0: one per location, in order."""
    data = _with_assignment(_samples()["repeats"], ["A", "B", "C", "D", "E"], order=["z", "y", "x", "w", "v"])
    old = ref.constraint_costs(_dump(data))
    assert len(old["fixed_assignments"]) == 3 and len(old["forbidden_assignments"]) == 5
    raw = _cpp_costs(data)
    assert all(len(raw[name]["costs"]) == 5 for name in SPECS if not raw[name]["global"])
    assert raw["forbidden_assignments_cost"]["costs"] == [0.0, 1.0, 0.0, 1.0, 0.0]  # B at w, D at y (rows deduped)
    assert old["forbidden_assignments"] == [0, 1, 0, 1, 0][::-1]  # the same terms, in assignment-dict order


def test_a_non_injective_assignment_is_refused_now():
    """0.1.5 scored one facility at two locations; the C++ model cannot represent it (to_core raises)."""
    data = json.loads(json.dumps(_samples()["three"]))
    data["assignment"] = {"L1": "F1", "L2": "F1", "L3": "F3"}
    assert ref.calculate_score(_dump(data)) == (0, -12.0)  # F1 counted at L1 and L2: 2 * (4*1 + 2*1)
    with pytest.raises(ValueError, match="'F1' is assigned to both 'L1' and 'L2'"):
        qap.calculate_score(Model.model_validate(data))


def test_a_partial_assignment_raises_on_the_model_like_0_1_5():
    data = json.loads(json.dumps(_samples()["web-demo"]))
    with pytest.raises(KeyError):
        ref.constraint_costs(_dump(data))
    with pytest.raises(ValueError, match="missing"):
        qap.constraint_costs(Model.model_validate(data))
