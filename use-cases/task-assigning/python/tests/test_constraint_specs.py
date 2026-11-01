"""task_assigning.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the catalog
(`constraint_info()`), the five knobs, repeated picks, and the pure-Python `score()`
against the engine — including the domain's medium level (`unassigned_tasks_cost`,
folded below hard).
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

    hard_level = solve([*picks, CriticalPriorityTaskEndTimeCost(hard=True)])
    assert hard_level.soft2 == 0
    assert hard_level.hard < 0
    assert hard_level.soft0 == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "required_skills_cost"  # extra s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ta.solve(build_solution(), time_limit_seconds=0.2, constraints=[])
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [RequiredSkillCost(), SquaredMakespanCost(weight=10), SquaredMakespanCost()]
    assert _zero_budget(_hand(), picks) == (-MEDIUM_PER_HARD, -(10 + SOFT1_WEIGHT) * (900 + 25))
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.2, random_seed=1, constraints=picks)
    python = score(ta.constraint_costs(solution, picks), picks)
    assert python.hard == engine(solution.score)[0]
    assert python.soft == pytest.approx(engine(solution.score)[1])
    assert get_constraints([RequiredSkillCost(), RequiredSkillCost()]) == [RequiredSkillCost(), RequiredSkillCost()]


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, hard=None, exponent=1)."""
    instances = (
        required_skill_cost,
        squared_makespan_cost,
        critical_priority_task_end_time_cost,
        major_priority_task_end_time_cost,
        minor_priority_task_end_time_cost,
        unassigned_tasks_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
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
        hard = None
        exponent = 1.0

    class Flat:
        name = "squared_makespan_cost"
        weight = None
        hard = None
        exponent = 0.0

    class Unbounded:
        name = "squared_makespan_cost"
        weight = None
        hard = None
        exponent = 1.0
        offset = float("inf")

    for pick, message in (
        (Negative(), "weight must be finite and >= 0"),
        (Flat(), "exponent must be finite and > 0"),
        (Unbounded(), "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            ta.constraint_costs(build_solution(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        SquaredMakespanCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="squared_makespan_cost", weight=None, hard=None, exponent=1.0)
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
        pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
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
        (UnassignedTasksCost(hard=True), (-1, 0, 0, 0)),  # one hard unit per task
        (UnassignedTasksCost(hard=False), (0, 0, 0, -1)),  # one soft unit per task, outside the fold
        (UnassignedTasksCost(weight=5), (0, -5, 0, 0)),  # an explicit weight stays in medium units
    ],
)
def test_the_engine_applies_the_medium_knobs(pick, per_task):
    """hard=True / hard=False / weight on the medium constraint, through the C++ binding,
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
    """hard=True: one hard unit per task; hard=False: one soft unit per task; weight 5:
    five medium units per task."""
    solution = _with_unassigned()
    raw = ta.constraint_costs(solution)
    hard = ta.calculate_score(solution).hard
    assert score(raw, [required_skill_cost, UnassignedTasksCost(hard=True)]) == ((hard - 3) * MEDIUM_PER_HARD, 0.0)
    assert score(raw, [required_skill_cost, UnassignedTasksCost(hard=False)]) == (hard * MEDIUM_PER_HARD, -3.0)
    assert score(raw, [required_skill_cost, UnassignedTasksCost(weight=5)]) == (hard * MEDIUM_PER_HARD - 15, 0.0)


@pytest.mark.parametrize(
    "selection",
    [
        [RequiredSkillCost()],
        [RequiredSkillCost(hard=False)],
        [UnassignedTasksCost(hard=False)],
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
        [RequiredSkillCost(hard=False)],
        [required_skill_cost, unassigned_tasks_cost, SquaredMakespanCost(weight=10)],
        [RequiredSkillCost(hard=False), unassigned_tasks_cost, squared_makespan_cost],
        [UnassignedTasksCost(hard=False), squared_makespan_cost, minor_priority_task_end_time_cost],
        [required_skill_cost, unassigned_tasks_cost, CriticalPriorityTaskEndTimeCost(exponent=2.0)],
        [RequiredSkillCost(exponent=2.0), MajorPriorityTaskEndTimeCost(hard=True, weight=0.5), unassigned_tasks_cost],
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


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------
#
# The hand instance is _hand(): e0 (no skills) runs t0 (critical, 0..10, needs skill x)
# and t1 (major, 10..30), e1 runs t2 (minor, 0..5), e2 is idle; every task is assigned
# (construction would place an unassigned one even at a zero budget). Raw per employee
# (e0, e1, e2): required_skill_cost [1, 0, 0], squared_makespan_cost [900, 25, 0],
# critical [10, 0, 0], major [30, 0, 0], minor [0, 5, 0]; unassigned_tasks_cost [0].
# Every employee is an evaluated unit, the idle one included.


def _hand():
    types = [
        ta.TaskType("A", base_duration=10, required_skills=["x"]),
        ta.TaskType("B", base_duration=20),
        ta.TaskType("C", base_duration=5),
    ]
    employees = [
        ta.Employee(f"e{i}", affinity_by_customer=[ta.Affinity.HIGH], tasks=tasks)
        for i, tasks in enumerate([[0, 1], [2], []])
    ]
    tasks = [
        ta.Task("t0", task_type=0, customer=0, priority=ta.Priority.CRITICAL),
        ta.Task("t1", task_type=1, customer=0, priority=ta.Priority.MAJOR),
        ta.Task("t2", task_type=2, customer=0, priority=ta.Priority.MINOR),
    ]
    return ta.TaskAssigningSolution(types, [ta.Customer("c")], tasks, employees)


def _zero_budget(solution, picks):
    """The engine's score of `solution` under `picks`: a solve with no time keeps it as it is."""
    before = solution.task_lists()
    config = ta.SolverConfig()
    config.spent_limit_seconds = 0
    ta.Solver(config).solve_with_constraints(solution, picks)
    assert solution.task_lists() == before
    return engine(solution.score)


REPEAT = [CriticalPriorityTaskEndTimeCost(), CriticalPriorityTaskEndTimeCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """4 x the critical end times plus 0.5 x their square per employee: 4 x 10 + 0.5 x 10^2 = 90."""
    raw = ta.constraint_costs(_hand(), REPEAT)
    assert list(raw) == ["critical_priority_task_end_time_cost", "critical_priority_task_end_time_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [10.0, 0.0, 0.0]
    assert "units" not in first  # every employee is evaluated
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (4.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "critical_priority_task_end_time_cost"
    assert second["default_weight"] == 4.0
    assert second["medium_fold"] == {"kind": "below_hard", "unit": 1e9}
    expected = (0, -(4 * 10 + 0.5 * 10**2))
    assert expected == (0, -90.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(ta.constraint_costs(_hand()), REPEAT) == expected
    assert _zero_budget(_hand(), REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [MinorPriorityTaskEndTimeCost(), MinorPriorityTaskEndTimeCost()]
    assert score(ta.constraint_costs(_hand()), picks) == (0, -10.0)
    assert _zero_budget(_hand(), picks) == (0, -10.0)
    assert list(ta.constraint_costs(_hand(), picks)) == [
        "minor_priority_task_end_time_cost",
        "minor_priority_task_end_time_cost#2",
    ]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        MinorPriorityTaskEndTimeCost(weight=1),
        UnassignedTasksCost(),
        MinorPriorityTaskEndTimeCost(weight=2),
        RequiredSkillCost(),
        MinorPriorityTaskEndTimeCost(weight=3),
    ]
    raw = ta.constraint_costs(_hand(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "required_skill_cost": 1.0,
        "minor_priority_task_end_time_cost": 1.0,
        "minor_priority_task_end_time_cost#2": 2.0,
        "minor_priority_task_end_time_cost#3": 3.0,
        "unassigned_tasks_cost": 1.0,
    }
    assert [key for key in raw if key.startswith("minor")] == [
        "minor_priority_task_end_time_cost",
        "minor_priority_task_end_time_cost#2",
        "minor_priority_task_end_time_cost#3",
    ]
    expected = (-MEDIUM_PER_HARD, -30.0)
    assert score(raw, picks) == expected
    assert _zero_budget(_hand(), picks) == expected


def test_a_repeated_medium_pick_adds_up():
    """Both instances stay on the medium level (folded below hard); a promoted repeat goes hard.

    No employees: every demo task stays unassigned, so the engine scores that state."""
    picks = [UnassignedTasksCost(), UnassignedTasksCost(weight=2)]
    tasks = len(_nobody().tasks)
    assert score(ta.constraint_costs(_nobody()), picks) == (-3 * tasks, 0.0)
    solution = _nobody()
    ta.solve(solution, time_limit_seconds=0.1, constraints=picks)
    assert (solution.score.hard, solution.score.soft0) == (0, -3 * tasks)
    assert engine(solution.score) == (-3 * tasks, 0.0)
    mixed = [UnassignedTasksCost(), UnassignedTasksCost(hard=True), UnassignedTasksCost(hard=False, offset=-1)]
    expected = (-tasks * MEDIUM_PER_HARD - tasks, -(tasks + 1.0))
    assert score(ta.constraint_costs(_nobody(), mixed), mixed) == expected
    solution = _nobody()
    ta.solve(solution, time_limit_seconds=0.1, constraints=mixed)
    assert (solution.score.hard, solution.score.soft0) == (-tasks, -tasks)
    assert engine(solution.score) == expected


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per employee: e2 is idle (raw 0, evaluated, so charged in literal mode).
        (CriticalPriorityTaskEndTimeCost(offset=4), (0, -4 * 6)),
        (CriticalPriorityTaskEndTimeCost(offset=4, clip=False), (0, -4 * (6 - 4 - 4))),  # e1, e2 are rewarded
        (CriticalPriorityTaskEndTimeCost(offset=20), (0, 0.0)),
        (CriticalPriorityTaskEndTimeCost(offset=20, clip=False), (0, -4 * (-10 - 20 - 20))),
        (MajorPriorityTaskEndTimeCost(offset=-1), (0, -2 * 31)),  # a negative offset adds to every non-zero unit
        (MajorPriorityTaskEndTimeCost(offset=-1, clip=False), (0, -2 * (31 + 1 + 1))),  # ...or to every unit
        (MinorPriorityTaskEndTimeCost(offset=2, exponent=2), (0, -(3**2))),
        (MinorPriorityTaskEndTimeCost(offset=3, exponent=3, clip=False), (0, -(-(3**3) + 2**3 - 3**3))),
        (SquaredMakespanCost(offset=100), (0, -SOFT1_WEIGHT * 800)),
        # Hard picks fold by 10^9 below hard; hard rounding is per employee.
        (RequiredSkillCost(offset=2, clip=False), (5 * MEDIUM_PER_HARD, 0.0)),  # (1-2) + (0-2) x 2
        (RequiredSkillCost(weight=3, offset=0.5, clip=False), (2 * MEDIUM_PER_HARD, 0.0)),  # 1.5 -> 2, -1.5 -> -2 x 2
        # The global's total (0 here) is shaped once.
        (UnassignedTasksCost(offset=3, clip=False), (3, 0.0)),
        (UnassignedTasksCost(offset=3), (0, 0.0)),
        (UnassignedTasksCost(offset=3, clip=False, hard=True), (3 * MEDIUM_PER_HARD, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(ta.constraint_costs(_hand()), [pick]) == expected
    raw = ta.constraint_costs(_hand(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget(_hand(), [pick]) == expected


@pytest.mark.parametrize(
    ("make_pick", "expected"),
    [
        (lambda tasks: UnassignedTasksCost(offset=2, exponent=2), lambda tasks: (-((tasks - 2) ** 2), 0.0)),
        (lambda tasks: UnassignedTasksCost(offset=tasks + 5, clip=False), lambda tasks: (5, 0.0)),
    ],
    ids=["threshold-squared", "literal"],
)
def test_the_global_total_is_shaped_once(make_pick, expected):
    """Every demo task unassigned: the total, not each task, is shaped ((N - 2)^2, not N x 0)."""
    tasks = len(_nobody().tasks)
    pick = make_pick(tasks)
    assert score(ta.constraint_costs(_nobody(), [pick]), [pick]) == expected(tasks)
    solution = _nobody()
    ta.solve(solution, time_limit_seconds=0.1, constraints=[pick])
    assert engine(solution.score) == expected(tasks)


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*get_constraints()[:-1], UnassignedTasksCost(exponent=2)]
    shifted = [type(spec)(exponent=spec.exponent, offset=0.0, clip=False) for spec in plain]
    raw, solved = _raw()
    assert score(raw, shifted) == score(raw, plain)
    engines = [_zero_budget(solved, picks) for picks in (plain, shifted)]
    assert engines[0] == engines[1]
    assert engines[0][0] == score(raw, plain)[0]
    assert engines[0][1] == pytest.approx(score(raw, plain)[1])
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


SELECTIONS = {
    "repeat": [*get_constraints(), *REPEAT, UnassignedTasksCost(weight=2, exponent=2)],
    "repeat-and-offsets": [
        RequiredSkillCost(),
        RequiredSkillCost(weight=1, offset=1, clip=False),
        SquaredMakespanCost(),
        SquaredMakespanCost(weight=0.5, offset=1000000, clip=False),
        CriticalPriorityTaskEndTimeCost(offset=30),
        MajorPriorityTaskEndTimeCost(offset=-5, clip=False, hard=True),
        MinorPriorityTaskEndTimeCost(exponent=2, offset=10),
        UnassignedTasksCost(),
        UnassignedTasksCost(offset=-2, hard=False, weight=3),
    ],
}


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_score_matches_the_engine_for_repeats_and_offsets(picks):
    raw, solved = _raw()
    expected = _zero_budget(solved, picks)
    python = score(ta.constraint_costs(solved, picks), picks)
    assert python.hard == expected[0]
    assert python.soft == pytest.approx(expected[1])
    assert score(raw, picks) == python  # the default costs score the repeats too


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    solution = build_solution()
    config = ta.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 2000
    config.spent_limit_seconds = 30
    ta.Solver(config).solve_with_constraints(solution, picks)
    raw = ta.constraint_costs(solution, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    expected = engine(solution.score)
    assert python.hard == expected[0]
    assert python.soft == pytest.approx(expected[1])
    assert score(ta.constraint_costs(solution), picks) == python
