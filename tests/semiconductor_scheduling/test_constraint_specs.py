"""optimaru.semiconductor_scheduling.constraints — the declarative cost function and the catalog.

"Which constraints are on", the catalog, the five knobs (the level included) and repeated
picks are covered here.
"""

import math
import pickle
import random
import re
from types import SimpleNamespace

import pytest

import optimaru.semiconductor_scheduling as fab
from optimaru._constraint import instance_keys
from optimaru.semiconductor_scheduling import constraints as specs
from optimaru.semiconductor_scheduling.constraints import (
    Constraint,
    Level,
    MachineCapacityCost,
    MaxQueueTimeCost,
    WeightedTardinessCost,
    get_constraints,
    score,
)
from optimaru.semiconductor_scheduling.models import SemiconductorScheduleModel

from .fab_random import instance

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


def test_list_constraints_is_the_catalog_in_order():
    assert fab.list_constraints() == NAMES
    assert [spec.name for spec in get_constraints()] == NAMES


def test_module_level_instances_keep_every_default():
    for name in NAMES:
        spec = getattr(specs, name)
        assert isinstance(spec, Constraint)
        assert spec.name == name
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)


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
    # A constraint may be picked several times: each pick is its own instance.
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
        assert (entry["weight"], entry["level"]) == (
            row.default_weight,
            row.default_level,
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
                    level=rng.choice([None, "hard", "soft"]),
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
    """weight * total ** exponent: the exponent shapes the total, not each item."""
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
    picked = [MachineCapacityCost(level="soft"), specs.MakespanCost(level="hard")]
    result = fab.calculate_score(problem, picked)
    assert (result.hard, result.soft) == (-makespan, -capacity if capacity else 0.0)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"level": 1}, TypeError),
        ({"level": "firm"}, ValueError),
        ({"hard": True}, TypeError),  # an unknown keyword
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        WeightedTardinessCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    negative = SimpleNamespace(name="makespan_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="makespan_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(name="makespan_cost", weight=None, level=None, exponent=1.0, offset=float("inf"))
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
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    pick = SimpleNamespace(name="makespan_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'makespan_cost': {re.escape(message)}"):
        fab.solve(_problem(), time_limit_seconds=0, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        specs.MakespanCost(**knobs)


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


# --- repeated picks and the offset / clip knobs -----------------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call) -> str:
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


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
        # Every constraint is global: the offset shapes its single total (BUSY). The literal
        # formula with an offset is refused on hard (the 14 hard defaults), so its numbers on
        # those constraints are checked on soft (not rounded).
        (specs.MakespanCost(offset=5), (0, -6.0)),
        (specs.MakespanCost(offset=15), (0, 0.0)),
        (specs.MakespanCost(offset=15, clip=False), (0, 4.0)),  # 11 - 15 below the offset: a bonus
        (specs.MakespanCost(offset=5, exponent=2), (0, -36.0)),
        (MachineCapacityCost(level="soft", offset=3, clip=False), (0, 3.0)),  # a raw 0 is charged in literal mode
        (MachineCapacityCost(offset=-1), (0, 0.0)),  # ...and left alone by the threshold
        (MachineCapacityCost(level="soft", offset=-1, clip=False), (0, -1.0)),
        (specs.PinnedAssignmentsCost(offset=-1), (-3, 0.0)),  # a negative offset adds to a non-zero total
        (specs.LotRoutePrecedenceCost(offset=0.5), (-26, 0.0)),  # hard rounds the total: 25.5 -> 26
        (specs.LotRoutePrecedenceCost(level="soft", offset=0.5, clip=False), (0, -25.5)),
        (MachineCapacityCost(level="soft", weight=3, offset=0.5, clip=False), (0, 1.5)),  # 3 x -0.5
        (WeightedTardinessCost(offset=2), (0, -100.0)),  # the default weight 10 x (12 - 2)
        (WeightedTardinessCost(weight=0, offset=2, clip=False), (0, 0.0)),  # weight 0 contributes 0
        (specs.CycleTimeCost(level="hard", offset=1), (-10, 0.0)),
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


def test_offset_zero_is_the_plain_shaping():
    """offset 0 is the plain shaping in both modes: a pick with offset 0 scores like one without it."""
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
    """Random pick lists with repeats and offsets in both modes: the mirror == the C++ assembly.

    A list with a literal offset on hard (the pick's level, or the default) is refused by
    both paths with the same message, naming its first such pick; moved to soft, the same
    picks score alike."""
    rng = random.Random(11)
    default_level = {row.name: row.default_level for row in fab.constraint_info()}

    def literal_on_hard(pick):
        return not pick.clip and pick.offset != 0 and (pick.level or default_level[pick.name]) == "hard"

    refused_lists = 0
    for seed in range(25):
        problem = _problem(seed)
        raw = fab.constraint_costs(problem)
        picks = []
        for spec in get_constraints():
            for _ in range(rng.choice([0, 1, 1, 2])):
                picks.append(
                    type(spec)(
                        weight=rng.choice([None, 0, 0.5, 2]),
                        level=rng.choice([None, "hard", "soft"]),
                        exponent=rng.choice([1.0, 1.0, 2.0, 0.5]),
                        offset=rng.choice([0.0, 0.0, 1.0, 2.5, -1.0, 10.0]),
                        clip=rng.choice([True, False]),
                    )
                )
        if not picks:
            continue
        refused = next((pick for pick in picks if literal_on_hard(pick)), None)
        if refused is not None:
            refused_lists += 1
            message = HARD_REFUSAL.format(q=repr(refused.name))
            with pytest.raises(ValueError) as engine_refusal:
                _engine(problem, picks)
            with pytest.raises(ValueError) as costs_refusal:
                fab.constraint_costs(problem, picks)
            with pytest.raises(ValueError) as mirror_refusal:
                score(raw, picks)
            assert str(engine_refusal.value) == str(costs_refusal.value) == str(mirror_refusal.value) == message
            picks = [
                type(pick)(weight=pick.weight, level="soft", exponent=pick.exponent, offset=pick.offset, clip=pick.clip)
                if literal_on_hard(pick)
                else pick
                for pick in picks
            ]
        engine = _engine(problem, picks)
        assert score(fab.constraint_costs(problem, picks), picks) == score(raw, picks)
        python = score(raw, picks)
        assert python.hard == engine.hard
        assert python.soft == pytest.approx(engine.soft)
    assert refused_lists > 0


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT],
        [
            *get_constraints()[:14],
            specs.LotRoutePrecedenceCost(weight=2, offset=1),
            specs.LotRoutePrecedenceCost(level="soft", weight=2, offset=1, clip=False),  # literal: soft only
            MachineCapacityCost(offset=1),
            specs.MakespanCost(offset=5, clip=False),
            specs.MakespanCost(weight=0.25, exponent=2, offset=10),
            WeightedTardinessCost(offset=-3),
            specs.CycleTimeCost(weight=0.5, offset=20, clip=False),
        ],
        [
            *get_constraints(),
            MachineCapacityCost(level="soft", weight=1.5, offset=1, clip=False),
            specs.LotRoutePrecedenceCost(level=Level.SOFT, weight=2, offset=-1),
            specs.MakespanCost(level="hard", weight=0.5, offset=10),
            WeightedTardinessCost(level=Level.HARD, weight=0.25, offset=3),  # a fractional hard threshold
            WeightedTardinessCost(weight=0.25, offset=3, clip=False),  # the literal copy on its default soft
        ],
    ],
    ids=["repeat", "repeat-and-offsets", "repeats-on-other-levels"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
    problem = _problem(8)
    result = fab.solve(problem, random_seed=3, full_assert=True, config=_steps(300), constraints=picks)
    raw = fab.constraint_costs(problem, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == result.hard
    assert python.soft == pytest.approx(result.soft)
    assert score(fab.constraint_costs(problem), picks) == python  # the default costs score the repeats too


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([MachineCapacityCost(level="soft", offset=1, clip=False)], (0, 1.0)),  # a raw 0 - 1, on soft
        ([specs.LotRoutePrecedenceCost(level=Level.SOFT, weight=0.5)], (0, -13.0)),
        ([specs.MakespanCost(level=Level.HARD)], (-11, 0.0)),  # the soft objective promoted
        ([WeightedTardinessCost(level="hard")], (-120, 0.0)),  # the default weight 10, now in hard units
        ([specs.PinnedAssignmentsCost(level="hard")], (-2, 0.0)),  # the default level, named
        ([specs.CycleTimeCost(level="soft")], (0, -11.0)),
        # A repeat carries its own level and shaping. Hard: 2 x (11 - 10) on the makespan
        # repeat, the 2 pinned misses and the tardiness beyond 2 (12 - 2; the literal formula
        # with an offset is refused on hard). Soft: the makespan
        # 11, 3 x (2 - 4) on the literal pinned repeat and the precedence beyond -1 (26 + 1):
        # -11 + 6 - 27.
        (
            [
                specs.MakespanCost(),
                specs.MakespanCost(level="hard", weight=2, offset=10),
                specs.PinnedAssignmentsCost(),
                specs.PinnedAssignmentsCost(level=Level.SOFT, weight=3, offset=4, clip=False),
                WeightedTardinessCost(level=Level.HARD, weight=1, offset=2),
                specs.LotRoutePrecedenceCost(level="soft", offset=-1),
            ],
            (-14, -32.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    problem = _busy_problem()
    assert score(fab.constraint_costs(problem), picks) == expected
    raw = fab.constraint_costs(problem, picks)
    for pick, key in zip(picks, instance_keys(picks), strict=True):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
        assert "hard" not in raw[key], key
    assert score(raw, picks) == expected
    assert _engine(problem, picks) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    for name, spec in (("machine_capacity_cost", MachineCapacityCost), ("makespan_cost", specs.MakespanCost)):
        message = f"constraint '{name}': this use case has no medium level"
        pick = spec(level=level)  # the spec cannot know the domain
        for picks in ([pick], [spec(), pick]):
            with pytest.raises(ValueError, match=message):
                fab.solve(_problem(), time_limit_seconds=0, constraints=picks)
            with pytest.raises(ValueError, match=message):
                _engine(_problem(), picks)
            with pytest.raises(ValueError, match=message):
                fab.constraint_costs(_problem(), picks)
            with pytest.raises(ValueError, match=message):
                score(fab.constraint_costs(_problem()), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        specs.MakespanCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        specs.MakespanCost(level="firm")
    pick = SimpleNamespace(name="makespan_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        _engine(_problem(), [specs.MakespanCost(), pick])


# --- clip=False with an offset stays off the hard score ---------------------------


@pytest.mark.parametrize("offset", [3.0, -3.0])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: MachineCapacityCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: specs.LotRoutePrecedenceCost(level="hard", offset=offset, clip=False),
        lambda offset: specs.MakespanCost(level=Level.HARD, offset=offset, clip=False),  # an objective moved to hard
    ],
    ids=["capacity-default-hard", "precedence-level-hard", "makespan-moved-to-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """Every constraint is global, so the literal formula shapes one total, a raw 0 included.
    On hard a positive offset would turn a clean total into a hard reward: on the busy
    instance, MachineCapacityCost(offset=26, clip=False) would score +26 for its total of 0
    and cancel the 26 precedence violations. A negative one would charge a schedule without
    a violation. solve, calculate_score, constraint_costs and the score() mirror refuse it
    with the same message, alone and as a repeat; so does the CP-SAT reference (its own
    tests)."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    picks = [*get_constraints(), spec]  # a repeat next to the defaults
    problem = _busy_problem()
    assert _refusal(lambda: fab.solve(problem.model_copy(deep=True), time_limit_seconds=0, constraints=[spec])) == (
        message
    )
    assert _refusal(lambda: fab.solve(problem.model_copy(deep=True), config=_steps(10), constraints=picks)) == message
    assert _refusal(lambda: _engine(problem, [spec])) == message
    assert _refusal(lambda: _engine(problem, picks)) == message
    assert _refusal(lambda: fab.constraint_costs(problem, [spec])) == message
    assert _refusal(lambda: score(fab.constraint_costs(problem), [spec])) == message
    assert _refusal(lambda: score(fab.constraint_costs(problem), picks)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """The capacity total 0 minus the offset, -(0 - offset), on soft, on every path; next to
    the hard defaults, which keep "hard 0 = no violation"."""
    pick = MachineCapacityCost(level="soft", offset=offset, clip=False)
    expected = (0, offset)
    problem = _busy_problem()
    assert score(fab.constraint_costs(problem), [pick]) == expected
    raw = fab.constraint_costs(problem, [pick])
    assert (raw["machine_capacity_cost"]["level"], raw["machine_capacity_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _engine(problem, [pick]) == expected
    defaults = _engine(problem, get_constraints())
    picks = [*get_constraints(), pick]
    engine = _engine(problem, picks)
    assert engine == score(fab.constraint_costs(problem), picks)
    assert (engine.hard, engine.soft) == (defaults.hard, pytest.approx(defaults.soft + offset))
    solved = problem.model_copy(deep=True)
    result = fab.solve(solved, random_seed=1, config=_steps(50), constraints=picks)
    assert tuple(score(fab.constraint_costs(solved), picks)) == pytest.approx(tuple(result))


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (MachineCapacityCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold, a raw 0 stays 0
        (specs.PinnedAssignmentsCost(offset=-3.0), (-5, 0.0)),  # 2 + 3
        (specs.PinnedAssignmentsCost(clip=False), (-2, 0.0)),  # clip=False with offset 0: the plain shaping
        (specs.PinnedAssignmentsCost(offset=0.0, clip=False), (-2, 0.0)),
        (specs.MakespanCost(level="hard", offset=5.0), (-6, 0.0)),  # 11 - 5
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    problem = _busy_problem()
    assert score(fab.constraint_costs(problem), [pick]) == expected
    assert score(fab.constraint_costs(problem, [pick]), [pick]) == expected
    assert _engine(problem, [pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick ends on decides."""
    name = "machine_capacity_cost"
    problem = _busy_problem()
    moved = SimpleNamespace(name=name, weight=None, level="soft", exponent=1.0, offset=3.0, clip=False)
    assert _engine(problem, [moved]) == score(fab.constraint_costs(problem), [moved]) == (0, 3.0)
    kept = SimpleNamespace(name=name, weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q=repr(name))
    assert _refusal(lambda: _engine(problem, [kept])) == message
    assert _refusal(lambda: score(fab.constraint_costs(problem), [kept])) == message
