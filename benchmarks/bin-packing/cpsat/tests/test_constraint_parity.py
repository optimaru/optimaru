"""C++ engine vs CP-SAT: the same assignment gives the same raw cost per constraint.

For every complete assignment of small feature-rich instances (bins 0..n-1,
CP-SAT's whole search space without symmetry breaking), each constraint is
selected alone at weight 1, the assignment is pinned in CP-SAT (assumptions on
its item/bin literals) and the CP-SAT term must equal the sum of the engine's
``constraint_costs()`` for that constraint — 0 exactly when the engine reports
no violation. Then 200 seeded random assignments of a six-item instance.
"""

from __future__ import annotations

import random
from itertools import product

import pytest
from bin_packing import constraint_costs, constraint_info, list_constraints
from bin_packing.constraints import get_constraints
from bin_packing.models import BinPackingProblemModel as Model
from ortools.sat.python import cp_model

from bin_packing_cpsat import build

# Over capacity, two colours shared, a duplicated (reversed) conflict pair.
QUAD = {
    "bin_capacity": 10,
    "items": [
        {"id": "A", "weight": 6, "color": "red"},
        {"id": "B", "weight": 5, "color": "red"},
        {"id": "C", "weight": 4, "color": "blue"},
        {"id": "D", "weight": 3, "color": "blue"},
    ],
    "conflicts": [{"first": "A", "second": "C"}, {"first": "C", "second": "A"}, {"first": "B", "second": "D"}],
}

# A colour carried by three items, one by a single item (never a violation), no conflicts.
TRIO_COLOURS = {
    "bin_capacity": 7,
    "items": [
        {"id": "x", "weight": 3, "color": "g"},
        {"id": "y", "weight": 3, "color": "g"},
        {"id": "z", "weight": 3, "color": "g"},
        {"id": "w", "weight": 4, "color": "solo"},
    ],
}

# Six items for the random pinned assignments.
SIX = {
    "bin_capacity": 12,
    "items": [
        {"id": "i1", "weight": 7, "color": "red"},
        {"id": "i2", "weight": 6, "color": "red"},
        {"id": "i3", "weight": 5, "color": "red"},
        {"id": "i4", "weight": 4, "color": "blue"},
        {"id": "i5", "weight": 3, "color": "blue"},
        {"id": "i6", "weight": 2},
    ],
    "conflicts": [
        {"first": "i1", "second": "i4"},
        {"first": "i2", "second": "i6"},
        {"first": "i6", "second": "i2"},
        {"first": "i3", "second": "i5"},
    ],
}

INSTANCES = {"quad": QUAD, "trio-colours": TRIO_COLOURS}


def _engine_raw(problem: dict, bins, name: str) -> float:
    model = Model.model_validate(problem)
    for item, bin_index in zip(model.items, bins, strict=True):
        item.bin = bin_index
    return sum(constraint_costs(model.to_core())[name]["costs"])


class _Pinned:
    """One constraint alone at weight 1 and its default level, ready to pin assignments."""

    def __init__(self, problem: dict, name: str) -> None:
        spec = next(spec for spec in get_constraints() if spec.name == name)
        self.built = build(Model.model_validate(problem), [type(spec)(weight=1)], symmetry_breaking=False)
        self.level = next(row.default_level for row in constraint_info() if row.name == name)
        self.built.objective.minimize()

    def cost(self, bins) -> int:
        model = self.built.model
        model.clear_assumptions()
        model.add_assumptions([self.built.context.assignments[item, b] for item, b in enumerate(bins)])
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert solver.solve(model) == cp_model.OPTIMAL
        return -self.built.objective.score(solver)[self.level]


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_every_pinned_assignment_costs_the_same_in_both_solvers(instance, name):
    problem = INSTANCES[instance]
    pinned = _Pinned(problem, name)
    n = len(problem["items"])
    checked = 0
    for bins in product(range(n), repeat=n):
        cpsat = pinned.cost(bins)
        engine = _engine_raw(problem, bins, name)
        assert cpsat == engine, f"{name} {bins}: CP-SAT {cpsat} != engine {engine}"
        checked += 1
    assert checked == n**n


@pytest.mark.parametrize("name", list_constraints())
def test_random_pinned_assignments_cost_the_same_in_both_solvers(name):
    pinned = _Pinned(SIX, name)
    rng = random.Random(f"bin-packing-parity:{name}")
    n = len(SIX["items"])
    nonzero = 0
    for _ in range(200):
        bins = [rng.randrange(n) for _ in range(n)]
        cpsat = pinned.cost(bins)
        engine = _engine_raw(SIX, bins, name)
        assert cpsat == engine, f"{name} {bins}: CP-SAT {cpsat} != engine {engine}"
        nonzero += engine != 0
    assert nonzero > 0  # the term is exercised, not only its zero


def test_quad_raw_costs_match_by_hand():
    """All four items in bin 0: load 18 on 10, two extra colours, all three listed pairs."""
    expected = {"bin_capacity_cost": 8, "color_conflict_cost": 2, "item_conflict_cost": 3, "used_bins_cost": 1}
    for name, value in expected.items():
        assert _Pinned(QUAD, name).cost((0, 0, 0, 0)) == value
        assert _engine_raw(QUAD, (0, 0, 0, 0), name) == value
    zero = {"bin_capacity_cost": 0, "color_conflict_cost": 0, "item_conflict_cost": 0, "used_bins_cost": 2}
    for name, value in zero.items():  # {A, D} {B, C}: within capacity, colours and pairs apart
        assert _Pinned(QUAD, name).cost((0, 1, 1, 0)) == value
        assert _engine_raw(QUAD, (0, 1, 1, 0), name) == value
