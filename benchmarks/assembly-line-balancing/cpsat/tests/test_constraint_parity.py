"""C++ engine vs CP-SAT: the same assignment gives the same raw cost per constraint.

For every complete assignment of small feature-rich instances (stations 0..n-1,
CP-SAT's whole search space), each constraint is selected alone at weight 1,
the assignment is pinned in CP-SAT (assumptions on its station literals) and
the CP-SAT term must equal the sum of the engine's ``constraint_costs()`` for
that constraint — 0 exactly when the engine reports no violation. The instances
include the hand-calculated fixture H of the C++ tests, which catches the old
group-anchor and duplicate-forbidden encodings.

Then the optimum: CP-SAT's optimum equals the brute-force optimum of the
engine's score, per constraint and for the default set.
"""

from __future__ import annotations

from itertools import product

import pytest
from assembly_line_balancing import constraint_costs, constraint_info, list_constraints
from assembly_line_balancing.constraints import (
    StationCapacityCost,
    TaskPrecedenceCost,
    UsedStationsCost,
    WorkloadImbalanceCost,
    get_constraints,
    score,
)
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel as Model
from ortools.sat.python import cp_model

from assembly_line_balancing_cpsat import build, catalog, solve

FIXTURE_H = {
    "cycle_time": 5,
    "max_stations": 3,
    "tasks": [
        {"id": "A", "duration": 3, "fixed_station": 0, "forbidden_stations": [1, 1]},
        {"id": "B", "duration": 3, "min_station": 1},
        {"id": "C", "duration": 2, "fixed_station": 1},
        {"id": "D", "duration": 6, "max_station": 2, "forbidden_stations": [0]},
    ],
    "precedences": [{"before": "A", "after": "B"}, {"before": "C", "after": "D"}],
    "same_station_groups": [{"id": "g1", "task_ids": ["A", "C", "C"]}, {"id": "g2", "task_ids": ["B", "A", "C"]}],
    "different_station_pairs": [
        {"first": "A", "second": "C"},
        {"first": "B", "second": "D"},
        {"first": "B", "second": "B"},
    ],
}

# Three tasks: duplicate arcs, a three-member group, max_stations, overlapping domains.
TRIO = {
    "cycle_time": 4,
    "max_stations": 2,
    "tasks": [
        {"id": "x", "duration": 2, "max_station": 1},
        {"id": "y", "duration": 3, "forbidden_stations": [2, 0, 2]},
        {"id": "z", "duration": 1, "min_station": 1},
    ],
    "precedences": [{"before": "x", "after": "y"}, {"before": "x", "after": "y"}, {"before": "z", "after": "y"}],
    "same_station_groups": [{"id": "all", "task_ids": ["x", "y", "z"]}],
    "different_station_pairs": [{"first": "x", "second": "z"}],
}

# A pure SALBP line (symmetry breaking applies).
PLAIN = {
    "cycle_time": 6,
    "tasks": [
        {"id": "a", "duration": 4},
        {"id": "b", "duration": 3},
        {"id": "c", "duration": 2},
        {"id": "d", "duration": 3},
    ],
    "precedences": [{"before": "a", "after": "c"}, {"before": "b", "after": "d"}],
}

INSTANCES = {"fixture-h": FIXTURE_H, "trio": TRIO, "plain": PLAIN}


def _assigned(problem: dict, stations) -> Model:
    model = Model.model_validate(problem)
    for task, station in zip(model.tasks, stations, strict=True):
        task.station = station
    return model


def _engine_raw(problem: dict, stations, name: str) -> float:
    return sum(constraint_costs(_assigned(problem, stations).to_core())[name]["costs"])


def _single(spec):
    """The constraint alone at weight 1 (the opt-in workload objective too), at its default level."""
    return type(spec)(weight=1)


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_pinned_assignments_cost_the_same_in_both_solvers(instance, name):
    problem = INSTANCES[instance]
    spec = next(spec for spec in get_constraints() if spec.name == name)
    built = build(Model.model_validate(problem), [_single(spec)], symmetry_breaking=False)
    level = next(row.default_level for row in constraint_info() if row.name == name)
    built.objective.minimize()
    n = len(problem["tasks"])
    checked = 0
    for stations in product(range(n), repeat=n):
        built.model.clear_assumptions()
        built.model.add_assumptions([built.context.assigned[task, station] for task, station in enumerate(stations)])
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert solver.solve(built.model) == cp_model.OPTIMAL
        cpsat = -built.objective.score(solver)[level]
        engine = _engine_raw(problem, stations, name)
        assert cpsat == engine, f"{name} {stations}: CP-SAT {cpsat} != engine {engine}"
        checked += 1
    assert checked == n**n


def test_fixture_h_raw_costs_match_by_hand():
    """The hand-calculated totals of the C++ tests, in CP-SAT."""
    stations = (1, 0, 1, 3)
    expected = {
        "station_capacity_cost": 1,
        "task_precedence_cost": 1,
        "max_stations_cost": 1,
        "task_station_bounds_cost": 2,
        "fixed_task_stations_cost": 1,
        "forbidden_task_stations_cost": 1,  # [1, 1] counts once (the old encoding said 2)
        "same_station_groups_cost": 1,  # {B, A, C} on {0, 1} (the old anchor encoding said 2)
        "different_station_pairs_cost": 2,
        "used_stations_cost": 3,
        "workload_imbalance_cost": 3,
    }
    for spec in get_constraints():
        built = build(Model.model_validate(FIXTURE_H), [_single(spec)], symmetry_breaking=False)
        built.objective.minimize()
        built.model.add_assumptions([built.context.assigned[task, station] for task, station in enumerate(stations)])
        solver = cp_model.CpSolver()
        assert solver.solve(built.model) == cp_model.OPTIMAL
        level = (
            "hard"
            if expected[spec.name] and spec.name not in {"used_stations_cost", "workload_imbalance_cost"}
            else "soft"
        )
        assert -built.objective.score(solver)[level] == expected[spec.name], spec.name
        assert _engine_raw(FIXTURE_H, stations, spec.name) == expected[spec.name]


def _brute_force(problem: dict, selection) -> tuple:
    n = len(problem["tasks"])
    best = None
    for stations in product(range(n), repeat=n):
        value = tuple(score(constraint_costs(_assigned(problem, stations).to_core()), selection))
        best = value if best is None or value > best else best
    return best


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_each_constraint_matches_brute_force(instance, name):
    problem = INSTANCES[instance]
    spec = next(spec for spec in get_constraints() if spec.name == name)
    selection = [_single(spec)]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, selection))


@pytest.mark.parametrize("instance", list(INSTANCES))
def test_the_default_set_matches_brute_force(instance):
    problem = INSTANCES[instance]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, get_constraints()))


def test_symmetry_breaking_keeps_the_optimum_of_a_plain_line():
    with_breaking = solve(Model.model_validate(PLAIN), time_limit_seconds=10)
    built = build(Model.model_validate(PLAIN), symmetry_breaking=False)
    solver = cp_model.CpSolver()
    assert built.objective.solve(solver) == cp_model.OPTIMAL
    assert built.objective.score(solver) == {"hard": with_breaking.score_hard, "soft": with_breaking.score_soft}
    # The used stations form a prefix.
    used = sorted({task.station for task in with_breaking.tasks})
    assert used == list(range(len(used)))


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
        assert entry["kind"] == "either"  # every assembly-line constraint is flippable
        assert entry["params"] == {"weight": None, "hard": None, "exponent": 1.0}
    assert {entry["name"]: entry["default_enabled"] for entry in entries}["workload_imbalance_cost"] is False


def test_demoting_hard_constraints_scores_like_the_engine():
    """hard=False lands on soft in both solvers: the reported score is the engine's under the same selection."""
    selection = [StationCapacityCost(hard=False), TaskPrecedenceCost(hard=False), *get_constraints()[2:]]
    solved = solve(Model.model_validate(FIXTURE_H), time_limit_seconds=10, constraints=selection)
    engine = score(constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(FIXTURE_H, selection))


def test_the_opt_in_workload_objective_is_off_by_default():
    model = Model.model_validate(PLAIN)
    plain = solve(model, time_limit_seconds=10)
    weighted = solve(
        model, time_limit_seconds=10, constraints=[*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)]
    )
    assert plain.score_soft == -plain.stations_used
    raw = constraint_costs(weighted.to_core())
    assert weighted.score_soft == -(raw["used_stations_cost"]["costs"][0] + raw["workload_imbalance_cost"]["costs"][0])


@pytest.mark.parametrize(
    "selection, message",
    [
        ([UsedStationsCost(exponent=2.0)], "exponent is not supported by CP-SAT"),
        ([UsedStationsCost(weight=0.5)], "must be an integer for CP-SAT"),
        ([{"name": "minimize_stations"}], "unknown constraint"),
    ],
)
def test_what_cpsat_cannot_honour_is_refused(selection, message):
    with pytest.raises(ValueError, match=message):
        solve(Model.model_validate(PLAIN), time_limit_seconds=1, constraints=selection)
