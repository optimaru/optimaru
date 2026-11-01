"""quadratic_assignment.constraints — an API for picking the cost function declaratively."""

import pickle
import re
from types import SimpleNamespace

import pytest

import quadratic_assignment as qap
from quadratic_assignment.constraints import (
    AssignmentCost,
    Constraint,
    FlowDistanceCost,
    ForbiddenAssignmentsCost,
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
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        PinnedAssignmentsCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="flow_distance_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="flow_distance_cost", weight=None, level=None, exponent=0.0)
    for pick, message in ((negative, "weight must be finite and >= 0"), (flat, "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            qap.solve(three(), time_limit_seconds=0, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
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


def test_empty_and_repeated_picks_are_rejected():
    with pytest.raises(ValueError, match="must not be empty"):
        qap.solve(three(), time_limit_seconds=0, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])
    with pytest.raises(ValueError, match="picked more than once"):
        qap.solve(three(), time_limit_seconds=0, constraints=[flow_distance_cost, FlowDistanceCost(weight=2)])
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([assignment_cost, assignment_cost])


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
