"""semiconductor_scheduling.constraints — the declarative cost function and the catalog."""

import math
import pickle
import random
import re
from types import SimpleNamespace

import pytest
from fab_random import instance

import semiconductor_scheduling as fab
from semiconductor_scheduling import constraints as specs
from semiconductor_scheduling.constraints import (
    Constraint,
    MachineCapacityCost,
    MaxQueueTimeCost,
    WeightedTardinessCost,
    get_constraints,
    score,
)
from semiconductor_scheduling.models import SemiconductorScheduleModel

NAMES = [
    "machine_eligibility_cost",
    "lot_route_precedence_cost",
    "machine_capacity_cost",
    "batch_capacity_cost",
    "batch_family_conflict_cost",
    "missing_setup_time_cost",
    "machine_unavailability_cost",
    "max_queue_time_cost",
    "min_queue_time_cost",
    "missing_transport_time_cost",
    "auxiliary_resource_capacity_cost",
    "auxiliary_resource_unavailability_cost",
    "pinned_assignments_cost",
    "max_cycle_time_cost",
    "machine_preference_cost",
    "min_batch_size_cost",
    "machine_load_imbalance_cost",
    "weighted_tardiness_cost",
    "cycle_time_cost",
    "makespan_cost",
]

OLD_NAMES = [
    "machine_eligibility",
    "lot_route_precedence",
    "machine_capacity",
    "batch_capacity",
    "batch_family_compatibility",
    "setup_transitions",
    "maintenance_windows",
    "maximum_queue_time",
    "minimum_queue_time",
    "inter_tool_transport",
    "auxiliary_resource_capacity",
    "auxiliary_resource_availability",
    "pinned_assignments",
    "maximum_cycle_time",
    "machine_preference",
    "minimum_batch_fill",
    "machine_load_balance",
    "weighted_tardiness",
    "total_cycle_time",
    "makespan",
]


def _problem(seed: int = 3) -> SemiconductorScheduleModel:
    return SemiconductorScheduleModel.model_validate(instance(seed))


def _busy_problem() -> SemiconductorScheduleModel:
    """A random instance whose assignments break hard and soft rules alike."""
    for seed in range(200):
        problem = _problem(seed)
        raw = fab.constraint_costs(problem)
        if sum(raw[name]["costs"][0] > 0 for name in NAMES) >= 10:
            return problem
    raise AssertionError("no busy instance")


def test_list_constraints_is_the_renamed_catalog_in_order():
    assert fab.list_constraints() == NAMES
    assert [spec.name for spec in get_constraints()] == NAMES


def test_module_level_instances_keep_every_default():
    for name in NAMES:
        spec = getattr(specs, name)
        assert isinstance(spec, Constraint)
        assert spec.name == name
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"
        assert "Default:" in (cls.__doc__ or "")


def test_get_constraints_contract():
    assert get_constraints() == list(specs._DEFAULTS)
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])
    with pytest.raises(ValueError, match="must not be empty"):
        fab.constraint_costs(_problem(), [])
    # Since 0.3.0 a constraint may be picked several times: each pick is its own instance.
    assert get_constraints([specs.MakespanCost(), specs.MakespanCost()]) == [specs.MakespanCost()] * 2
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    rows = fab.constraint_info()
    assert [row.name for row in rows] == NAMES
    expected = [("hard", 1.0)] * 14 + [("soft", 1.0)] * 3 + [("soft", 10.0), ("soft", 1.0), ("soft", 1.0)]
    assert [(row.default_level, row.default_weight) for row in rows] == expected
    assert all(row.level_flippable and not row.reward and row.default_enabled for row in rows)


def test_constraint_info_rows_are_plain_values():
    row = fab.constraint_info()[17]
    assert isinstance(row, fab.ConstraintInfo)
    assert row._asdict() == {
        "name": "weighted_tardiness_cost",
        "default_level": "soft",
        "default_weight": 10.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='weighted_tardiness_cost'")
    assert pickle.loads(pickle.dumps(fab.constraint_info())) == fab.constraint_info()
    assert fab.ConstraintInfo.__module__ == fab._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    raw = fab.constraint_costs(_problem())
    assert list(raw) == NAMES
    for row in fab.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"], entry["global"]) == (True, False, True)
        assert (entry["name"], entry["label"], entry["exponent"]) == (row.name, "", 1.0)
        assert len(entry["costs"]) == 1  # a global constraint reports its total
        assert "medium_fold" not in entry


# --- The knobs ----------------------------------------------------------------


def test_weight_none_means_the_component_default():
    explicit = [type(spec)(weight=10 if spec.name == "weighted_tardiness_cost" else 1) for spec in get_constraints()]
    problem = _busy_problem()
    raw = fab.constraint_costs(problem)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == fab.calculate_score(problem)


def test_score_equals_the_engine_under_random_knobs():
    """score(constraint_costs) re-prices exactly like the C++ assembly (calculate_score with picks)."""
    rng = random.Random(7)
    for seed in range(25):
        problem = _problem(seed)
        raw = fab.constraint_costs(problem)
        picks = []
        for spec in get_constraints():
            if rng.random() < 0.2:
                continue
            picks.append(
                type(spec)(
                    weight=rng.choice([None, 0, 0.5, 2, 3.25]),
                    hard=rng.choice([None, True, False]),
                    exponent=rng.choice([1.0, 1.0, 2.0, 0.5]),
                )
            )
        if not picks:
            continue
        engine = fab.calculate_score(problem.model_copy(deep=True), picks)
        python = score(raw, picks)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)


def test_exponent_shapes_the_total_of_a_global_constraint():
    """0.2.0: weight * total ** exponent (0.1.x shaped each item)."""
    problem = _busy_problem()
    raw = fab.constraint_costs(problem)
    total = raw["cycle_time_cost"]["costs"][0]
    picked = [specs.machine_eligibility_cost, specs.CycleTimeCost(weight=2, exponent=2.0)]
    expected_soft = -(2 * total**2)
    assert score(raw, picked).soft == pytest.approx(expected_soft)
    assert fab.calculate_score(problem, picked).soft == pytest.approx(expected_soft)


def test_fractional_hard_weight_rounds_per_constraint():
    problem = _busy_problem()
    raw = fab.constraint_costs(problem)
    queue = raw["max_queue_time_cost"]["costs"][0]
    precedence = raw["lot_route_precedence_cost"]["costs"][0]
    picked = [MaxQueueTimeCost(weight=0.5), specs.LotRoutePrecedenceCost(weight=0.5)]

    def half_away(value):
        return math.floor(value + 0.5)

    assert fab.calculate_score(problem, picked).hard == -(half_away(queue / 2) + half_away(precedence / 2))


def test_levels_flip_both_ways():
    problem = _busy_problem()
    raw = fab.constraint_costs(problem)
    capacity = raw["machine_capacity_cost"]["costs"][0]
    makespan = raw["makespan_cost"]["costs"][0]
    picked = [MachineCapacityCost(hard=False), specs.MakespanCost(hard=True)]
    result = fab.calculate_score(problem, picked)
    assert (result.hard, result.soft) == (-makespan, -capacity if capacity else 0.0)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        WeightedTardinessCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    negative = SimpleNamespace(name="makespan_cost", weight=-2.0, hard=None, exponent=1.0)
    flat = SimpleNamespace(name="makespan_cost", weight=None, hard=None, exponent=0.0)
    unbounded = SimpleNamespace(name="makespan_cost", weight=None, hard=None, exponent=1.0, offset=float("inf"))
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            fab.solve(_problem(), time_limit_seconds=0, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            fab.calculate_score(_problem(), [pick])
        with pytest.raises(ValueError, match=message):
            fab.constraint_costs(_problem(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        specs.MakespanCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    pick = SimpleNamespace(name="makespan_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'makespan_cost': {re.escape(message)}"):
        fab.solve(_problem(), time_limit_seconds=0, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        specs.MakespanCost(**knobs)


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_unknown(old):
    """0.2.0 renamed the catalog; the 0.1.x names are gone (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        fab.calculate_score(_problem(), [pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(fab.constraint_costs(_problem()), [pick])


# --- Solving with picks ---------------------------------------------------------


def test_default_solve_equals_explicit_default_constraints():
    plain = _problem(5)
    explicit = _problem(5)
    fab.solve(plain, time_limit_seconds=5, random_seed=2, parallel_solver_count=1, config=_steps(300))
    fab.solve(explicit, time_limit_seconds=5, random_seed=2, config=_steps(300), constraints=get_constraints())
    assert plain.assignments == explicit.assignments
    assert (plain.score_hard, plain.score_soft) == (explicit.score_hard, explicit.score_soft)


def test_picks_score_the_result_and_the_totals_stay_complete():
    problem = _problem(8)
    picks = [specs.machine_eligibility_cost, WeightedTardinessCost(weight=3)]
    result = fab.solve(problem, time_limit_seconds=5, random_seed=1, config=_steps(200), constraints=picks)
    raw = fab.constraint_costs(problem)
    assert result == score(raw, picks)
    assert (problem.score_hard, problem.score_soft) == tuple(result)
    assert problem.makespan == raw["makespan_cost"]["costs"][0]
    assert problem.total_cycle_time == raw["cycle_time_cost"]["costs"][0]


def _steps(count: int) -> fab.SolverConfig:
    config = fab.SolverConfig()
    config.step_count_limit = count
    return config


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------

# The busy instance's raw totals the hand cases below read (a precondition test pins them).
BUSY = {
    "makespan_cost": 11.0,
    "cycle_time_cost": 11.0,
    "weighted_tardiness_cost": 12.0,
    "lot_route_precedence_cost": 26.0,
    "pinned_assignments_cost": 2.0,
    "machine_capacity_cost": 0.0,
}


def _engine(problem: SemiconductorScheduleModel, picks):
    """The C++ assembly's score of the problem's assignments under ``picks``."""
    return fab.calculate_score(problem.model_copy(deep=True), picks)


def test_the_busy_instance_holds_the_totals_the_hand_cases_read():
    raw = fab.constraint_costs(_busy_problem())
    assert {name: raw[name]["costs"][0] for name in BUSY} == BUSY


REPEAT = [specs.MakespanCost(), specs.MakespanCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """makespan x 1 plus 0.5 x makespan^2 on 11: 11 + 60.5 = 71.5 soft."""
    problem = _busy_problem()
    raw = fab.constraint_costs(problem, REPEAT)
    assert list(raw) == ["makespan_cost", "makespan_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [11.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "makespan_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -(11 + 0.5 * 11**2))
    assert expected == (0, -71.5)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw total.
    assert score(fab.constraint_costs(problem), REPEAT) == expected
    assert _engine(problem, REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [specs.CycleTimeCost(), specs.CycleTimeCost()]
    problem = _busy_problem()
    assert score(fab.constraint_costs(problem), picks) == (0, -22.0)
    assert _engine(problem, picks) == (0, -22.0)
    assert list(fab.constraint_costs(problem, picks)) == ["cycle_time_cost", "cycle_time_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        specs.CycleTimeCost(weight=1),
        specs.MakespanCost(),
        specs.CycleTimeCost(weight=2),
        specs.PinnedAssignmentsCost(),
        specs.CycleTimeCost(weight=3),
    ]
    problem = _busy_problem()
    raw = fab.constraint_costs(problem, picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "pinned_assignments_cost": 1.0,
        "cycle_time_cost": 1.0,
        "cycle_time_cost#2": 2.0,
        "cycle_time_cost#3": 3.0,
        "makespan_cost": 1.0,
    }
    assert score(raw, picks) == (-2, -(11 * 6 + 11))
    assert _engine(problem, picks) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Every constraint is global: the offset shapes its single total (BUSY).
        (specs.MakespanCost(offset=5), (0, -6.0)),
        (specs.MakespanCost(offset=15), (0, 0.0)),
        (specs.MakespanCost(offset=15, clip=False), (0, 4.0)),  # 11 - 15 below the offset: a bonus
        (specs.MakespanCost(offset=5, exponent=2), (0, -36.0)),
        (MachineCapacityCost(offset=3, clip=False), (3, 0.0)),  # a raw 0 is charged in literal mode
        (MachineCapacityCost(offset=-1), (0, 0.0)),  # ...and left alone by the threshold
        (MachineCapacityCost(offset=-1, clip=False), (-1, 0.0)),
        (specs.PinnedAssignmentsCost(offset=-1), (-3, 0.0)),  # a negative offset adds to a non-zero total
        (specs.LotRoutePrecedenceCost(offset=0.5, clip=False), (-26, 0.0)),  # 25.5 rounds half away: 26
        (MachineCapacityCost(weight=3, offset=0.5, clip=False), (2, 0.0)),  # 3 x -0.5 = -1.5 -> -2
        (WeightedTardinessCost(offset=2), (0, -100.0)),  # the default weight 10 x (12 - 2)
        (WeightedTardinessCost(weight=0, offset=2, clip=False), (0, 0.0)),  # weight 0 contributes 0
        (specs.CycleTimeCost(hard=True, offset=1), (-10, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    problem = _busy_problem()
    assert score(fab.constraint_costs(problem), [pick]) == expected
    raw = fab.constraint_costs(problem, [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # every constraint is global, none is scoped
    assert score(raw, [pick]) == expected
    assert _engine(problem, [pick]) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [MaxQueueTimeCost(weight=0.5), specs.CycleTimeCost(exponent=2), WeightedTardinessCost(exponent=1.5)]
    shifted = [
        MaxQueueTimeCost(weight=0.5, offset=0.0, clip=False),
        specs.CycleTimeCost(exponent=2, offset=0),
        WeightedTardinessCost(exponent=1.5, offset=0.0, clip=False),
    ]
    problem = _busy_problem()
    raw = fab.constraint_costs(problem)
    assert score(raw, shifted) == score(raw, plain)
    assert _engine(problem, plain) == _engine(problem, shifted) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


def test_score_equals_the_engine_with_repeats_and_offsets():
    """Random pick lists with repeats and offsets in both modes: the mirror == the C++ assembly."""
    rng = random.Random(11)
    for seed in range(25):
        problem = _problem(seed)
        raw = fab.constraint_costs(problem)
        picks = []
        for spec in get_constraints():
            for _ in range(rng.choice([0, 1, 1, 2])):
                picks.append(
                    type(spec)(
                        weight=rng.choice([None, 0, 0.5, 2]),
                        hard=rng.choice([None, True, False]),
                        exponent=rng.choice([1.0, 1.0, 2.0, 0.5]),
                        offset=rng.choice([0.0, 0.0, 1.0, 2.5, -1.0, 10.0]),
                        clip=rng.choice([True, False]),
                    )
                )
        if not picks:
            continue
        engine = _engine(problem, picks)
        assert score(fab.constraint_costs(problem, picks), picks) == score(raw, picks)
        python = score(raw, picks)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT],
        [
            *get_constraints()[:14],
            specs.LotRoutePrecedenceCost(weight=2, offset=1, clip=False),
            MachineCapacityCost(offset=1),
            specs.MakespanCost(offset=5, clip=False),
            specs.MakespanCost(weight=0.25, exponent=2, offset=10),
            WeightedTardinessCost(offset=-3),
            specs.CycleTimeCost(weight=0.5, offset=20, clip=False),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    problem = _problem(8)
    result = fab.solve(problem, random_seed=3, full_assert=True, config=_steps(300), constraints=picks)
    raw = fab.constraint_costs(problem, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == result.hard
    assert python.soft == pytest.approx(result.soft)
    assert score(fab.constraint_costs(problem), picks) == python  # the default costs score the repeats too
