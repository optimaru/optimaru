"""quadratic_assignment.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here.
"""

import json
import pickle
import re
from pathlib import Path
from types import SimpleNamespace

import pytest

import quadratic_assignment as qap
from quadratic_assignment._constraint import instance_keys
from quadratic_assignment.constraints import (
    AssignmentCost,
    Constraint,
    FlowDistanceCost,
    ForbiddenAssignmentsCost,
    Level,
    LocationCapacityCost,
    PinnedAssignmentsCost,
    assignment_cost,
    flow_distance_cost,
    forbidden_assignments_cost,
    get_constraints,
    location_capacity_cost,
    pinned_assignments_cost,
    score,
)
from quadratic_assignment.models import ForbiddenAssignmentModel, QuadraticAssignmentProblemModel

NAMES = [
    "pinned_assignments_cost",
    "forbidden_assignments_cost",
    "location_capacity_cost",
    "assignment_cost",
    "flow_distance_cost",
]


def side(assignment=None) -> QuadraticAssignmentProblemModel:
    """The 0.1.x side-constraint fixture: L1<-F1, L2<-F2 breaks the pin (1), uses the forbidden
    pair (1), overloads L1 by 2, costs 7 + 8 = 15 directly and 22 in flow-distance."""
    return QuadraticAssignmentProblemModel(
        facilities=["F1", "F2"],
        locations=["L1", "L2"],
        flows=[[0, 3], [2, 0]],
        distances=[[0, 4], [5, 0]],
        assignment_costs=[[7, 1], [2, 8]],
        assignment=assignment or {"L1": "F1", "L2": "F2"},
        fixed_assignments={"L1": "F2"},
        forbidden_assignments=[ForbiddenAssignmentModel(facility="F2", location="L2")],
        facility_demands={"F1": 5, "F2": 2},
        location_capacities={"L1": 3, "L2": 10},
    )


def three() -> QuadraticAssignmentProblemModel:
    return QuadraticAssignmentProblemModel(
        facilities=["F1", "F2", "F3"],
        locations=["L1", "L2", "L3"],
        flows=[[0, 3, 1], [3, 0, 2], [1, 2, 0]],
        distances=[[0, 1, 4], [1, 0, 2], [4, 2, 0]],
        fixed_assignments={"L1": "F3", "L2": "F3"},
        forbidden_assignments=[ForbiddenAssignmentModel(facility="F1", location="L1")],
        facility_demands={"F1": 4, "F2": 1},
        location_capacities={"L1": 2, "L3": 0},
    )


def _zero() -> qap.SolverConfig:
    config = qap.SolverConfig()
    config.spent_limit_seconds = 0
    return config


def solved(constraints=None, problem=None) -> QuadraticAssignmentProblemModel:
    problem = problem if problem is not None else three()
    config = qap.SolverConfig()
    config.step_count_limit = 2000
    config.random_seed = 1
    qap.solve(problem, config=config, constraints=constraints)
    return problem


def engine_score(problem: QuadraticAssignmentProblemModel, constraints) -> tuple[int, float]:
    """The engine's score of the model's assignment as it stands (a warm start, zero budget)."""
    core = problem.to_core()
    core.warm_start = True
    qap.Solver(_zero()).solve_with_constraints(core, constraints)
    return core.score.hard, core.score.soft


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    assert qap.list_constraints() == NAMES
    for spec in get_constraints():
        assert spec.name in NAMES


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == qap.list_constraints()


def test_module_level_instances_keep_every_default():
    """Module-level instances keep the defaults (weight=None, level=None, exponent=1)."""
    instances = (
        pinned_assignments_cost,
        forbidden_assignments_cost,
        location_capacity_cost,
        assignment_cost,
        flow_distance_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert list(instances) == get_constraints()


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = qap.constraint_info()
    assert [row.name for row in rows] == qap.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [("hard", 1.0, True, False, True)] * 3 + [("soft", 1.0, True, False, True)] * 2


def test_constraint_info_rows_are_plain_values():
    row = qap.constraint_info()[0]
    assert isinstance(row, qap.ConstraintInfo)
    assert row._asdict() == {
        "name": "pinned_assignments_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='pinned_assignments_cost'")
    assert pickle.loads(pickle.dumps(qap.constraint_info())) == qap.constraint_info()
    assert qap.ConstraintInfo.__module__ == qap._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults; the side constraints per location, the objectives global."""
    raw = qap.constraint_costs(side())
    assert list(raw) == NAMES
    for row in qap.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (row.level_flippable, row.reward)
        is_global = row.name in ("assignment_cost", "flow_distance_cost")
        assert entry["global"] is is_global and len(entry["costs"]) == (1 if is_global else 2)
        assert (entry["name"], entry["label"], entry["exponent"]) == (row.name, "", 1.0)
        assert "medium_fold" not in entry  # no medium level in this domain


# --- Raw costs by hand ----------------------------------------------------------


def test_constraint_costs_match_the_hand_calculation():
    raw = qap.constraint_costs(side())
    assert {name: entry["costs"] for name, entry in raw.items()} == {
        "pinned_assignments_cost": [1.0, 0.0],
        "forbidden_assignments_cost": [0.0, 1.0],
        "location_capacity_cost": [2.0, 0.0],
        "assignment_cost": [15.0],
        "flow_distance_cost": [22.0],
    }
    assert score(raw) == (-4, -37.0)
    clean = qap.constraint_costs(side({"L1": "F2", "L2": "F1"}))
    assert {name: entry["costs"] for name, entry in clean.items()} == {
        "pinned_assignments_cost": [0.0, 0.0],
        "forbidden_assignments_cost": [0.0, 0.0],
        "location_capacity_cost": [0.0, 0.0],
        "assignment_cost": [3.0],
        "flow_distance_cost": [23.0],
    }
    assert score(clean) == (0, -26.0)


def test_units_shape_per_location_and_globals_their_total():
    """The exponent shapes each location of a side constraint and the total of an objective (as 0.1.x did)."""
    problem = three()
    problem.assignment = {"L1": "F1", "L2": "F2", "L3": "F3"}
    raw = qap.constraint_costs(problem)
    assert raw["location_capacity_cost"]["costs"] == [2.0, 0.0, 0.0]  # F1's 4 on L1's 2
    assert raw["pinned_assignments_cost"]["costs"] == [1.0, 1.0, 0.0]
    assert score(raw, [LocationCapacityCost(exponent=2)]) == (-4, 0.0)
    assert score(raw, [PinnedAssignmentsCost(exponent=3)]) == (-2, 0.0)  # 1^3 + 1^3
    flow = raw["flow_distance_cost"]["costs"][0]
    assert score(raw, [FlowDistanceCost(exponent=2)]) == (0, -(flow**2))
    for picks in (
        [LocationCapacityCost(exponent=2)],
        [PinnedAssignmentsCost(exponent=3)],
        [FlowDistanceCost(exponent=2)],
    ):
        assert engine_score(problem, picks) == score(raw, picks)


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weight 1 everywhere."""
    explicit = [type(spec)(weight=1) for spec in get_constraints()]
    problem = solved()
    raw = qap.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (problem.score_hard, problem.score_soft)


def test_default_solve_equals_explicit_default_constraints():
    plain = solved()
    explicit = solved(get_constraints())
    assert (plain.score_hard, plain.score_soft) == (explicit.score_hard, explicit.score_soft)
    assert plain.assignment == explicit.assignment


def test_picking_a_subset_drops_the_other_terms():
    problem = side()
    assert qap.calculate_score(problem, [pinned_assignments_cost, forbidden_assignments_cost]) == (-2, 0.0)
    assert qap.calculate_score(problem, [flow_distance_cost]) == (0, -22.0)


def test_level_can_be_flipped():
    """Demoting a hard constraint moves its cost to the soft level; promoting a soft one, to hard."""
    problem = side()
    flipped = [
        spec if spec.name != "location_capacity_cost" else LocationCapacityCost(level="soft")
        for spec in get_constraints()
    ]
    assert qap.calculate_score(problem, flipped) == (-2, -39.0)
    assert engine_score(problem, flipped) == (-2, -39.0)
    promoted = [spec if spec.name != "assignment_cost" else AssignmentCost(level="hard") for spec in get_constraints()]
    assert qap.calculate_score(problem, promoted) == (-19, -22.0)
    assert engine_score(problem, promoted) == (-19, -22.0)


def test_reweighting_scales_only_that_term():
    problem = side()
    heavier = [spec if spec.name != "flow_distance_cost" else FlowDistanceCost(weight=3) for spec in get_constraints()]
    assert qap.calculate_score(problem, heavier) == (-4, -81.0)
    assert engine_score(problem, heavier) == (-4, -81.0)


def test_weights_steer_the_solve():
    """A heavy direct cost turns the side instance toward its cheapest direct placement."""
    problem = side()
    problem.fixed_assignments = {}
    problem.forbidden_assignments = []
    problem.location_capacities = {}
    problem.assignment_costs = [[0, 100], [100, 0]]  # identity is direct-cheap, flow-dear (22 vs 23)
    qap.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=[flow_distance_cost])
    assert problem.assignment == {"L1": "F1", "L2": "F2"}  # flow alone prefers 22 over 23
    problem.flows = [[0, 2], [3, 0]]  # now the identity costs 23 in flow, the swap 22
    qap.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=[flow_distance_cost])
    assert problem.assignment == {"L1": "F2", "L2": "F1"}
    qap.solve(problem, time_limit_seconds=0.2, random_seed=1, constraints=[flow_distance_cost, assignment_cost])
    assert problem.assignment == {"L1": "F1", "L2": "F2"}  # the direct cost outweighs one unit of flow


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": float("inf")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"exponent": -1.0}, ValueError),
        ({"level": 1}, TypeError),
        ({"level": "firm"}, ValueError),
        ({"hard": True}, TypeError),  # the removed knob is an unknown keyword
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        PinnedAssignmentsCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="flow_distance_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="flow_distance_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(name="flow_distance_cost", weight=None, level=None, exponent=1.0, offset=float("inf"))
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            qap.solve(three(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            qap.constraint_costs(side(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        FlowDistanceCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="location_capacity_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'location_capacity_cost': {re.escape(message)}"):
        qap.solve(three(), time_limit_seconds=0, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        LocationCapacityCost(**knobs)


def test_unknown_and_old_names_are_refused():
    """0.2.0 renamed the catalog; the 0.1.x names are unknown now (no aliases)."""
    old_names = (
        "fixed_assignments",
        "forbidden_assignments",
        "location_capacities",
        "minimize_assignment_cost",
        "minimize_flow_distance",
    )
    raw = qap.constraint_costs(side())
    for old in (*old_names, "fixed_assignments_cost", "flow_distance"):
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            qap.solve(three(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])


def test_an_empty_pick_list_is_rejected():
    with pytest.raises(ValueError, match="must not be empty"):
        qap.solve(three(), time_limit_seconds=0, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        qap.constraint_costs(side(), [])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [FlowDistanceCost(), FlowDistanceCost(weight=2), LocationCapacityCost()]
    assert engine_score(side(), picks) == (-2, -(22 + 2 * 22))
    assert qap.calculate_score(side(), picks) == (-2, -66.0)
    assert get_constraints([AssignmentCost(), AssignmentCost()]) == [AssignmentCost(), AssignmentCost()]


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [flow_distance_cost],
        [PinnedAssignmentsCost(weight=3), LocationCapacityCost(level="soft"), flow_distance_cost],
        [LocationCapacityCost(exponent=2.0), ForbiddenAssignmentsCost(weight=0.5), FlowDistanceCost(weight=2)],
        [PinnedAssignmentsCost(level="soft", exponent=0.5), FlowDistanceCost(exponent=1.5)],
        [PinnedAssignmentsCost(weight=0.5), ForbiddenAssignmentsCost(weight=0.5), AssignmentCost(level="hard")],
        [*get_constraints(), LocationCapacityCost(weight=0.5, exponent=2.0), FlowDistanceCost(weight=0.5)],
        [
            LocationCapacityCost(offset=1.0),
            LocationCapacityCost(offset=-2.0, clip=False),
            FlowDistanceCost(offset=3),
        ],
        [
            PinnedAssignmentsCost(offset=0.5, clip=False, weight=3),
            ForbiddenAssignmentsCost(offset=1.0, clip=False),
            AssignmentCost(offset=-2.0, exponent=2.0),
            FlowDistanceCost(offset=40.0, clip=False),
            FlowDistanceCost(weight=0.5, offset=2.0, exponent=1.5),
        ],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs equals the engine score."""
    problem = three()
    problem.assignment_costs = [[1, -4, 2], [0, 3, -1], [5, 2, 0]]
    core = problem.to_core()
    config = qap.SolverConfig()
    config.step_count_limit = 500
    config.random_seed = 2
    qap.Solver(config).solve_with_constraints(core, specs)
    python = score(qap.constraint_costs(core), specs)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)


def test_fractional_hard_weights_round_per_unit():
    """Each location rounds on its own (half away from zero): two broken pins at 0.5 -> -2, not -1."""
    problem = three()
    problem.assignment = {"L1": "F1", "L2": "F2", "L3": "F3"}  # both pins on F3 broken
    raw = qap.constraint_costs(problem)
    picks = [PinnedAssignmentsCost(weight=0.5)]
    assert score(raw, picks) == (-2, 0.0)
    assert engine_score(problem, picks) == (-2, 0.0)


def test_score_is_additive_over_the_picked_constraints():
    raw = qap.constraint_costs(solved())
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_equals_the_engine_for_the_default_assembly():
    problem = solved()
    engine = qap.calculate_score(problem.to_core())
    python = score(qap.constraint_costs(problem))
    assert (python.hard, python.soft) == (engine.hard, engine.soft)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def scoped() -> QuadraticAssignmentProblemModel:
    """Three locations, one of each scope at the identity, every side constraint 0: L1 pinned
    to F1, (F3, L2) forbidden, only L2 capacitated."""
    return QuadraticAssignmentProblemModel(
        facilities=["F1", "F2", "F3"],
        locations=["L1", "L2", "L3"],
        flows=[[0, 3, 1], [3, 0, 2], [1, 2, 0]],
        distances=[[0, 1, 4], [1, 0, 2], [4, 2, 0]],
        assignment={"L1": "F1", "L2": "F2", "L3": "F3"},
        fixed_assignments={"L1": "F1"},
        forbidden_assignments=[ForbiddenAssignmentModel(facility="F3", location="L2")],
        location_capacities={"L2": 10},
    )


def engine_solve(problem: QuadraticAssignmentProblemModel, constraints) -> qap.QuadraticAssignmentProblem:
    """The engine's zero-budget warm-start solve of the model's assignment (it keeps the assignment)."""
    core = problem.to_core()
    core.warm_start = True
    qap.Solver(_zero()).solve_with_constraints(core, constraints)
    return core


REPEAT = [FlowDistanceCost(), FlowDistanceCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """flow 22 plus 0.5 x flow^2: 22 + 0.5 x 484 = 264 soft."""
    problem = side()
    raw = qap.constraint_costs(problem, REPEAT)
    assert list(raw) == ["flow_distance_cost", "flow_distance_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [22.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "flow_distance_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -(22 + 0.5 * 22**2))
    assert expected == (0, -264.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(qap.constraint_costs(problem), REPEAT) == expected
    assert qap.calculate_score(problem, REPEAT) == expected
    engine = engine_solve(problem, REPEAT)
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


def test_identical_picks_count_twice():
    picks = [LocationCapacityCost(), LocationCapacityCost()]
    assert score(qap.constraint_costs(side()), picks) == (-4, 0.0)
    assert engine_score(side(), picks) == (-4, 0.0)
    assert list(qap.constraint_costs(side(), picks)) == ["location_capacity_cost", "location_capacity_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        AssignmentCost(weight=1),
        FlowDistanceCost(),
        AssignmentCost(weight=2),
        PinnedAssignmentsCost(),
        AssignmentCost(weight=3),
    ]
    raw = qap.constraint_costs(side(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "pinned_assignments_cost": 1.0,
        "assignment_cost": 1.0,
        "assignment_cost#2": 2.0,
        "assignment_cost#3": 3.0,
        "flow_distance_cost": 1.0,
    }
    assert score(raw, picks) == (-1, -(15 * 6 + 22))
    assert engine_score(side(), picks) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Side constraints, shaped per evaluated location: pinned [1, 0] (L1), forbidden
        # [0, 1] (L2), capacity [2, 0] (L1 and L2).
        (LocationCapacityCost(offset=1), (-1, 0.0)),
        (LocationCapacityCost(offset=1, clip=False), (0, 0.0)),  # (2 - 1) + (0 - 1): a raw 0 is charged
        (LocationCapacityCost(offset=2.5, clip=False), (4, 0.0)),  # llround(-0.5) + llround(-2.5): per location
        (PinnedAssignmentsCost(offset=1, clip=False), (0, 0.0)),  # L1 only: 1 - 1; L2 is outside the scope
        (PinnedAssignmentsCost(offset=-1), (-2, 0.0)),
        (PinnedAssignmentsCost(offset=-1, clip=False), (-2, 0.0)),  # still L1 only
        (ForbiddenAssignmentsCost(offset=3, clip=False), (2, 0.0)),  # L2 only: 1 - 3
        (ForbiddenAssignmentsCost(level="soft", offset=0.5), (0, -0.5)),
        # The objectives shape their single total: flow 22, assignment 15.
        (FlowDistanceCost(offset=20), (0, -2.0)),
        (FlowDistanceCost(offset=30, clip=False), (0, 8.0)),  # 22 - 30 below the offset: a bonus
        (FlowDistanceCost(offset=10, exponent=2), (0, -144.0)),
        (AssignmentCost(offset=-5), (0, -20.0)),
        (AssignmentCost(offset=20), (0, 0.0)),
        (AssignmentCost(weight=0, offset=20, clip=False), (0, 0.0)),  # weight 0 contributes 0
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    problem = side()
    assert score(qap.constraint_costs(problem), [pick]) == expected
    raw = qap.constraint_costs(problem, [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert qap.calculate_score(problem, [pick]) == expected
    engine = engine_solve(problem, [pick])
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


def test_the_scoped_side_constraints_report_their_units():
    """constraint_costs() carries the evaluated locations of the three side constraints."""
    assert {key: entry.get("units") for key, entry in qap.constraint_costs(side()).items()} == {
        "pinned_assignments_cost": [0],
        "forbidden_assignments_cost": [1],
        "location_capacity_cost": [0, 1],
        "assignment_cost": None,
        "flow_distance_cost": None,
    }
    problem = three()
    problem.assignment = {"L1": "F1", "L2": "F2", "L3": "F3"}
    assert {key: entry.get("units") for key, entry in qap.constraint_costs(problem).items()} == {
        "pinned_assignments_cost": [0, 1],
        "forbidden_assignments_cost": [0],
        "location_capacity_cost": [0, 2],
        "assignment_cost": None,
        "flow_distance_cost": None,
    }


@pytest.mark.parametrize("spec", [PinnedAssignmentsCost, ForbiddenAssignmentsCost, LocationCapacityCost])
def test_a_literal_offset_charges_only_the_evaluated_locations(spec):
    """On an all-zero solution a literal offset scores (#evaluated locations) x -offset on both
    paths: one location each here, so +2 hard, not 3 x 2."""
    problem = scoped()
    raw = qap.constraint_costs(problem)
    assert raw[spec.name]["costs"] == [0.0, 0.0, 0.0]
    assert len(raw[spec.name]["units"]) == 1
    pick = spec(offset=2, clip=False)
    assert score(raw, [pick]) == (2, 0.0)
    assert score(qap.constraint_costs(problem, [pick]), [pick]) == (2, 0.0)
    assert engine_score(problem, [pick]) == (2, 0.0)
    assert engine_score(problem, [spec(offset=2)]) == (0, 0.0)


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain_picks = [LocationCapacityCost(exponent=2), PinnedAssignmentsCost(), FlowDistanceCost(exponent=1.5)]
    shifted_by_zero = [
        LocationCapacityCost(exponent=2, offset=0.0, clip=False),
        PinnedAssignmentsCost(offset=0),
        FlowDistanceCost(exponent=1.5, offset=0.0, clip=False),
    ]
    problem = solved()
    raw = qap.constraint_costs(problem)
    assert score(raw, shifted_by_zero) == score(raw, plain_picks)
    assert engine_score(problem, plain_picks) == engine_score(problem, shifted_by_zero)
    assert engine_score(problem, plain_picks) == tuple(score(raw, plain_picks))
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT],
        [
            PinnedAssignmentsCost(),
            PinnedAssignmentsCost(offset=1, clip=False, weight=2),
            ForbiddenAssignmentsCost(offset=-1),
            LocationCapacityCost(),
            LocationCapacityCost(offset=1, exponent=2, weight=0.5, level="soft"),
            AssignmentCost(offset=-10, clip=False),
            FlowDistanceCost(offset=20, clip=False),
            FlowDistanceCost(weight=0.1, exponent=2, offset=5),
        ],
        [
            *get_constraints(),
            PinnedAssignmentsCost(level="soft", weight=1.5, offset=1, clip=False),
            LocationCapacityCost(level=Level.SOFT, weight=2, offset=-1),
            AssignmentCost(level="hard", weight=0.5, offset=10),
            FlowDistanceCost(level=Level.HARD, weight=0.25, offset=100, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets", "repeats-on-other-levels"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    data = json.loads((Path(__file__).parent / "fixtures" / "web_demo.json").read_text())
    core = QuadraticAssignmentProblemModel.model_validate(data).to_core()
    config = qap.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    qap.Solver(config).solve_with_constraints(core, picks)
    raw = qap.constraint_costs(core, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)
    assert score(qap.constraint_costs(core), picks) == python  # the default costs score the repeats too


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([LocationCapacityCost(level="soft")], (0, -2.0)),
        ([PinnedAssignmentsCost(level=Level.SOFT, weight=2)], (0, -2.0)),
        ([ForbiddenAssignmentsCost(level="soft", offset=2, clip=False)], (0, 1.0)),  # L2 only: 1 - 2, on soft
        ([FlowDistanceCost(level=Level.HARD)], (-22, 0.0)),  # the soft objective promoted
        ([AssignmentCost(level="soft")], (0, -15.0)),  # the default level, named
        ([PinnedAssignmentsCost(level=Level.HARD)], (-1, 0.0)),
        # A repeat carries its own level and shaping. Hard: 2 x (22 - 20) on the flow repeat,
        # the capacity [2, 0] and (15 - 10) on the literal assignment cost. Soft: the flow 22,
        # 3 x ((2 - 2) + (0 - 2)) on the literal capacity repeat and the pinned L1 beyond -1
        # (1 + 1): -22 + 6 - 2.
        (
            [
                FlowDistanceCost(),
                FlowDistanceCost(level="hard", weight=2, offset=20),
                LocationCapacityCost(),
                LocationCapacityCost(level=Level.SOFT, weight=3, offset=2, clip=False),
                AssignmentCost(level=Level.HARD, offset=10, clip=False),
                PinnedAssignmentsCost(level="soft", offset=-1),
            ],
            (-11, -18.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    problem = side()
    assert score(qap.constraint_costs(problem), picks) == expected
    raw = qap.constraint_costs(problem, picks)
    for pick, key in zip(picks, instance_keys(picks), strict=True):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
        assert raw[key]["hard"] == (raw[key]["level"] == "hard"), key
    assert score(raw, picks) == expected
    assert qap.calculate_score(problem, picks) == expected
    engine = engine_solve(problem, picks)
    assert engine.assignment() == problem.to_core().assignment()
    assert (engine.score.hard, engine.score.soft) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    for name, spec in (("pinned_assignments_cost", PinnedAssignmentsCost), ("flow_distance_cost", FlowDistanceCost)):
        message = f"constraint '{name}': this use case has no medium level"
        pick = spec(level=level)  # the spec cannot know the domain
        for picks in ([pick], [spec(), pick]):
            with pytest.raises(ValueError, match=message):
                engine_solve(side(), picks)
            with pytest.raises(ValueError, match=message):
                solved(picks)
            with pytest.raises(ValueError, match=message):
                qap.constraint_costs(side(), picks)
            with pytest.raises(ValueError, match=message):
                score(qap.constraint_costs(side()), picks)
            with pytest.raises(ValueError, match=message):
                qap.calculate_score(side(), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        LocationCapacityCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        LocationCapacityCost(level="firm")
    pick = SimpleNamespace(name="location_capacity_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        engine_solve(side(), [LocationCapacityCost(), pick])


def test_the_knob_was_renamed():
    """0.3.0 replaced hard=True / hard=False by level="hard" / level="soft"; hard= is unknown."""
    with pytest.raises(TypeError):
        FlowDistanceCost(hard=True)
    with pytest.raises(TypeError):
        LocationCapacityCost(weight=None, hard=False)
    assert FlowDistanceCost(level="hard") == FlowDistanceCost(level=Level.HARD)
