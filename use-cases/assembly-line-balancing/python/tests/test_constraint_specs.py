"""assembly_line_balancing.constraints — an API for picking the cost function declaratively.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import assembly_line_balancing as alb
from assembly_line_balancing._constraint import instance_keys
from assembly_line_balancing.constraints import (
    Constraint,
    DifferentStationPairsCost,
    FixedTaskStationsCost,
    ForbiddenTaskStationsCost,
    Level,
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
    selection = [StationCapacityCost(level="soft"), TaskPrecedenceCost(level="soft")]
    raw = alb.constraint_costs(core)
    assert score(raw, selection).hard == 0
    assert score(raw, selection).soft == -(16)  # 24 - 8 on station 0
    promoted = score(raw, [UsedStationsCost(level="hard")])
    assert (promoted.hard, promoted.soft) == (-1, 0.0)


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "station_capacity_costs"  # trailing s
        weight = None
        level = None
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
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
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
        ({"offset": float("nan")}, ValueError),
        ({"offset": "1"}, TypeError),
        ({"clip": 0}, TypeError),
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
        ({"offset": float("inf")}, "offset must be finite, not inf"),
    ):
        pick = SimpleNamespace(name="used_stations_cost", weight=None, level=None, exponent=1.0)
        for key, value in knobs.items():
            setattr(pick, key, value)
        with pytest.raises(ValueError, match=message):
            alb.solve(build_problem(), config=_config(), constraints=[pick])


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
    pick = SimpleNamespace(name="used_stations_cost", weight=None, level=None, exponent=1.0)
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
    pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
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
        [StationCapacityCost(level="soft"), TaskPrecedenceCost(level="soft"), used_stations_cost],
        [StationCapacityCost(exponent=2.0), SameStationGroupsCost(exponent=2.0), UsedStationsCost(level="hard")],
        [DifferentStationPairsCost(weight=0.5), MaxStationsCost(weight=2.5), used_stations_cost],
        [*get_constraints(), UsedStationsCost(weight=10, offset=2), StationCapacityCost(exponent=2)],
        [
            # The literal formula with an offset is refused on hard; its copies sit on soft.
            StationCapacityCost(offset=1),
            StationCapacityCost(level="soft", offset=0.5, clip=False, weight=3),
            TaskPrecedenceCost(offset=1),
            TaskPrecedenceCost(level="soft", offset=1, clip=False),
            SameStationGroupsCost(offset=-1),
            DifferentStationPairsCost(level="soft", offset=1, clip=False),
            UsedStationsCost(),
            UsedStationsCost(offset=3, exponent=2, weight=0.5),
            WorkloadImbalanceCost(weight=1, offset=2, clip=False),
        ],
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


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [StationCapacityCost(), UsedStationsCost(weight=10), UsedStationsCost()]
    problem = _zero_budget_solve(fixture_h(), picks)
    assert (problem.score.hard, problem.score.soft) == (-1, -(10 + 1) * 3)
    assert get_constraints([StationCapacityCost(), StationCapacityCost()]) == [
        StationCapacityCost(),
        StationCapacityCost(),
    ]


def test_unassigned_tasks_contribute_nothing():
    """0.2.0 follows the engine convention: a task at -1 is in no station, arc, pair or group."""
    problem = fixture_h()
    problem.set_assignment([-1, -1, -1, -1])
    raw = alb.constraint_costs(problem)
    assert {name: sum(entry["costs"]) for name, entry in raw.items()} == dict.fromkeys(NAMES, 0)
    score_ = alb.calculate_score(problem)
    assert (score_.hard, score_.soft) == (0, 0.0)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _zero_budget_solve(problem, picks):
    """Score a complete assignment under ``picks`` on the engine: a warm start, no step taken."""
    config = alb.SolverConfig()
    config.spent_limit_seconds = 0
    assignment = problem.assignment()
    problem.warm_start = True  # otherwise the solve clears the stations and constructs
    alb.Solver(config).solve_with_constraints(problem, picks)
    assert problem.assignment() == assignment
    return problem


def _engine(picks, problem=None):
    solved = _zero_budget_solve(problem or fixture_h(), picks)
    return (solved.score.hard, solved.score.soft)


REPEAT = [UsedStationsCost(), UsedStationsCost(weight=10, offset=2)]


def test_a_repeated_pick_by_hand():
    """Fixture H uses 3 stations: 3, plus 10 x (3 - 2) for the stations beyond the second."""
    raw = alb.constraint_costs(fixture_h(), REPEAT)
    assert list(raw) == ["used_stations_cost", "used_stations_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [3.0]
    assert (first["weight"], first["offset"], second["weight"], second["offset"]) == (1.0, 0.0, 10.0, 2.0)
    assert first["name"] == second["name"] == "used_stations_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -(3 + 10 * 1))
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(alb.constraint_costs(fixture_h()), REPEAT) == expected
    assert _engine(REPEAT) == expected
    assert alb.calculate_score(fixture_h(), REPEAT) == expected


def test_a_repeated_unit_pick_by_hand():
    """Station bounds [1, 0, 0, 1] plus 2 x their square per station: -(2 + 2 x 2)."""
    picks = [TaskStationBoundsCost(), TaskStationBoundsCost(weight=2, exponent=2)]
    raw = alb.constraint_costs(fixture_h(), picks)
    assert list(raw) == ["task_station_bounds_cost", "task_station_bounds_cost#2"]
    assert raw["task_station_bounds_cost"]["costs"] == raw["task_station_bounds_cost#2"]["costs"] == [1, 0, 0, 1]
    assert score(raw, picks) == (-6, 0.0)
    assert score(alb.constraint_costs(fixture_h()), picks) == (-6, 0.0)
    assert _engine(picks) == (-6, 0.0)


def test_identical_picks_count_twice():
    picks = [TaskPrecedenceCost(), TaskPrecedenceCost()]
    assert score(alb.constraint_costs(fixture_h()), picks) == (-2, 0.0)
    assert _engine(picks) == (-2, 0.0)
    assert list(alb.constraint_costs(fixture_h(), picks)) == ["task_precedence_cost", "task_precedence_cost#2"]


def test_the_keys_follow_the_catalog_and_the_pick_order_of_each_name():
    """Each name's instances stay together in pick order; the names keep the catalog order."""
    picks = [
        UsedStationsCost(weight=1),
        StationCapacityCost(),
        UsedStationsCost(weight=2),
        TaskPrecedenceCost(),
        UsedStationsCost(weight=3),
    ]
    raw = alb.constraint_costs(fixture_h(), picks)
    assert [(key, entry["weight"]) for key, entry in raw.items()] == [
        ("station_capacity_cost", 1.0),
        ("task_precedence_cost", 1.0),
        ("used_stations_cost", 1.0),
        ("used_stations_cost#2", 2.0),
        ("used_stations_cost#3", 3.0),
    ]
    assert score(raw, picks) == (-2, -(6 * 3))
    assert _engine(picks) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per-station constraints, shaped per station (V = 4); an empty station (raw 0)
        # counts in literal mode. Every hard default refuses the literal formula with an
        # offset, so its numbers are checked on soft, where nothing is rounded.
        (TaskStationBoundsCost(level="soft", offset=1, clip=False), (0, 2.0)),  # 0 + -1 + -1 + 0
        (StationCapacityCost(offset=-1), (-2, 0.0)),  # a negative offset adds to every non-zero station
        (StationCapacityCost(level="soft", offset=-1, clip=False), (0, -5.0)),  # ...or to every station
        (StationCapacityCost(offset=0.5, weight=3), (-2, 0.0)),  # hard rounds per station: 1.5 -> 2, 0 -> 0
        (StationCapacityCost(level="soft", offset=0.5, clip=False, weight=3), (0, 3.0)),  # 1.5 - 3 x 1.5
        # The two scoped constraints charge only the stations they evaluate.
        (MaxStationsCost(level="soft", offset=2, clip=False), (0, 1.0)),  # station 3 only: 1 - 2
        (ForbiddenTaskStationsCost(level="soft", offset=2, clip=False), (0, 3.0)),  # stations 0, 1: (0 - 2) + (1 - 2)
        (ForbiddenTaskStationsCost(offset=-1), (-2, 0.0)),
        # Global constraints shape their one total.
        (TaskPrecedenceCost(level="soft", offset=3, clip=False), (0, 2.0)),  # 1 - 3: a reward, refused on hard
        (SameStationGroupsCost(offset=1), (0, 0.0)),
        (UsedStationsCost(offset=1, exponent=2), (0, -4.0)),
        (UsedStationsCost(offset=5, exponent=3, clip=False), (0, 8.0)),
        (WorkloadImbalanceCost(weight=1, offset=1), (0, -2.0)),
        (WorkloadImbalanceCost(offset=5, clip=False), (0, 0.0)),  # weight 0 (the default) contributes nothing
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(alb.constraint_costs(fixture_h()), [pick]) == expected
    raw = alb.constraint_costs(fixture_h(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected


def _all_zero_scoped():
    """Fixture H with A on 0, B and C on 1, D on 2: no task beyond max_stations or on a
    forbidden station, V = 4."""
    problem = fixture_h()
    problem.set_assignment([0, 1, 1, 2])
    return problem


def test_the_scoped_constraints_report_and_charge_their_units_only():
    """max_stations_cost evaluates the stations from max_stations (3) on, i.e. [3];
    forbidden_task_stations_cost the stations some task forbids, [0, 1]. A literal offset on
    an all-zero solution charges -offset on those units, not on all four stations."""
    raw = alb.constraint_costs(_all_zero_scoped())
    assert raw["max_stations_cost"]["units"] == [3]
    assert raw["forbidden_task_stations_cost"]["units"] == [0, 1]
    assert raw["max_stations_cost"]["costs"] == raw["forbidden_task_stations_cost"]["costs"] == [0, 0, 0, 0]
    assert all(
        "units" not in entry for name, entry in raw.items() if "max_stations" not in name and "forbidden" not in name
    )
    for pick, expected in (
        # On soft: on hard (their default) the literal formula with an offset is refused.
        (MaxStationsCost(level="soft", offset=5, clip=False), (0, 5.0)),  # one evaluated station
        (ForbiddenTaskStationsCost(level="soft", offset=5, clip=False), (0, 10.0)),  # two evaluated stations
        (MaxStationsCost(offset=5), (0, 0.0)),  # the threshold leaves a 0 at 0
    ):
        assert score(raw, [pick]) == expected
        picked = alb.constraint_costs(_all_zero_scoped(), [pick])
        assert next(iter(picked.values()))["units"] == raw[pick.name]["units"]
        assert score(picked, [pick]) == expected
        assert _engine([pick], _all_zero_scoped()) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*get_constraints()[:-1], WorkloadImbalanceCost(weight=2, exponent=2)]
    shifted = [type(spec)(offset=0.0, clip=bool(index % 2)) for index, spec in enumerate(get_constraints()[:-1])]
    shifted.append(WorkloadImbalanceCost(weight=2, exponent=2, offset=0, clip=False))
    raw = alb.constraint_costs(fixture_h())
    assert score(raw, shifted) == score(raw, plain)
    assert _engine(plain) == _engine(shifted) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT, StationCapacityCost(weight=2, exponent=2)],
        [
            StationCapacityCost(),
            StationCapacityCost(level="soft", offset=1, clip=False, weight=2),
            TaskPrecedenceCost(offset=-1),
            MaxStationsCost(offset=1),
            MaxStationsCost(level="soft", offset=1, clip=False),
            TaskStationBoundsCost(offset=1),
            FixedTaskStationsCost(),
            ForbiddenTaskStationsCost(level="soft", offset=1, clip=False),
            SameStationGroupsCost(level="soft", offset=1, clip=False, weight=3),
            DifferentStationPairsCost(level="soft", offset=-1, clip=False),
            DifferentStationPairsCost(),
            UsedStationsCost(),
            UsedStationsCost(offset=3, exponent=2, weight=0.25),
            WorkloadImbalanceCost(weight=0.5, offset=2, clip=False),
        ],
        [
            *get_constraints(),
            StationCapacityCost(level="soft", weight=1.5, offset=1, clip=False),
            ForbiddenTaskStationsCost(level=Level.SOFT, weight=2, offset=1, clip=False),
            UsedStationsCost(level="hard", offset=3),
            WorkloadImbalanceCost(level="hard", weight=0.5, offset=1),
        ],
    ],
    ids=["repeat", "repeat-and-offsets", "repeats-on-other-levels"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
    problem = build_model()
    problem.max_stations = 4
    problem.tasks[2].forbidden_stations = [0, 1]
    core = problem.to_core()
    config = _config(steps=5000, seed=3)
    config.full_assert = True
    config.parallel_solver_count = 1
    alb.Solver(config).solve_with_constraints(core, picks)
    raw = alb.constraint_costs(core, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == core.score.hard
    assert python.soft == pytest.approx(core.score.soft)
    assert score(alb.constraint_costs(core), picks) == python  # the default costs score the repeats too


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([UsedStationsCost(level="hard")], (-3, 0.0)),
        ([TaskStationBoundsCost(level=Level.SOFT, weight=2)], (0, -4.0)),
        ([WorkloadImbalanceCost(level="hard", weight=1)], (-3, 0.0)),  # the opt-in objective, on hard
        ([StationCapacityCost(level="hard")], (-1, 0.0)),  # the default level, named
        # A scoped constraint on soft charges only its own stations: (0 - 2) + (1 - 2).
        ([ForbiddenTaskStationsCost(level="soft", offset=2, clip=False)], (0, 3.0)),
        # A repeat carries its own level and shaping. Hard: the station bounds [1, 0, 0, 1]
        # (2) and the used stations beyond 1 (3 - 1). Soft: the 3 used stations and
        # 3 x (x - 1) per station on the bounds, 3 x (0 - 1 - 1 + 0) = -6: -3 + 6.
        (
            [
                UsedStationsCost(),
                UsedStationsCost(level="hard", offset=1),
                TaskStationBoundsCost(),
                TaskStationBoundsCost(level=Level.SOFT, weight=3, offset=1, clip=False),
            ],
            (-4, 3.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(alb.constraint_costs(fixture_h()), picks) == expected
    raw = alb.constraint_costs(fixture_h(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert alb.calculate_score(fixture_h(), picks) == expected
    assert _engine(picks) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    message = "constraint 'used_stations_cost': this use case has no medium level"
    pick = UsedStationsCost(level=level)  # the spec cannot know the domain
    for picks in ([pick], [UsedStationsCost(), pick]):
        with pytest.raises(ValueError, match=message):
            _engine(picks)
        with pytest.raises(ValueError, match=message):
            alb.solve(build_problem(), config=_config(steps=10), constraints=picks)
        with pytest.raises(ValueError, match=message):
            alb.constraint_costs(fixture_h(), picks)
        with pytest.raises(ValueError, match=message):
            score(alb.constraint_costs(fixture_h()), picks)
        with pytest.raises(ValueError, match=message):
            alb.calculate_score(fixture_h(), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        UsedStationsCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        UsedStationsCost(level="firm")
    pick = SimpleNamespace(name="used_stations_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        _engine([UsedStationsCost(), pick])


def test_the_knob_was_renamed():
    """0.3.0 replaced hard=True / hard=False by level="hard" / level="soft"; hard= is unknown."""
    with pytest.raises(TypeError):
        UsedStationsCost(hard=True)
    with pytest.raises(TypeError):
        StationCapacityCost(weight=None, hard=False)


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [3.0, -3.0])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: StationCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: StationCapacityCost(level="hard", offset=offset, clip=False),
        lambda offset: MaxStationsCost(offset=offset, clip=False),  # scoped, hard by default
        lambda offset: TaskPrecedenceCost(offset=offset, clip=False),  # a global, hard by default
        lambda offset: UsedStationsCost(level=Level.HARD, offset=offset, clip=False),
    ],
    ids=["capacity-default-hard", "capacity-level-hard", "max-stations", "precedence", "used-stations-moved-to-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward every station below it, an empty one included, so the
    spare capacity of three stations would cancel station 3's overload and an infeasible
    line could score hard 0 (or above); a negative one would charge every station. The
    engine (solve, Solver, constraint_costs, calculate_score) and the score() mirror
    refuse it with the same message, alone and as a repeat; so does the CP-SAT reference
    (its own tests)."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    picks = [*get_constraints(), spec]  # a repeat next to the defaults
    assert _refusal(lambda: alb.solve(build_problem(), config=_config(steps=10), constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget_solve(fixture_h(), [spec])) == message
    assert _refusal(lambda: _zero_budget_solve(fixture_h(), picks)) == message
    assert _refusal(lambda: alb.constraint_costs(fixture_h(), [spec])) == message
    assert _refusal(lambda: alb.calculate_score(fixture_h(), [spec])) == message
    assert _refusal(lambda: score(alb.constraint_costs(fixture_h()), [spec])) == message
    assert _refusal(lambda: score(alb.constraint_costs(fixture_h()), picks)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """Capacity [0, 0, 0, 1] minus the offset on each of the four stations, on soft, on every path."""
    pick = StationCapacityCost(level="soft", offset=offset, clip=False)
    expected = (0, -(1 - 4 * offset))
    assert score(alb.constraint_costs(fixture_h()), [pick]) == expected
    raw = alb.constraint_costs(fixture_h(), [pick])
    assert (raw["station_capacity_cost"]["level"], raw["station_capacity_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert alb.calculate_score(fixture_h(), [pick]) == expected
    assert _engine([pick]) == expected
    # Next to the hard default, which keeps "hard 0 = no violation": -1 hard.
    assert _engine([StationCapacityCost(), pick]) == (-1, expected[1])


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (StationCapacityCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold
        (StationCapacityCost(offset=-3.0), (-4, 0.0)),  # 1 + 3 on station 3 only
        (StationCapacityCost(clip=False), (-1, 0.0)),  # clip=False with offset 0: the 0.2.0 shaping
        (StationCapacityCost(offset=0.0, clip=False), (-1, 0.0)),
        (MaxStationsCost(offset=-3.0), (-4, 0.0)),
        (TaskPrecedenceCost(offset=-3.0), (-4, 0.0)),
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(alb.constraint_costs(fixture_h()), [pick]) == expected
    assert _engine([pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(
        name="station_capacity_cost", weight=None, level="soft", exponent=1.0, offset=3.0, clip=False
    )
    assert _engine([moved]) == (0, 11.0)
    kept = SimpleNamespace(name="station_capacity_cost", weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q="'station_capacity_cost'")
    assert _refusal(lambda: _engine([kept])) == message
    assert _refusal(lambda: score(alb.constraint_costs(fixture_h()), [kept])) == message
