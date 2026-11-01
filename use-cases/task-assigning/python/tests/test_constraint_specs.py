"""task_assigning.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the catalog
(`constraint_info()`), the knobs, and the pure-Python `score()` against the engine —
including the domain's medium level (`unassigned_tasks_cost`, folded below hard).
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import task_assigning as ta
from task_assigning.constraints import (
    Constraint,
    CriticalPriorityTaskEndTimeCost,
    MajorPriorityTaskEndTimeCost,
    MinorPriorityTaskEndTimeCost,
    RequiredSkillCost,
    SquaredMakespanCost,
    UnassignedTasksCost,
    critical_priority_task_end_time_cost,
    get_constraints,
    major_priority_task_end_time_cost,
    minor_priority_task_end_time_cost,
    required_skill_cost,
    score,
    squared_makespan_cost,
    unassigned_tasks_cost,
)
from task_assigning.demo import generate_demo_data

MEDIUM_PER_HARD = 10**9  # kMissingSkillUnit: the below-hard medium fold
SOFT1_WEIGHT = 10**7  # squared_makespan_cost's default weight (soft1 over soft2)


def build_solution():
    return generate_demo_data().to_core()


def solve(constraints=None):
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return solution.score


def engine(levels):
    """The domain Score (hard, soft0, soft1, soft2) folded the way the engine folds it."""
    return (levels.hard * MEDIUM_PER_HARD + levels.soft0, levels.soft1 * SOFT1_WEIGHT + levels.soft2)


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, the global constraint (unassigned_tasks_cost) after.
    available = ta.list_constraints()
    assert available == [
        "required_skill_cost",
        "squared_makespan_cost",
        "critical_priority_task_end_time_cost",
        "major_priority_task_end_time_cost",
        "minor_priority_task_end_time_cost",
        "unassigned_tasks_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ta.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    solution = build_solution()
    ta.solve(
        solution,
        time_limit_seconds=0.6,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = ta.calculate_score(solution)
    assert solution.score.hard == recomputed.hard
    assert solution.score.soft0 == recomputed.soft0
    assert solution.score.soft1 == pytest.approx(recomputed.soft1)
    assert solution.score.soft2 == pytest.approx(recomputed.soft2)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — that level is emptied entirely.

    Four Bendable levels: hard = missing skills, soft0 = unassigned,
    soft1 = makespan, soft2 = priority end times."""
    baseline = solve()
    assert baseline.soft1 < 0 and baseline.soft2 < 0

    hard_only = solve([required_skill_cost, unassigned_tasks_cost])
    assert hard_only.soft1 == 0
    assert hard_only.soft2 == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the makespan weight grows soft only; hard stays put."""
    picks = [required_skill_cost, unassigned_tasks_cost]
    base = solve([*picks, squared_makespan_cost])
    heavier = solve([*picks, SquaredMakespanCost(weight=10**9)])
    assert heavier.hard == base.hard
    assert heavier.soft1 < base.soft1  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard side.

    As long as every task is assigned, priority end times can never be 0. Raising the
    level charges its weight in HARD units (0.2.0: the 10^9 between the hard and the
    medium level is the domain's fold, not a weight), so the penalty moves from soft2
    to the Bendable hard level wholesale."""
    picks = [unassigned_tasks_cost]

    soft_level = solve([*picks, critical_priority_task_end_time_cost])
    assert soft_level.soft2 < 0
    assert soft_level.hard == 0 and soft_level.soft0 == 0

    hard_level = solve([*picks, CriticalPriorityTaskEndTimeCost(level="hard")])
    assert hard_level.soft2 == 0
    assert hard_level.hard < 0
    assert hard_level.soft0 == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "required_skills_cost"  # extra s
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ta.solve(build_solution(), time_limit_seconds=0.2, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_is_rejected():
    """The later pick must not silently win: state each constraint once."""
    with pytest.raises(ValueError, match="picked more than once"):
        ta.solve(
            build_solution(),
            time_limit_seconds=0.2,
            constraints=[required_skill_cost, SquaredMakespanCost(weight=10), squared_makespan_cost],
        )
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([required_skill_cost, required_skill_cost])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    instances = (
        required_skill_cost,
        squared_makespan_cost,
        critical_priority_task_end_time_cost,
        major_priority_task_end_time_cost,
        minor_priority_task_end_time_cost,
        unassigned_tasks_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert list(get_constraints()) == list(instances)


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = ta.constraint_info()
    assert [row.name for row in rows] == ta.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("soft", 1e7, True, False, True),
        ("soft", 4.0, True, False, True),
        ("soft", 2.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("medium", 1.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = ta.constraint_info()[0]
    assert isinstance(row, ta.ConstraintInfo)
    assert row._asdict() == {
        "name": "required_skill_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='required_skill_cost'")
    assert pickle.loads(pickle.dumps(ta.constraint_info())) == ta.constraint_info()
    assert ta.ConstraintInfo.__module__ == ta._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint,
    and the domain's below-hard medium fold on EVERY entry."""
    raw = ta.constraint_costs(build_solution())
    for row in ta.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name == "unassigned_tasks_cost")
        assert (entry["name"], entry["label"]) == (row.name, "")  # one instance per name, no label
        assert entry["medium_fold"] == {"kind": "below_hard", "unit": 1e9}


# --- The knobs ------------------------------------------------------------------


def _solved(constraints=None):
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return solution


def _scrambled():
    """A solved solution with two tasks taken off the longest list and one more moved."""
    solution = _solved()
    lists = solution.task_lists()
    longest = max(range(len(lists)), key=lambda e: len(lists[e]))
    lists[longest] = lists[longest][2:]
    donor = max(range(len(lists)), key=lambda e: len(lists[e]))
    moved = lists[donor].pop()
    lists[(donor + 1) % len(lists)].append(moved)
    solution.set_task_lists(lists)
    ta.calculate_score(solution)
    return solution


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights 1 / 1e7 / 4 / 2 / 1 / 1."""
    explicit = [
        RequiredSkillCost(weight=1),
        SquaredMakespanCost(weight=10**7),
        CriticalPriorityTaskEndTimeCost(weight=4),
        MajorPriorityTaskEndTimeCost(weight=2),
        MinorPriorityTaskEndTimeCost(weight=1),
        UnassignedTasksCost(weight=1),
    ]
    solution = _scrambled()
    raw = ta.constraint_costs(solution)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == engine(ta.calculate_score(solution))


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
        SquaredMakespanCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "squared_makespan_cost"
        weight = -2.0
        level = None
        exponent = 1.0

    class Flat:
        name = "squared_makespan_cost"
        weight = None
        level = None
        exponent = 0.0

    for pick, message in ((Negative(), "weight must be finite and >= 0"), (Flat(), "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])


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
    pick = SimpleNamespace(name="squared_makespan_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'squared_makespan_cost': {re.escape(message)}"):
        ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        SquaredMakespanCost(**knobs)


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    old_names = (
        "no_missing_skills",
        "minimize_unassigned_tasks",
        "minimize_makespan",
        "critical_priority_task_end_time",
        "major_priority_task_end_time",
        "minor_priority_task_end_time",
    )
    raw = ta.constraint_costs(build_solution())
    for old in old_names:
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])


# --- The medium level (unassigned_tasks_cost, folded below hard) --------------


def _nobody():
    """The demo tasks without employees: nothing can be assigned, so solve() only scores
    the all-unassigned state with the caller's constraints (the engine path)."""
    core = build_solution()
    return ta.TaskAssigningSolution(core.task_types, core.customers, core.tasks, [])


def _with_unassigned():
    """The demo instance with every task but the first three on employee 0's list."""
    solution = build_solution()
    lists = [[] for _ in solution.employees]
    lists[0] = list(range(3, len(solution.tasks)))
    solution.set_task_lists(lists)
    return solution


@pytest.mark.parametrize(
    ("pick", "per_task"),
    [
        (unassigned_tasks_cost, (0, -1, 0, 0)),  # medium: soft0, folded below hard
        (UnassignedTasksCost(level="hard"), (-1, 0, 0, 0)),  # one hard unit per task
        (UnassignedTasksCost(level="soft"), (0, 0, 0, -1)),  # one soft unit per task, outside the fold
        (UnassignedTasksCost(weight=5), (0, -5, 0, 0)),  # an explicit weight stays in medium units
    ],
)
def test_the_engine_applies_the_medium_knobs(pick, per_task):
    """level="hard" / level="soft" / weight on the medium constraint, through the C++ binding,
    and score() on the same raw costs agrees with the engine."""
    selection = [required_skill_cost, pick]
    solution = _nobody()
    ta.solve(solution, time_limit_seconds=0.1, constraints=selection)
    tasks = len(solution.tasks)
    levels = solution.score
    assert (levels.hard, levels.soft0, levels.soft1, levels.soft2) == tuple(tasks * unit for unit in per_task)
    assert score(ta.constraint_costs(solution), selection) == engine(levels)


def test_the_medium_level_folds_below_hard():
    """Default: engine.hard = hard * 10^9 + medium; the medium raw cost is a task count."""
    solution = _with_unassigned()
    levels = ta.calculate_score(solution)
    raw = ta.constraint_costs(solution)
    assert raw["unassigned_tasks_cost"]["costs"] == [3.0]
    assert levels.soft0 == -3
    assert score(raw) == engine(levels)
    assert score(raw).hard == levels.hard * MEDIUM_PER_HARD - 3


def test_score_applies_the_medium_knobs_like_the_engine():
    """level="hard": one hard unit per task; level="soft": one soft unit per task; weight 5:
    five medium units per task."""
    solution = _with_unassigned()
    raw = ta.constraint_costs(solution)
    hard = ta.calculate_score(solution).hard
    assert score(raw, [required_skill_cost, UnassignedTasksCost(level="hard")]) == ((hard - 3) * MEDIUM_PER_HARD, 0.0)
    assert score(raw, [required_skill_cost, UnassignedTasksCost(level="soft")]) == (hard * MEDIUM_PER_HARD, -3.0)
    assert score(raw, [required_skill_cost, UnassignedTasksCost(weight=5)]) == (hard * MEDIUM_PER_HARD - 15, 0.0)


@pytest.mark.parametrize(
    "selection",
    [
        [RequiredSkillCost()],
        [RequiredSkillCost(level="soft")],
        [UnassignedTasksCost(level="soft")],
        [required_skill_cost, critical_priority_task_end_time_cost],
    ],
)
def test_a_selection_without_a_medium_term_equals_the_engine(selection):
    """below_hard folds hard * 10^9 whatever the selection holds — also with no medium
    term selected, or the medium term demoted to soft."""
    solved = _solved(selection)
    assert score(ta.constraint_costs(solved), selection) == engine(solved.score)
    nobody = _nobody()
    ta.solve(nobody, time_limit_seconds=0.1, constraints=selection)
    assert score(ta.constraint_costs(nobody), selection) == engine(nobody.score)


def test_required_skills_alone_still_fold_hard_by_the_medium_unit():
    solution = _with_unassigned()
    hard = ta.calculate_score(solution).hard
    assert hard < 0  # employee 0 lacks some of the required skills
    assert score(ta.constraint_costs(solution), [RequiredSkillCost()]) == (hard * MEDIUM_PER_HARD, 0.0)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    solution = _solved()
    return ta.constraint_costs(solution), solution


def test_score_equals_the_engine_for_the_default_assembly():
    """score(raw) is the engine-level (hard, soft); fold the domain Score to compare."""
    for solution in (_solved(), _scrambled(), _with_unassigned()):
        python = score(ta.constraint_costs(solution))
        assert python == engine(ta.calculate_score(solution))


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [required_skill_cost],
        [RequiredSkillCost(level="soft")],
        [required_skill_cost, unassigned_tasks_cost, SquaredMakespanCost(weight=10)],
        [RequiredSkillCost(level="soft"), unassigned_tasks_cost, squared_makespan_cost],
        [UnassignedTasksCost(level="soft"), squared_makespan_cost, minor_priority_task_end_time_cost],
        [required_skill_cost, unassigned_tasks_cost, CriticalPriorityTaskEndTimeCost(exponent=2.0)],
        [RequiredSkillCost(exponent=2.0), MajorPriorityTaskEndTimeCost(level="hard", weight=0.5), unassigned_tasks_cost],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per unit, so reproducing it needs the per-unit raw costs —
    totals alone are not enough."""
    solution = _solved(specs)
    python = score(ta.constraint_costs(solution), specs)
    expected = engine(solution.score)
    assert python.hard == expected[0]
    assert python.soft == pytest.approx(expected[1])


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out.

    Hard rounding happens per unit contribution, so splitting by constraint and
    summing gives the same value. "How much does this term add to the score"
    can be read without rescoring."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """Changing only the weight scales that term linearly — no rescoring."""
    raw, _ = _raw()
    base = score(raw, [squared_makespan_cost])
    ten = score(raw, [SquaredMakespanCost(weight=10)])
    default_weight = raw[squared_makespan_cost.name]["weight"]
    assert default_weight == 1e7
    factor = 10 / default_weight
    assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])
