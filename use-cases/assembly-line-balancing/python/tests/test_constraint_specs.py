"""assembly_line_balancing.constraints — an API for picking the cost function declaratively."""

import pickle
import re
from types import SimpleNamespace

import pytest

import assembly_line_balancing as alb
from assembly_line_balancing.constraints import (
    Constraint,
    DifferentStationPairsCost,
    FixedTaskStationsCost,
    ForbiddenTaskStationsCost,
    MaxStationsCost,
    SameStationGroupsCost,
    StationCapacityCost,
    TaskPrecedenceCost,
    TaskStationBoundsCost,
    UsedStationsCost,
    WorkloadImbalanceCost,
    different_station_pairs_cost,
    fixed_task_stations_cost,
    forbidden_task_stations_cost,
    get_constraints,
    max_stations_cost,
    same_station_groups_cost,
    score,
    station_capacity_cost,
    task_precedence_cost,
    task_station_bounds_cost,
    used_stations_cost,
    workload_imbalance_cost,
)
from assembly_line_balancing.models import (
    AssemblyLineBalancingProblemModel,
    PrecedenceModel,
    TaskGroupModel,
    TaskModel,
    TaskPairModel,
)

NAMES = [
    "station_capacity_cost",
    "task_precedence_cost",
    "max_stations_cost",
    "task_station_bounds_cost",
    "fixed_task_stations_cost",
    "forbidden_task_stations_cost",
    "same_station_groups_cost",
    "different_station_pairs_cost",
    "used_stations_cost",
    "workload_imbalance_cost",
]
INSTANCES = (
    station_capacity_cost,
    task_precedence_cost,
    max_stations_cost,
    task_station_bounds_cost,
    fixed_task_stations_cost,
    forbidden_task_stations_cost,
    same_station_groups_cost,
    different_station_pairs_cost,
    used_stations_cost,
    workload_imbalance_cost,
)
GLOBALS = {"task_precedence_cost", "same_station_groups_cost", "used_stations_cost", "workload_imbalance_cost"}


def build_model() -> AssemblyLineBalancingProblemModel:
    """Six tasks of 4 on cycle 8, a chain, a zone {T0, T1} and a separation (T4, T5): 4 stations at best."""
    return AssemblyLineBalancingProblemModel(
        cycle_time=8,
        tasks=[TaskModel(id=f"T{i}", duration=4) for i in range(6)],
        precedences=[PrecedenceModel(before=f"T{i}", after=f"T{i + 1}") for i in range(5)],
        same_station_groups=[TaskGroupModel(id="zone", task_ids=["T0", "T1"])],
        different_station_pairs=[TaskPairModel(first="T4", second="T5")],
    )


def build_problem():
    return build_model().to_core()


def fixture_h():
    """The hand-calculated fixture of the C++ tests (every constraint positive): -10hard/-3soft."""
    return AssemblyLineBalancingProblemModel(
        cycle_time=5,
        max_stations=3,
        tasks=[
            TaskModel(id="A", duration=3, station=1, fixed_station=0, forbidden_stations=[1, 1]),
            TaskModel(id="B", duration=3, station=0, min_station=1),
            TaskModel(id="C", duration=2, station=1, fixed_station=1),
            TaskModel(id="D", duration=6, station=3, max_station=2, forbidden_stations=[0]),
        ],
        precedences=[PrecedenceModel(before="A", after="B"), PrecedenceModel(before="C", after="D")],
        same_station_groups=[
            TaskGroupModel(id="g1", task_ids=["A", "C", "C"]),
            TaskGroupModel(id="g2", task_ids=["B", "A", "C"]),
        ],
        different_station_pairs=[
            TaskPairModel(first="A", second="C"),
            TaskPairModel(first="B", second="D"),
            TaskPairModel(first="B", second="B"),
        ],
    ).to_core()


def _config(steps=400, seed=1):
    config = alb.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = steps
    config.random_seed = seed
    return config


def solve(constraints=None):
    problem = build_problem()
    alb.solve(problem, config=_config(), constraints=constraints)
    return problem.score


def _solved(constraints=None):
    problem = build_problem()
    alb.solve(problem, config=_config(), constraints=constraints)
    return problem


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows, in the 0.1.5 catalog order."""
    assert alb.list_constraints() == NAMES
    for spec in get_constraints():
        assert spec.name in alb.list_constraints()


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == alb.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert (plain.hard, plain.soft) == (explicit.hard, explicit.soft) == (0, -4.0)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    hard_only = solve([station_capacity_cost, task_precedence_cost])
    assert (hard_only.hard, hard_only.soft) == (0, 0.0)


def test_reweighting_one_constraint_scales_only_that_term():
    base = solve([station_capacity_cost, used_stations_cost])
    heavier = solve([station_capacity_cost, UsedStationsCost(weight=10)])
    assert heavier.hard == base.hard
    assert heavier.soft == 10 * base.soft


def test_the_opt_in_workload_objective_balances_the_line():
    """workload_imbalance_cost is off by default; weighted, it joins the soft score."""
    problem = _solved([*get_constraints()[:-1], WorkloadImbalanceCost(weight=1)])
    raw = alb.constraint_costs(problem)
    assert problem.score.soft == -(raw["used_stations_cost"]["costs"][0] + raw["workload_imbalance_cost"]["costs"][0])


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft empties the hard level."""
    problem = build_model()
    for task in problem.tasks:
        task.station = 0
    core = problem.to_core()
    assert alb.calculate_score(core).hard < 0
    selection = [StationCapacityCost(hard=False), TaskPrecedenceCost(hard=False)]
    raw = alb.constraint_costs(core)
    assert score(raw, selection).hard == 0
    assert score(raw, selection).soft == -(16)  # 24 - 8 on station 0
    promoted = score(raw, [UsedStationsCost(hard=True)])
    assert (promoted.hard, promoted.soft) == (-1, 0.0)


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "station_capacity_costs"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    with pytest.raises(ValueError, match="must not be empty"):
        alb.solve(build_problem(), config=_config(), constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    for spec in INSTANCES:
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in INSTANCES] == NAMES
    assert get_constraints() == list(INSTANCES)


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"
        assert "Default:" in cls.__doc__


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = alb.constraint_info()
    assert [row.name for row in rows] == alb.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [("hard", 1.0, True, False, True)] * 8 + [
        ("soft", 1.0, True, False, True),
        ("soft", 0.0, True, False, False),  # the opt-in objective
    ]


def test_constraint_info_rows_are_plain_values():
    row = alb.constraint_info()[0]
    assert isinstance(row, alb.ConstraintInfo)
    assert row._asdict() == {
        "name": "station_capacity_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='station_capacity_cost'")
    assert pickle.loads(pickle.dumps(alb.constraint_info())) == alb.constraint_info()
    assert alb.ConstraintInfo.__module__ == alb._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint, in catalog order."""
    raw = alb.constraint_costs(build_problem())
    assert list(raw) == alb.list_constraints()
    for row in alb.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name in GLOBALS)
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert "medium_fold" not in entry  # no medium level in this domain


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights."""
    explicit = [type(spec)(weight=0 if spec.name == "workload_imbalance_cost" else 1) for spec in get_constraints()]
    problem = fixture_h()
    raw = alb.constraint_costs(problem)
    engine = alb.calculate_score(problem)
    assert score(raw) == score(raw, explicit) == (engine.hard, engine.soft) == (-10, -3.0)


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
        UsedStationsCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    for knobs, message in (
        ({"weight": -2.0}, "weight must be finite and >= 0"),
        ({"exponent": 0.0}, "exponent must be finite and > 0"),
    ):
        pick = SimpleNamespace(name="used_stations_cost", weight=None, hard=None, exponent=1.0)
        for key, value in knobs.items():
            setattr(pick, key, value)
        with pytest.raises(ValueError, match=message):
            alb.solve(build_problem(), config=_config(), constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="used_stations_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'used_stations_cost': {re.escape(message)}"):
        alb.solve(build_problem(), config=_config(), constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        UsedStationsCost(**knobs)


@pytest.mark.parametrize(
    "old",
    [
        "station_capacity",
        "task_precedence",
        "station_count_limit",
        "task_station_bounds",
        "fixed_task_stations",
        "forbidden_task_stations",
        "same_station_groups",
        "different_station_pairs",
        "minimize_stations",
        "workload_balance",
    ],
)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        alb.solve(build_problem(), config=_config(), constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(alb.constraint_costs(build_problem()), [pick])


def test_constraint_costs_match_every_constraint_by_hand():
    """Fixture H, per station (V = 4: loads 3 / 5 / 0 / 6) and per global total."""
    costs = {name: entry["costs"] for name, entry in alb.constraint_costs(fixture_h()).items()}
    assert costs == {
        "station_capacity_cost": [0, 0, 0, 1],  # station 3: 6 - 5
        "task_precedence_cost": [1],  # A on 1 before B on 0
        "max_stations_cost": [0, 0, 0, 1],  # D on 3 with max_stations 3
        "task_station_bounds_cost": [1, 0, 0, 1],  # B below 1; D above 2
        "fixed_task_stations_cost": [0, 1, 0, 0],  # A on 1, fixed 0
        "forbidden_task_stations_cost": [0, 1, 0, 0],  # A on 1, forbidden twice: once
        "same_station_groups_cost": [1],  # {B, A, C} on {0, 1}
        "different_station_pairs_cost": [1, 1, 0, 0],  # self-pair (B, B) on 0; (A, C) on 1
        "used_stations_cost": [3],
        "workload_imbalance_cost": [3],  # 6 - 3
    }
    assert score(alb.constraint_costs(fixture_h())) == (-10, -3.0)


def test_exponent_shapes_per_station_and_global_totals():
    """0.2.0: per-station constraints shape each station, globals their total."""
    raw = alb.constraint_costs(fixture_h())
    # Pairs: 1 + 1 per station -> 1^2 + 1^2 = 2 (0.1.5 shaped per pair: also 2).
    assert score(raw, [DifferentStationPairsCost(exponent=2.0)]).hard == -2
    # Bounds: B (1) on station 0, D (1) on station 3 -> 2.
    assert score(raw, [TaskStationBoundsCost(exponent=3.0)]).hard == -2
    # A global: used stations 3 -> 3^2 = 9.
    assert score(raw, [UsedStationsCost(exponent=2.0)]).soft == -9.0
    # Same-station groups: the TOTAL is shaped (0.1.5 shaped per group).
    assert score(raw, [SameStationGroupsCost(exponent=2.0)]).hard == -1
    for spec in (MaxStationsCost, FixedTaskStationsCost, ForbiddenTaskStationsCost):
        assert score(raw, [spec(exponent=2.0)]).hard == -1


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def test_score_equals_the_engine_for_the_default_assembly():
    problem = _solved()
    engine = alb.calculate_score(problem)
    python = score(alb.constraint_costs(problem))
    assert (python.hard, python.soft) == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [station_capacity_cost],
        [*get_constraints()[:-1], WorkloadImbalanceCost(weight=3)],
        [StationCapacityCost(hard=False), TaskPrecedenceCost(hard=False), used_stations_cost],
        [StationCapacityCost(exponent=2.0), SameStationGroupsCost(exponent=2.0), UsedStationsCost(hard=True)],
        [DifferentStationPairsCost(weight=0.5), MaxStationsCost(weight=2.5), used_stations_cost],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score."""
    problem = _solved(specs)
    python = score(alb.constraint_costs(problem), specs)
    assert python.hard == problem.score.hard
    assert python.soft == pytest.approx(problem.score.soft)
    selected = alb.calculate_score(problem, specs)
    assert (selected.hard, selected.soft) == (python.hard, python.soft)


def test_score_is_additive_over_the_picked_constraints():
    raw = alb.constraint_costs(fixture_h())
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_naming_a_constraint_twice_is_rejected():
    with pytest.raises(ValueError, match="picked more than once"):
        alb.solve(
            build_problem(),
            config=_config(),
            constraints=[station_capacity_cost, UsedStationsCost(weight=10), used_stations_cost],
        )
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([station_capacity_cost, station_capacity_cost])


def test_unassigned_tasks_contribute_nothing():
    """0.2.0 follows the engine convention: a task at -1 is in no station, arc, pair or group."""
    problem = fixture_h()
    problem.set_assignment([-1, -1, -1, -1])
    raw = alb.constraint_costs(problem)
    assert {name: sum(entry["costs"]) for name, entry in raw.items()} == dict.fromkeys(NAMES, 0)
    score_ = alb.calculate_score(problem)
    assert (score_.hard, score_.soft) == (0, 0.0)
