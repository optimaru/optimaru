"""C++ engine vs CP-SAT: the same permutation gives the same raw cost per constraint.

For every permutation of small feature-rich instances (CP-SAT's whole search
space: a pinned facility, a facility pinned twice, a repeated forbidden row,
partial demand / capacity maps, diagonal flows, negative entries) and 100
seeded random permutations of a 6-facility instance, each constraint is
selected alone at weight 1, the permutation is pinned in CP-SAT (assumptions on
the x[f, l] literals) and the CP-SAT term must equal the engine's
``constraint_costs()`` total for that constraint — 0 exactly when the engine
reports no violation.

Then the optimum: CP-SAT's optimum equals the brute-force optimum of the
engine's score, per constraint and for the default set; and the catalog and
the knobs (defaults, level="soft", weights) agree.
"""

from __future__ import annotations

import random
from itertools import permutations

import pytest
from ortools.sat.python import cp_model
from quadratic_assignment import Solver, SolverConfig, constraint_costs, constraint_info, list_constraints
from quadratic_assignment import solve as engine_solve
from quadratic_assignment.constraints import (
    AssignmentCost,
    FlowDistanceCost,
    ForbiddenAssignmentsCost,
    Level,
    LocationCapacityCost,
    PinnedAssignmentsCost,
    get_constraints,
    score,
)
from quadratic_assignment.models import QuadraticAssignmentProblemModel as Model

from quadratic_assignment_cpsat import build, catalog, solve

# Every fact, non-negative data (the regime the 0.1.x CP-SAT model assumed).
EVERYTHING = {
    "facilities": ["A", "B", "C", "D", "E"],
    "locations": ["v", "w", "x", "y", "z"],
    "flows": [[2, 3, 0, 1, 4], [3, 0, 5, 2, 0], [0, 5, 1, 1, 1], [1, 2, 1, 0, 2], [4, 0, 1, 2, 0]],
    "distances": [[1, 2, 3, 4, 5], [2, 0, 1, 2, 3], [3, 1, 2, 1, 1], [4, 2, 1, 0, 2], [5, 3, 1, 2, 4]],
    "assignment_costs": [[3, 2, 0, 1, 4], [0, 0, 5, 1, 2], [4, 1, 1, 1, 0], [2, 2, 2, 2, 2], [9, 0, 1, 3, 1]],
    "fixed_assignments": {"z": "C", "v": "C", "x": "A"},
    "forbidden_assignments": [
        {"facility": "B", "location": "w"},
        {"facility": "B", "location": "w"},
        {"facility": "D", "location": "y"},
        {"facility": "E", "location": "v"},
    ],
    "facility_demands": {"E": 7, "A": 3, "B": 0},
    "location_capacities": {"y": 2, "v": 0, "w": 9},
}

# Negative flows, distances and direct costs (accepted by the model; scored as given).
SIGNED = {
    "facilities": ["P", "Q", "R", "S"],
    "locations": ["k", "l", "m", "n"],
    "flows": [[1, -3, 2, 0], [2, 0, -1, 4], [-2, 1, 0, 3], [0, 5, -4, -1]],
    "distances": [[-1, 2, 3, -2], [2, 0, -1, 4], [3, -1, 2, 1], [-2, 4, 1, 0]],
    "assignment_costs": [[-5, 2, 0, 3], [1, -2, 4, 0], [0, 0, -7, 2], [3, 3, 3, -1]],
    "fixed_assignments": {"k": "Q"},
    "forbidden_assignments": [{"facility": "P", "location": "k"}, {"facility": "S", "location": "n"}],
    "facility_demands": {"P": 4, "S": 2},
    "location_capacities": {"k": 1, "m": 3},
}

# The 0.1.x side-constraint fixture.
SIDE = {
    "facilities": ["F1", "F2"],
    "locations": ["L1", "L2"],
    "flows": [[0, 3], [2, 0]],
    "distances": [[0, 4], [5, 0]],
    "assignment_costs": [[7, 1], [2, 8]],
    "fixed_assignments": {"L1": "F2"},
    "forbidden_assignments": [{"facility": "F2", "location": "L2"}],
    "facility_demands": {"F1": 5, "F2": 2},
    "location_capacities": {"L1": 3, "L2": 10},
}

INSTANCES = {"everything": EVERYTHING, "signed": SIGNED, "side": SIDE}


def _random_instance(seed: int, n: int = 6) -> dict:
    rng = random.Random(seed)
    facilities = [f"f{k}" for k in range(n)]
    locations = [f"l{k}" for k in range(n)]
    return {
        "facilities": facilities,
        "locations": locations,
        "flows": [[rng.randint(0, 9) for _ in facilities] for _ in facilities],
        "distances": [[rng.randint(0, 9) for _ in locations] for _ in locations],
        "assignment_costs": [[rng.randint(0, 6) for _ in locations] for _ in facilities],
        "fixed_assignments": {locations[0]: facilities[3], locations[4]: facilities[3]},
        "forbidden_assignments": [
            {"facility": facilities[1], "location": locations[1]},
            {"facility": facilities[2], "location": locations[5]},
            {"facility": facilities[1], "location": locations[1]},
        ],
        "facility_demands": {facility: rng.randint(0, 6) for facility in facilities[:4]},
        "location_capacities": {location: rng.randint(0, 5) for location in locations[2:]},
    }


RANDOM = _random_instance(7)


def _assigned(problem: dict, perm) -> Model:
    """facility perm[i] at location i."""
    model = Model.model_validate(problem)
    model.assignment = dict(zip(model.locations, perm, strict=True))
    return model


def _engine_raw(problem: dict, perm, name: str) -> float:
    return sum(constraint_costs(_assigned(problem, perm).to_core())[name]["costs"])


def _single(name: str):
    """The constraint alone at weight 1, at its default level."""
    spec = next(spec for spec in get_constraints() if spec.name == name)
    return type(spec)(weight=1)


def _default_level(name: str) -> str:
    return next(row.default_level for row in constraint_info() if row.name == name)


class _Pinned:
    """One constraint's CP-SAT model, re-solved with the x literals pinned to a permutation."""

    def __init__(self, problem: dict, name: str) -> None:
        self.problem = problem
        self.built = build(Model.model_validate(problem), [_single(name)])
        self.built.objective.minimize()
        self.level = _default_level(name)

    def raw(self, perm) -> int:
        facilities = self.problem["facilities"]
        n = len(facilities)
        at = [facilities.index(facility) for facility in perm]  # facility index per location
        model = self.built.model
        model.clear_assumptions()
        model.add_assumptions(
            [
                literal if at[location] == facility else literal.Not()
                for (facility, location), literal in self.built.context.assigned.items()
            ]
        )
        assert len(self.built.context.assigned) == n * n
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert solver.solve(model) == cp_model.OPTIMAL
        return -self.built.objective.score(solver)[self.level]


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_pinned_permutations_cost_the_same_in_both_solvers(instance, name):
    problem = INSTANCES[instance]
    pinned = _Pinned(problem, name)
    nonzero = 0
    for perm in permutations(problem["facilities"]):
        engine = _engine_raw(problem, perm, name)
        cpsat = pinned.raw(perm)
        assert cpsat == engine, f"{name} {perm}: CP-SAT {cpsat} != engine {engine}"
        nonzero += engine != 0
    if instance != "side":
        assert nonzero > 0, f"{name} is never non-zero on {instance}: the parity check proves nothing"


@pytest.mark.parametrize("name", list_constraints())
def test_random_permutations_cost_the_same_in_both_solvers(name):
    pinned = _Pinned(RANDOM, name)
    rng = random.Random(f"parity:{name}")
    nonzero = 0
    for _ in range(100):
        perm = list(RANDOM["facilities"])
        rng.shuffle(perm)
        engine = _engine_raw(RANDOM, perm, name)
        assert pinned.raw(perm) == engine, f"{name} {perm}"
        nonzero += engine != 0
    assert nonzero > 0


def test_raw_costs_match_by_hand():
    """A B C D E at v w x y z on EVERYTHING, every term by hand, in both solvers."""
    perm = ["A", "B", "C", "D", "E"]
    expected = {
        "pinned_assignments_cost": 3,  # z wants C (has E), v wants C (has A), x wants A (has C)
        "forbidden_assignments_cost": 2,  # B at w (listed twice: once), D at y
        "location_capacity_cost": 3,  # v: A's 3 on 0; w: B's 0 on 9; y: D (no demand) on 2
        "assignment_cost": 3 + 0 + 1 + 2 + 1,  # the diagonal of the cost matrix
        "flow_distance_cost": sum(
            EVERYTHING["flows"][a][b] * EVERYTHING["distances"][a][b] for a in range(5) for b in range(5)
        ),
    }
    for name, value in expected.items():
        assert _engine_raw(EVERYTHING, perm, name) == value, name
        assert _Pinned(EVERYTHING, name).raw(perm) == value, name


def test_repeated_forbidden_rows_count_once_in_both_solvers():
    """The 0.1.x CP-SAT model summed over rows (twice for a repeated one); 0.2.0 counts a pair once."""
    problem = {**SIDE, "forbidden_assignments": [{"facility": "F2", "location": "L2"}] * 3}
    assert _engine_raw(problem, ["F1", "F2"], "forbidden_assignments_cost") == 1
    assert _Pinned(problem, "forbidden_assignments_cost").raw(["F1", "F2"]) == 1


def _brute_force(problem: dict, selection) -> tuple:
    best = None
    for perm in permutations(problem["facilities"]):
        value = tuple(score(constraint_costs(_assigned(problem, perm).to_core()), selection))
        best = value if best is None or value > best else best
    return best


@pytest.mark.parametrize("instance", list(INSTANCES))
@pytest.mark.parametrize("name", list_constraints())
def test_each_constraint_matches_brute_force(instance, name):
    problem = INSTANCES[instance]
    selection = [_single(name)]
    solved = solve(Model.model_validate(problem), time_limit_seconds=10, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(problem, selection))


@pytest.mark.parametrize("instance", [*INSTANCES, "random"])
def test_the_default_set_matches_brute_force_in_both_solvers(instance):
    problem = RANDOM if instance == "random" else INSTANCES[instance]
    optimum = _brute_force(problem, get_constraints())
    solved = solve(Model.model_validate(problem), time_limit_seconds=20)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(optimum)
    engine = Model.model_validate(problem)
    result = engine_solve(engine, time_limit_seconds=1.0, random_seed=1)
    assert (result.hard, result.soft) == pytest.approx(optimum)


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
        assert entry["kind"] == "either"  # every quadratic-assignment constraint is flippable
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0, "offset": 0.0, "clip": True}


def test_demoting_a_hard_constraint_scores_like_the_engine():
    """level="soft" moves the term to soft in both solvers; the CP-SAT result rescores identically."""
    selection = [
        LocationCapacityCost(level="soft") if spec.name == "location_capacity_cost" else spec
        for spec in get_constraints()
    ]
    solved = solve(Model.model_validate(EVERYTHING), time_limit_seconds=20, constraints=selection)
    engine = score(constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(EVERYTHING, selection))


def test_signed_objectives_keep_the_levels_lexicographic():
    """Negative entries: the soft bound covers the whole range, so hard still dominates soft."""
    selection = [PinnedAssignmentsCost(), AssignmentCost(weight=5), FlowDistanceCost(weight=3)]
    solved = solve(Model.model_validate(SIGNED), time_limit_seconds=20, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(SIGNED, selection))
    promoted = [PinnedAssignmentsCost(), AssignmentCost(level="hard"), FlowDistanceCost()]
    solved = solve(Model.model_validate(SIGNED), time_limit_seconds=20, constraints=promoted)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(SIGNED, promoted))


def test_exponents_and_fractional_weights_stay_unsupported():
    with pytest.raises(ValueError, match="exponent is not supported"):
        solve(Model.model_validate(SIDE), time_limit_seconds=1, constraints=[FlowDistanceCost(exponent=2)])
    with pytest.raises(ValueError, match="must be an integer"):
        solve(Model.model_validate(SIDE), time_limit_seconds=1, constraints=[FlowDistanceCost(weight=0.5)])


def test_selections_by_name_and_mapping_work():
    solved = solve(
        Model.model_validate(SIDE),
        time_limit_seconds=5,
        constraints=["flow_distance_cost", {"name": "assignment_cost", "weight": 1}],
    )
    assert (solved.score_hard, solved.score_soft) == (0, -26.0)  # L1<-F2, L2<-F1: 3 + 23
    with pytest.raises(ValueError, match="unknown constraint"):
        solve(Model.model_validate(SIDE), time_limit_seconds=1, constraints=["minimize_flow_distance"])


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

SELECTIONS = {
    "repeat": [
        PinnedAssignmentsCost(),
        PinnedAssignmentsCost(weight=2),
        FlowDistanceCost(),
        FlowDistanceCost(weight=3),
    ],
    # The literal formula with an offset is refused on hard: the hard picks are
    # thresholds, the literal copies of the hard side constraints sit on soft.
    "repeat-literal": [
        ForbiddenAssignmentsCost(level="soft", offset=1, clip=False),
        ForbiddenAssignmentsCost(weight=2, offset=-1),
        LocationCapacityCost(),
        AssignmentCost(offset=5, clip=False),
        FlowDistanceCost(),
    ],
    "hard-offsets": [
        PinnedAssignmentsCost(offset=1),
        PinnedAssignmentsCost(level="soft", offset=1, clip=False),
        LocationCapacityCost(offset=2),
        LocationCapacityCost(level="soft", weight=2, offset=3, clip=False),
        ForbiddenAssignmentsCost(offset=-2),
        ForbiddenAssignmentsCost(level="soft", offset=-2, clip=False),
        AssignmentCost(offset=8),
        FlowDistanceCost(offset=60, clip=False),
        FlowDistanceCost(offset=-5),
    ],
    "identical-twice": [LocationCapacityCost(), LocationCapacityCost(), AssignmentCost()],
    # Each pick on its own level: a soft literal repeat of the hard pins, the capacity on soft
    # with a hard threshold repeat, the soft direct cost promoted to hard (threshold) next to
    # a soft threshold repeat, a soft forbidden repeat and the flow on both levels.
    "levels": [
        PinnedAssignmentsCost(),
        PinnedAssignmentsCost(level="soft", weight=3, offset=1, clip=False),
        LocationCapacityCost(level=Level.SOFT, weight=2),
        LocationCapacityCost(level="hard", offset=1),
        AssignmentCost(level="hard", weight=2, offset=8),
        AssignmentCost(level=Level.SOFT, offset=3),
        ForbiddenAssignmentsCost(level="soft", offset=-1),
        FlowDistanceCost(),
        FlowDistanceCost(level=Level.HARD, offset=60),
    ],
}


def _engine_score(core, selection) -> dict:
    """The engine's own score of the core's assignment: a zero-budget warm start keeps it."""
    config = SolverConfig()
    config.spent_limit_seconds = 0
    core.warm_start = True
    kept = core.assignment()
    Solver(config).solve_with_constraints(core, selection)
    assert core.assignment() == kept
    return {"hard": core.score.hard, "soft": core.score.soft}


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_permutation(selection):
    """Pin each of the 120 permutations of EVERYTHING: the CP-SAT objective's per-level value is
    the engine's score of that permutation under the same repeated / shifted picks (its own and
    the score() mirror's), exactly. The
    side constraints are scoped (not every location is fixed, forbidden or capacitated), so a
    literal offset matches only when CP-SAT adds terms for exactly the engine's locations."""
    built = build(Model.model_validate(EVERYTHING), selection)
    built.objective.minimize()
    facilities = EVERYTHING["facilities"]
    for perm in permutations(facilities):
        at = [facilities.index(facility) for facility in perm]
        model = built.model
        model.clear_assumptions()
        model.add_assumptions(
            [
                literal if at[location] == facility else literal.Not()
                for (facility, location), literal in built.context.assigned.items()
            ]
        )
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = 1
        assert solver.solve(model) == cp_model.OPTIMAL
        core = _assigned(EVERYTHING, perm).to_core()
        engine = score(constraint_costs(core, selection), selection)
        assert _engine_score(core, selection) == {"hard": engine.hard, "soft": engine.soft}, perm
        assert built.objective.score(solver) == {"hard": engine.hard, "soft": engine.soft}, perm
        # Re-pricing the default costs (each name once) gives the same per instance key.
        assert built.objective.score_costs(constraint_costs(core)) == {"hard": engine.hard, "soft": engine.soft}


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection):
    solved = solve(Model.model_validate(EVERYTHING), time_limit_seconds=20, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(EVERYTHING, selection))
    engine = score(constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_mapping_selections_repeat_and_shift_like_specs():
    """The server's {name, weight, level, offset, clip} dicts pick the same instances as specs."""
    specs = [FlowDistanceCost(), FlowDistanceCost(weight=3, offset=20, clip=False)]
    dicts = [{"name": "flow_distance_cost"}, {"name": "flow_distance_cost", "weight": 3, "offset": 20, "clip": False}]
    by_specs = solve(Model.model_validate(SIDE), time_limit_seconds=5, constraints=specs)
    assert solve(Model.model_validate(SIDE), time_limit_seconds=5, constraints=dicts) == by_specs
    assert (by_specs.score_hard, by_specs.score_soft) == (0, -(22 + 3 * (22 - 20)))  # the identity: 22 < 23
    # With a level per pick: the direct cost beyond 10 on hard rules the identity out (15 vs 3),
    # and the soft literal pin earns 2 x (0 - 1) on L1 <- F2: -(23 + 3 x 3) + 2.
    specs = [
        *specs,
        AssignmentCost(level="hard", offset=10),
        PinnedAssignmentsCost(level="soft", weight=2, offset=1, clip=False),
    ]
    dicts = [
        *dicts,
        {"name": "assignment_cost", "level": "hard", "offset": 10},
        {"name": "pinned_assignments_cost", "weight": 2, "level": "soft", "offset": 1, "clip": False},
    ]
    by_specs = solve(Model.model_validate(SIDE), time_limit_seconds=5, constraints=specs)
    assert solve(Model.model_validate(SIDE), time_limit_seconds=5, constraints=dicts) == by_specs
    assert (by_specs.score_hard, by_specs.score_soft) == (0, -(23 + 3 * (23 - 20)) + 2)


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_like_the_engine(level):
    """The domain has no medium level: CP-SAT refuses the pick, a repeat too, in the engine's words."""
    message = "constraint 'location_capacity_cost': this use case has no medium level"
    for picks in (
        [LocationCapacityCost(level=level)],
        [LocationCapacityCost(), LocationCapacityCost(level=level)],
        [{"name": "location_capacity_cost"}, {"name": "location_capacity_cost", "level": "medium"}],
    ):
        with pytest.raises(ValueError, match=message):
            solve(Model.model_validate(SIDE), time_limit_seconds=1, constraints=picks)


def test_a_fractional_offset_is_refused_by_cpsat():
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(Model.model_validate(SIDE), time_limit_seconds=1, constraints=[FlowDistanceCost(offset=0.5)])


@pytest.mark.parametrize(
    ("pick", "data"),
    [(FlowDistanceCost(offset=1), "flows and distances"), (AssignmentCost(offset=-1, clip=False), "assignment costs")],
)
def test_an_offset_on_a_total_that_may_be_negative_is_refused(pick, data):
    """ObjectiveBuilder.add shapes a non-negative range: with negative flows, distances or direct
    costs (SIGNED) an objective's offset is refused instead of shaped wrongly; offset 0 still works."""
    with pytest.raises(ValueError, match=f"offset needs non-negative {data} for CP-SAT"):
        solve(Model.model_validate(SIGNED), time_limit_seconds=1, constraints=[PinnedAssignmentsCost(), pick])
    plain = [PinnedAssignmentsCost(), type(pick)()]
    solved = solve(Model.model_validate(SIGNED), time_limit_seconds=10, constraints=plain)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(SIGNED, plain))


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call) -> str:
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [2, -2])
@pytest.mark.parametrize(
    "selection",
    [
        lambda offset: LocationCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: {"name": "location_capacity_cost", "offset": offset, "clip": False},
        lambda offset: FlowDistanceCost(level=Level.HARD, offset=offset, clip=False),
        lambda offset: {"name": "flow_distance_cost", "level": "hard", "offset": offset, "clip": False},
    ],
    ids=["capacity-spec", "capacity-mapping", "flow-moved-to-hard", "flow-mapping"],
)
def test_the_literal_offset_is_refused_on_hard_like_the_engine(selection, offset):
    """solve() and build() refuse the pick with the engine's and score()'s message, alone and
    as a repeat next to the defaults, before any term is added (and before the objectives'
    non-negative-data check)."""
    pick = selection(offset)
    name = pick["name"] if isinstance(pick, dict) else pick.name
    message = HARD_REFUSAL.format(q=repr(name))
    for problem in (SIDE, SIGNED):
        for picks in ([pick], [*get_constraints(), pick]):
            with pytest.raises(ValueError) as refused:
                solve(Model.model_validate(problem), time_limit_seconds=1, constraints=picks)
            assert str(refused.value) == message
            with pytest.raises(ValueError) as refused:
                build(Model.model_validate(problem), picks)
            assert str(refused.value) == message
    if not isinstance(pick, dict):
        # The engine and the score() mirror say the same.
        core = _assigned(SIDE, ["F1", "F2"]).to_core()
        assert _refusal(lambda: _engine_score(core, [pick])) == message
        assert _refusal(lambda: constraint_costs(core, [pick])) == message
        assert _refusal(lambda: score(constraint_costs(core), [pick])) == message


@pytest.mark.parametrize("offset", [2, -2])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """The soft literal capacity, charged on the capacitated locations only, next to the hard
    defaults reaches the brute-force optimum."""
    selection = [*get_constraints(), LocationCapacityCost(level="soft", offset=offset, clip=False)]
    solved = solve(Model.model_validate(EVERYTHING), time_limit_seconds=20, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(_brute_force(EVERYTHING, selection))
    engine = score(constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
