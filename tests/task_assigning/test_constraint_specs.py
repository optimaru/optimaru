"""optimaru.task_assigning.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the catalog
(`constraint_info()`), the five knobs, repeated picks, and the pure-Python `score()`
against the engine — including the domain's medium level (`unassigned_tasks_cost`,
folded below hard).
"""

from types import SimpleNamespace

import pytest

import optimaru.task_assigning as ta
from optimaru._constraint import instance_keys
from optimaru.task_assigning.constraints import (
    CriticalPriorityTaskEndTimeCost,
    Level,
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
from optimaru.task_assigning.demo import generate_demo_data

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

    Four score levels: hard = missing skills, soft0 = unassigned,
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
    level charges its weight in HARD units (the 10^9 between the hard and the
    medium level is the domain's fold, not a weight), so the penalty moves from soft2
    to the hard level wholesale."""
    picks = [unassigned_tasks_cost]

    soft_level = solve([*picks, critical_priority_task_end_time_cost])
    assert soft_level.soft2 < 0
    assert soft_level.hard == 0 and soft_level.soft0 == 0

    hard_level = solve([*picks, CriticalPriorityTaskEndTimeCost(level="hard")])
    assert hard_level.soft2 == 0
    assert hard_level.hard < 0
    assert hard_level.soft0 == 0


def test_naming_a_constraint_twice_scores_both_picks():
    """A constraint may be picked several times: each pick is scored on its own.

    The later pick neither wins in silence nor is an error: both are
    instances with their own knobs, and they add up."""
    picks = [RequiredSkillCost(), SquaredMakespanCost(weight=10), SquaredMakespanCost()]
    assert _zero_budget(_hand(), picks) == (-MEDIUM_PER_HARD, -(10 + SOFT1_WEIGHT) * (900 + 25))
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.2, random_seed=1, constraints=picks)
    python = score(ta.constraint_costs(solution, picks), picks)
    assert python.hard == engine(solution.score)[0]
    assert python.soft == pytest.approx(engine(solution.score)[1])
    assert get_constraints([RequiredSkillCost(), RequiredSkillCost()]) == [RequiredSkillCost(), RequiredSkillCost()]


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


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint,
    and the domain's below-hard medium fold on EVERY entry."""
    raw = ta.constraint_costs(build_solution())
    for row in ta.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (
            row.default_weight,
            row.default_level,
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
        [
            RequiredSkillCost(exponent=2.0),
            MajorPriorityTaskEndTimeCost(level="hard", weight=0.5),
            unassigned_tasks_cost,
        ],
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


# --- repeated picks and the offset / clip knobs -----------------------------------
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
    mixed = [UnassignedTasksCost(), UnassignedTasksCost(level="hard"), UnassignedTasksCost(level="soft", offset=-1)]
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
        # Hard picks fold by 10^9 below hard; hard rounding is per employee. Hard and medium
        # (it folds below hard here) refuse the literal formula with an offset, so its
        # numbers are checked on soft: (1 - 2) + (0 - 2) x 2, the idle employee included.
        (RequiredSkillCost(weight=3, offset=0.5), (-2 * MEDIUM_PER_HARD, 0.0)),  # 1.5 -> 2 on e0
        (RequiredSkillCost(level="soft", offset=2, clip=False), (0, 5.0)),
        (RequiredSkillCost(level="soft", weight=2, offset=0.5, clip=False), (0, 1.0)),  # 1 - 1 - 1, not rounded
        # The global's total (0 here) is shaped once.
        (UnassignedTasksCost(level="soft", offset=3, clip=False), (0, 3.0)),
        (UnassignedTasksCost(offset=3), (0, 0.0)),
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
        # The literal formula with an offset stays off the below-hard medium: on soft.
        (lambda tasks: UnassignedTasksCost(level="soft", offset=tasks + 5, clip=False), lambda tasks: (0, 5.0)),
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


def test_offset_zero_is_the_plain_shaping():
    """offset 0 is the plain shaping in both modes: a pick with offset 0 scores like one without it."""
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
        RequiredSkillCost(level="soft", weight=1, offset=1, clip=False),  # hard refuses the literal formula
        RequiredSkillCost(weight=1, offset=1),
        SquaredMakespanCost(),
        SquaredMakespanCost(weight=0.5, offset=1000000, clip=False),
        CriticalPriorityTaskEndTimeCost(offset=30),
        MajorPriorityTaskEndTimeCost(offset=-5, level="hard"),
        MajorPriorityTaskEndTimeCost(offset=-5, clip=False),
        MinorPriorityTaskEndTimeCost(exponent=2, offset=10),
        UnassignedTasksCost(),
        UnassignedTasksCost(offset=-2, level="soft", weight=3),
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
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
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


# --- Levels: each pick, a repeat included, on the level it names --------------------


def test_a_soft_constraint_can_move_to_medium():
    """On medium the critical end times land on soft0 (below hard, above every soft term),
    and score() folds them below hard like the engine."""
    picks = [unassigned_tasks_cost, CriticalPriorityTaskEndTimeCost(level=Level.MEDIUM)]
    solution = build_solution()
    ta.solve(solution, time_limit_seconds=0.6, random_seed=1, constraints=picks)
    levels = solution.score
    assert levels.hard == 0 and levels.soft2 == 0
    assert levels.soft0 < 0  # every task is assigned, so this is the critical end times alone
    assert score(ta.constraint_costs(solution), picks) == engine(levels)
    assert score(ta.constraint_costs(solution, picks), picks) == engine(levels)


def test_core_refuses_an_unknown_level_like_the_python_base():
    pick = SimpleNamespace(name="required_skill_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        ta.constraint_costs(_hand(), [pick])


def test_a_pick_without_a_level_is_refused():
    """A duck-typed pick without `level` is refused on every path instead of running at the
    default level."""
    pick = SimpleNamespace(name="required_skill_cost", weight=None, exponent=1.0)
    with pytest.raises(AttributeError, match="level"):
        ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(AttributeError, match="level"):
        ta.constraint_costs(_hand(), [pick])
    with pytest.raises(AttributeError, match="level"):
        score(ta.constraint_costs(_hand()), [pick])


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        # _hand(): required_skill [1, 0, 0], squared_makespan [900, 25, 0], critical [10, 0, 0],
        # minor [0, 5, 0], unassigned [0]. Medium folds BELOW hard: engine.hard = hard * 10^9 + medium.
        ([CriticalPriorityTaskEndTimeCost(level="medium")], (-40, 0.0)),  # weight 4 in medium units
        ([CriticalPriorityTaskEndTimeCost(level=Level.HARD)], (-40 * MEDIUM_PER_HARD, 0.0)),
        ([SquaredMakespanCost(level="medium", weight=1)], (-925, 0.0)),
        ([MinorPriorityTaskEndTimeCost(level=Level.MEDIUM, weight=0.5)], (-3, 0.0)),  # 2.5 -> 3, rounded per unit
        ([RequiredSkillCost(level="soft", weight=2)], (0, -2.0)),
        ([UnassignedTasksCost(level=Level.SOFT, offset=1, clip=False)], (0, 1.0)),
        # A repeat carries its own level and shaping: the soft critical end times, 4 x the part
        # beyond 6 on medium, hard the part beyond 8 per employee (2), and soft (x - 12) per
        # employee: -2 - 12 - 12 = -26, a reward (hard and medium refuse the literal formula
        # with an offset).
        (
            [
                CriticalPriorityTaskEndTimeCost(),
                CriticalPriorityTaskEndTimeCost(level="medium", offset=6),
                CriticalPriorityTaskEndTimeCost(level="hard", weight=1, offset=8),
                CriticalPriorityTaskEndTimeCost(level="soft", weight=1, offset=12, clip=False),
            ],
            (-2 * MEDIUM_PER_HARD - 16, -40.0 + 26),
        ),
        # The medium global repeated onto all three levels, each total (0) shaped once: a
        # threshold adds nothing to it on hard, the literal formula charges it on soft.
        (
            [
                UnassignedTasksCost(),
                UnassignedTasksCost(level=Level.HARD, offset=2),
                UnassignedTasksCost(level=Level.SOFT, weight=3, offset=-1, clip=False),
            ],
            (0, -3.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(ta.constraint_costs(_hand()), picks) == expected
    raw = ta.constraint_costs(_hand(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(_hand(), picks) == expected


def test_a_repeat_on_medium_reports_its_level_unfolded():
    """solution.score reports each score level on its own: the repeat's medium term is soft0."""
    picks = [MinorPriorityTaskEndTimeCost(), MinorPriorityTaskEndTimeCost(level="medium")]
    solution = _hand()
    _zero_budget(solution, picks)
    assert (solution.score.hard, solution.score.soft0, solution.score.soft1, solution.score.soft2) == (0, -5, 0, -5)


def test_a_repeat_is_checked_on_its_own_level():
    """The second pick's level is read and checked like the first's, on both paths."""
    pick = SimpleNamespace(name="required_skill_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="constraint 'required_skill_cost': level must be 'hard', 'medium' or 'soft'"):
        _zero_budget(_hand(), [RequiredSkillCost(), pick])
    with pytest.raises(ValueError, match="constraint 'required_skill_cost': level must be 'hard', 'medium' or 'soft'"):
        score(ta.constraint_costs(_hand()), [RequiredSkillCost(), pick])


# --- clip=False with an offset stays off the hard score ---------------------------
# Medium folds BELOW hard here (engine.hard = hard * 10^9 + medium), so medium counts
# toward the hard score and refuses the literal formula too, with its own message.

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)
MEDIUM_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be medium here, where medium counts "
    "toward the hard score; use clip=True (a threshold) or level='soft'"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [2.5, -2.5])
@pytest.mark.parametrize(
    ("pick", "refusal"),
    [
        (lambda offset: RequiredSkillCost(offset=offset, clip=False), HARD_REFUSAL),  # the default, hard
        (lambda offset: RequiredSkillCost(level="hard", offset=offset, clip=False), HARD_REFUSAL),
        (lambda offset: UnassignedTasksCost(level=Level.HARD, offset=offset, clip=False), HARD_REFUSAL),
        (lambda offset: MinorPriorityTaskEndTimeCost(level="hard", offset=offset, clip=False), HARD_REFUSAL),
        (lambda offset: UnassignedTasksCost(offset=offset, clip=False), MEDIUM_REFUSAL),  # the default, medium
        (lambda offset: UnassignedTasksCost(level="medium", offset=offset, clip=False), MEDIUM_REFUSAL),
        (lambda offset: CriticalPriorityTaskEndTimeCost(level="medium", offset=offset, clip=False), MEDIUM_REFUSAL),
        (lambda offset: SquaredMakespanCost(level=Level.MEDIUM, offset=offset, clip=False), MEDIUM_REFUSAL),
    ],
    ids=[
        "required-skill-default-hard",
        "required-skill-level-hard",
        "unassigned-moved-to-hard",
        "minor-moved-to-hard",
        "unassigned-default-medium",
        "unassigned-level-medium",
        "critical-moved-to-medium",
        "makespan-moved-to-medium",
    ],
)
def test_the_literal_offset_is_refused_where_it_counts_toward_hard(pick, refusal, offset):
    """A positive offset would reward the employees below it, so one employee's slack would
    cancel another's missing skill (or a left-out task) and an infeasible plan could score
    hard 0; a negative one would charge every employee, an idle one included. The engine
    (solve, solve_with_constraints, constraint_costs) and the score() mirror refuse it with
    the same message; the CP-SAT reference's test_solve.py checks the same words."""
    spec = pick(offset)
    message = refusal.format(q=repr(spec.name))
    repeated = [type(spec)(), spec]  # a refused repeat next to its accepted first instance
    assert _refusal(lambda: ta.solve(build_solution(), time_limit_seconds=0.1, constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget(_hand(), repeated)) == message
    assert _refusal(lambda: ta.constraint_costs(_hand(), [spec])) == message
    assert _refusal(lambda: ta.constraint_costs(_hand(), repeated)) == message
    assert _refusal(lambda: score(ta.constraint_costs(_hand()), [spec])) == message
    assert _refusal(lambda: score(ta.constraint_costs(_hand()), repeated)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """On soft: (1 - offset) + (0 - offset) x 2 per employee, the idle one included, and
    (0 - offset) on the unassigned total, on every path."""
    picks = [
        RequiredSkillCost(level="soft", offset=offset, clip=False),
        UnassignedTasksCost(level="soft", offset=offset, clip=False),
    ]
    expected = (0, 3 * offset - 1 + offset)
    assert score(ta.constraint_costs(_hand()), picks) == expected
    raw = ta.constraint_costs(_hand(), picks)
    assert [(entry["level"], entry["clip"]) for entry in raw.values()] == [("soft", False)] * 2
    assert score(raw, picks) == expected
    assert _zero_budget(_hand(), picks) == expected
    solution = build_solution()
    full = [RequiredSkillCost(), UnassignedTasksCost(), *picks]
    ta.solve(solution, time_limit_seconds=0.2, random_seed=1, constraints=full)
    python = score(ta.constraint_costs(solution), full)
    assert python.hard == engine(solution.score)[0]
    assert python.soft == pytest.approx(engine(solution.score)[1])


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # _hand(): required_skill [1, 0, 0] on hard, critical [10, 0, 0] moved to medium.
        (RequiredSkillCost(offset=0.5), (-MEDIUM_PER_HARD, 0.0)),  # clip=True on hard: 0.5 -> 1
        (RequiredSkillCost(offset=-1), (-2 * MEDIUM_PER_HARD, 0.0)),  # adds 1 to the non-zero employee only
        (RequiredSkillCost(clip=False), (-MEDIUM_PER_HARD, 0.0)),  # clip=False with offset 0: the plain shaping
        (RequiredSkillCost(offset=0.0, clip=False), (-MEDIUM_PER_HARD, 0.0)),
        (CriticalPriorityTaskEndTimeCost(level="medium", offset=4), (-24, 0.0)),  # a threshold on medium
        (CriticalPriorityTaskEndTimeCost(level="medium", clip=False), (-40, 0.0)),
        (UnassignedTasksCost(offset=-1), (0, 0.0)),  # nothing left out: a threshold adds nothing to 0
    ],
)
def test_the_allowed_combinations_on_hard_and_medium(pick, expected):
    assert score(ta.constraint_costs(_hand()), [pick]) == expected
    assert _zero_budget(_hand(), [pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(
        name="unassigned_tasks_cost", weight=None, level="soft", exponent=1.0, offset=1.0, clip=False
    )
    assert _zero_budget(_hand(), [moved]) == (0, 1.0)
    kept = SimpleNamespace(name="unassigned_tasks_cost", weight=None, level=None, exponent=1.0, offset=1.0, clip=False)
    message = MEDIUM_REFUSAL.format(q="'unassigned_tasks_cost'")
    assert _refusal(lambda: _zero_budget(_hand(), [kept])) == message
    assert _refusal(lambda: ta.constraint_costs(_hand(), [kept])) == message
    assert _refusal(lambda: score(ta.constraint_costs(_hand()), [kept])) == message
    hard = SimpleNamespace(name="required_skill_cost", weight=None, level=None, exponent=1.0, offset=1.0, clip=False)
    message = HARD_REFUSAL.format(q="'required_skill_cost'")
    assert _refusal(lambda: _zero_budget(_hand(), [hard])) == message
    assert _refusal(lambda: score(ta.constraint_costs(_hand()), [hard])) == message
