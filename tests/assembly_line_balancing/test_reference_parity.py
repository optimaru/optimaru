"""The C++ domain computes exactly what the pure-Python reference computes.

`reference_assembly_line_balancing` is an independent scorer and construction
(an oracle, plain data in and out). On every sample — the web demo
(`fixtures/web_demo.json`, a copy of the web app's sample) and the package's own
test fixtures — and on seeded random instances, the C++ `constraint_costs`
totals equal the oracle's per constraint (under the engine names), the default
score equals the oracle's, and the zero-budget construction returns the
oracle's stations.

Complete assignments are compared directly. A partial assignment differs by
design (the engine convention: an unassigned task contributes nothing); it is compared against the oracle on the problem restricted to the
assigned tasks, which states that convention exactly.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import pytest

import optimaru.assembly_line_balancing as alb
from optimaru.assembly_line_balancing.constraints import Constraint, get_constraints, score
from optimaru.assembly_line_balancing.models import AssemblyLineBalancingProblemModel as Model

from . import reference_assembly_line_balancing as ref

FIXTURES = Path(__file__).parent / "fixtures"


def _samples() -> dict[str, dict]:
    tasks3 = [{"id": "A", "duration": 3}, {"id": "B", "duration": 3}, {"id": "C", "duration": 2}]
    return {
        "web-demo": json.loads((FIXTURES / "web_demo.json").read_text()),
        # test_assembly_line_balancing.py
        "manual": {
            "cycle_time": 5,
            "tasks": [{"id": "A", "duration": 3, "station": 1}, {"id": "B", "duration": 3, "station": 0}],
            "precedences": [{"before": "A", "after": "B"}],
        },
        "zoning": {
            "cycle_time": 5,
            "max_stations": 1,
            "tasks": [
                {"id": "A", "duration": 3, "station": 1, "max_station": 1, "fixed_station": 0},
                {"id": "B", "duration": 3, "station": 0, "forbidden_stations": [0]},
            ],
            "precedences": [{"before": "A", "after": "B"}],
            "same_station_groups": [{"id": "zone", "task_ids": ["A", "B"]}],
            "different_station_pairs": [{"first": "A", "second": "B"}],
        },
        # test_initial_solution.py
        "three-tasks": {"cycle_time": 5, "tasks": tasks3, "precedences": [{"before": "A", "after": "B"}]},
        "three-tasks-max-2": {
            "cycle_time": 5,
            "max_stations": 2,
            "tasks": tasks3,
            "precedences": [{"before": "A", "after": "B"}],
        },
        # The C++ tests' fixture H (every constraint positive).
        "fixture-h": {
            "cycle_time": 5,
            "max_stations": 3,
            "tasks": [
                {"id": "A", "duration": 3, "station": 1, "fixed_station": 0, "forbidden_stations": [1, 1]},
                {"id": "B", "duration": 3, "station": 0, "min_station": 1},
                {"id": "C", "duration": 2, "station": 1, "fixed_station": 1},
                {"id": "D", "duration": 6, "station": 3, "max_station": 2, "forbidden_stations": [0]},
            ],
            "precedences": [{"before": "A", "after": "B"}, {"before": "C", "after": "D"}],
            "same_station_groups": [
                {"id": "g1", "task_ids": ["A", "C", "C"]},
                {"id": "g2", "task_ids": ["B", "A", "C"]},
            ],
            "different_station_pairs": [
                {"first": "A", "second": "C"},
                {"first": "B", "second": "D"},
                {"first": "B", "second": "B"},
            ],
        },
    }


def _with_stations(problem: dict, stations: list[int], *, warm_start: bool = False) -> dict:
    data = json.loads(json.dumps(problem))
    for task, station in zip(data["tasks"], stations, strict=True):
        task["station"] = station
    data["warm_start"] = warm_start
    return data


def _cpp_totals(data: dict) -> dict[str, float]:
    costs = alb.constraint_costs(Model.model_validate(data).to_core())
    return {name: sum(entry["costs"]) for name, entry in costs.items()}


def _cpp_score(data: dict) -> tuple[int, float]:
    result = alb.calculate_score(Model.model_validate(data).to_core())
    return result.hard, result.soft


def _assert_parity(data: dict) -> None:
    """Complete assignment: per-constraint raw totals and the default score equal the oracle."""
    dump = Model.model_validate(data).model_dump()
    assert all(task["station"] >= 0 for task in dump["tasks"])
    assert _cpp_totals(data) == ref.totals(dump)
    assert _cpp_score(data) == ref.default_score(dump)


def _random_complete(problem: dict, rng: random.Random) -> list[int]:
    top = len(problem["tasks"]) + 2
    return [rng.randrange(top) for _ in problem["tasks"]]


def _constructed(data: dict) -> list[int]:
    core = Model.model_validate(data).to_core()
    config = alb.SolverConfig()
    config.spent_limit_seconds = 0
    alb.Solver(config).solve(core)
    return core.assignment()


# ----------------------------------------------------------------------------- samples


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_matches_the_oracle(name):
    problem = _samples()[name]
    rng = random.Random(f"parity:{name}")
    states = [ref.construct(problem)]  # the reference starting point
    if all(task.get("station", -1) >= 0 for task in problem["tasks"]):
        states.append([task["station"] for task in problem["tasks"]])  # the sample as given
    states += [_random_complete(problem, rng) for _ in range(40)]
    for stations in states:
        _assert_parity(_with_stations(problem, stations))


@pytest.mark.parametrize("name", list(_samples()))
def test_every_sample_constructs_like_the_oracle(name):
    problem = _samples()[name]
    assert _constructed(problem) == ref.construct(problem)
    rng = random.Random(f"construct:{name}")
    for _ in range(10):
        # A partial warm start: some tasks seeded, the rest constructed.
        seed = [rng.randrange(len(problem["tasks"]) + 2) if rng.random() < 0.4 else -1 for _ in problem["tasks"]]
        data = _with_stations(problem, seed, warm_start=True)
        assert _constructed(data) == ref.construct(data)


@pytest.mark.parametrize("name", list(_samples()))
def test_a_solved_sample_matches_the_oracle(name):
    problem = _samples()[name]
    model = Model.model_validate(problem)
    config = alb.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = 300
    config.random_seed = 42
    model = alb.solve(model, config=config)
    _assert_parity(model.model_dump())
    assert (model.score_hard, model.score_soft) == ref.default_score(model.model_dump())


# ----------------------------------------------------------------------------- random instances


def _random_instance(rng: random.Random) -> dict:
    """A valid model with every feature (duplicates, self-pairs, groups of 3+, domains, fixed, forbidden)."""
    n = rng.randint(1, 9)
    ids = [f"t{index}" for index in range(n)]
    tasks = []
    for task_id in ids:
        task = {"id": task_id, "duration": rng.randint(1, 7), "min_station": rng.randrange(min(n, 3))}
        if rng.random() < 0.4:
            task["max_station"] = rng.randint(task["min_station"], n - 1)
        top = task.get("max_station", n - 1)
        forbidden = [rng.randrange(n) for _ in range(rng.randint(0, 3))]
        allowed = [s for s in range(task["min_station"], top + 1) if s not in forbidden]
        if allowed and rng.random() < 0.3:
            task["fixed_station"] = rng.choice(allowed)
        if forbidden:
            task["forbidden_stations"] = forbidden
        tasks.append(task)
    order = ids[:]
    rng.shuffle(order)
    precedences = []
    for _ in range(rng.randint(0, 2 * n)):
        a, b = sorted(rng.sample(range(n), 2)) if n > 1 else (0, 0)
        if a != b:
            precedences.append({"before": order[a], "after": order[b]})  # acyclic: follows `order`
    data = {
        "cycle_time": rng.randint(4, 12),
        "tasks": tasks,
        "precedences": precedences,
        "same_station_groups": [
            {"id": f"g{g}", "task_ids": [rng.choice(ids) for _ in range(rng.randint(2, 4))]}
            for g in range(rng.randint(0, 3))
        ],
        "different_station_pairs": [
            {"first": rng.choice(ids), "second": rng.choice(ids)} for _ in range(rng.randint(0, n))
        ],
    }
    if rng.random() < 0.5:
        data["max_stations"] = rng.randint(1, n + 1)
    Model.model_validate(data)  # the generator only builds valid problems
    return data


SEEDS = range(60)


@pytest.mark.parametrize("seed", SEEDS)
def test_random_instances_match_the_oracle(seed):
    rng = random.Random(seed)
    problem = _random_instance(rng)
    for _ in range(5):
        _assert_parity(_with_stations(problem, _random_complete(problem, rng)))
    assert _constructed(problem) == ref.construct(problem)
    seed_stations = [rng.randrange(len(problem["tasks"]) + 2) if rng.random() < 0.5 else -1 for _ in problem["tasks"]]
    warm = _with_stations(problem, seed_stations, warm_start=True)
    assert _constructed(warm) == ref.construct(warm)


@pytest.mark.parametrize("seed", SEEDS)
def test_scores_match_the_oracle_at_integer_weights(seed):
    """score() re-prices like the reference score_costs at integer weights and exponent 1, whatever the levels."""
    rng = random.Random(1000 + seed)
    problem = _random_instance(rng)
    data = _with_stations(problem, _random_complete(problem, rng))
    raw = alb.constraint_costs(Model.model_validate(data).to_core())
    reference_costs = ref.constraint_costs(Model.model_validate(data).model_dump())
    reference_name = {new: ref_name for ref_name, new in ref.ENGINE_NAME.items()}
    specs = get_constraints()
    rng.shuffle(specs)
    picked: list[Constraint] = []
    for spec in specs[: rng.randint(1, len(specs))]:
        weight = rng.choice([None, 0, 1, 2, 5])
        level = rng.choice([None, "hard", "soft"])
        picked.append(type(spec)(weight=weight, level=level))
    expected = ref.score_costs(
        reference_costs,
        [
            (reference_name[spec.name], spec.weight, {None: None, "hard": True, "soft": False}[spec.level], 1.0)
            for spec in picked
        ],
    )
    assert tuple(score(raw, picked)) == pytest.approx(expected)


@pytest.mark.parametrize("seed", range(30))
def test_partial_assignments_follow_the_engine_convention(seed):
    """An unassigned task contributes nothing: the oracle on the problem without it."""
    rng = random.Random(2000 + seed)
    problem = _random_instance(rng)
    stations = [rng.randrange(len(problem["tasks"]) + 2) if rng.random() < 0.6 else -1 for _ in problem["tasks"]]
    data = _with_stations(problem, stations)
    restricted = ref.restricted_to_assigned(Model.model_validate(data).model_dump())
    assert _cpp_totals(data) == ref.totals(restricted)
    assert _cpp_score(data) == ref.default_score(restricted)


def test_fixture_h_matches_the_hand_calculation():
    """The C++ tests' hand-calculated totals, through the Python path and the oracle alike."""
    problem = _samples()["fixture-h"]
    expected = {
        "station_capacity_cost": 1,
        "task_precedence_cost": 1,
        "max_stations_cost": 1,
        "task_station_bounds_cost": 2,
        "fixed_task_stations_cost": 1,
        "forbidden_task_stations_cost": 1,
        "same_station_groups_cost": 1,
        "different_station_pairs_cost": 2,
        "used_stations_cost": 3,
        "workload_imbalance_cost": 3,
    }
    assert _cpp_totals(problem) == ref.totals(problem) == expected
    assert _cpp_score(problem) == ref.default_score(problem) == (-10, -3.0)


def test_the_unassigned_web_demo_scores_zero():
    """The reference charges the unassigned web demo -18hard (bounds 16, fixed 1, pairs 1); the engine nothing."""
    problem = _samples()["web-demo"]
    assert ref.default_score(problem) == (-18, 0.0)
    assert _cpp_score(problem) == (0, 0.0)
    assert set(_cpp_totals(problem).values()) == {0}
