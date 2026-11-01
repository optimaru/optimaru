import pytest

import task_assigning as domain
from task_assigning.constraints import (
    CriticalPriorityTaskEndTimeCost,
    Level,
    MajorPriorityTaskEndTimeCost,
    MinorPriorityTaskEndTimeCost,
    RequiredSkillCost,
    SquaredMakespanCost,
    UnassignedTasksCost,
    get_constraints,
    score,
)
from task_assigning.demo import generate_demo_data
from task_assigning.models import (
    CustomerModel,
    EmployeeModel,
    TaskAssigningSolutionModel,
    TaskModel,
    TaskTypeModel,
)
from task_assigning import calculate_score
from task_assigning_cpsat import catalog, solve
from task_assigning_cpsat.solve import MEDIUM_FOLD


def test_smoke_and_plugin_catalog():
    problem = generate_demo_data()
    result = solve(problem, time_limit_seconds=2.0)
    assert len(
        {task for employee in result.employees for task in employee.tasks}
    ) == len(problem.tasks)
    assert result.score_hard == 0
    assert {entry["name"] for entry in catalog()} >= {
        "required_skill_cost",
        "unassigned_tasks_cost",
    }
    shared = calculate_score(result.to_core())
    assert (
        result.score_hard,
        result.score_soft0,
        result.score_soft1,
        result.score_soft2,
    ) == (shared.hard, shared.soft0, shared.soft1, shared.soft2)


def test_explicit_constraint_list_replaces_defaults():
    result = solve(
        generate_demo_data(),
        time_limit_seconds=2.0,
        constraints=["unassigned_tasks_cost"],
    )
    assert result.score_hard == 0
    assert result.score_soft1 == 0
    assert result.score_soft2 == 0


def _parity_problem() -> TaskAssigningSolutionModel:
    return TaskAssigningSolutionModel(
        task_types=[
            TaskTypeModel(code="T", base_duration=10, required_skills=["required"])
        ],
        customers=[CustomerModel(id="C")],
        tasks=[
            TaskModel(
                id="critical", task_type_code="T", customer_id="C", priority="CRITICAL"
            ),
            TaskModel(
                id="major", task_type_code="T", customer_id="C", priority="MAJOR"
            ),
            TaskModel(
                id="minor", task_type_code="T", customer_id="C", priority="MINOR"
            ),
            TaskModel(
                id="unassigned",
                task_type_code="T",
                customer_id="C",
                priority="MINOR",
            ),
        ],
        employees=[EmployeeModel(id="E", skills=[], tasks=[])],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_tasks(constraint) -> None:
    problem = _parity_problem()
    employees = [0, 0, 0, -1]
    starts = [0, 40, 80, 0]
    candidate = problem.model_copy(deep=True)
    candidate.employees[0].tasks = ["critical", "major", "minor"]
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_employee_by_task=employees,
        _fixed_starts=starts,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)


def _engine_levels(solved: TaskAssigningSolutionModel) -> tuple[int, float]:
    """The model's four Bendable levels folded the way the engine folds them."""
    return (
        solved.score_hard * 10**9 + solved.score_soft0,
        solved.score_soft1 * 10**7 + solved.score_soft2,
    )


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info()."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == "either"  # every task-assigning constraint is flippable
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    assert [entry["default_level"] for entry in entries] == [
        "hard",
        "soft",
        "soft",
        "soft",
        "soft",
        "medium",
    ]


@pytest.mark.parametrize(
    "selection",
    [
        None,
        [RequiredSkillCost(level="soft"), *get_constraints()[1:]],
        [*get_constraints()[:-1], UnassignedTasksCost(level="soft")],
        [*get_constraints()[:-1], UnassignedTasksCost(level="hard")],
        [
            *get_constraints()[:2],
            CriticalPriorityTaskEndTimeCost(level=Level.MEDIUM),
            *get_constraints()[3:],
        ],
    ],
    ids=[
        "defaults",
        "skills-soft",
        "unassigned-soft",
        "unassigned-hard",
        "critical-medium",
    ],
)
def test_cpsat_scores_like_the_engine(selection) -> None:
    """CP-SAT's reported score is the engine's score of the same solution under the
    same selection; level="soft" lands on soft (from hard OR medium) in both solvers,
    and level="medium" moves a soft term onto medium (below hard) in both."""
    solved = solve(generate_demo_data(), time_limit_seconds=2, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert _engine_levels(solved) == (engine.hard, pytest.approx(engine.soft))


def test_a_demoted_medium_term_counts_on_soft_in_both_solvers() -> None:
    """unassigned_tasks_cost(level="soft") is one SOFT unit per unassigned task in CP-SAT
    and in the engine; the selection without a medium term reports soft0 = 0."""
    problem = _parity_problem()
    selection = [UnassignedTasksCost(level="soft")]
    proxy = {}
    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=selection,
        _fixed_employee_by_task=[0, 0, 0, -1],
        _fixed_starts=[0, 40, 80, 0],
        _proxy_score=proxy,
    )
    assert proxy == {"hard": 0, "medium": 0, "soft": -1}
    assert (
        solved.score_hard,
        solved.score_soft0,
        solved.score_soft1,
        solved.score_soft2,
    ) == (0, 0, 0, -1)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert _engine_levels(solved) == tuple(engine) == (0, -1.0)


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _two_employees() -> TaskAssigningSolutionModel:
    """E0 without skills, E1 with the required one; four 40-minute tasks (NONE affinity)."""
    return TaskAssigningSolutionModel(
        task_types=[
            TaskTypeModel(code="T", base_duration=10, required_skills=["required"])
        ],
        customers=[CustomerModel(id="C")],
        tasks=[
            TaskModel(id=name, task_type_code="T", customer_id="C", priority=priority)
            for name, priority in (
                ("critical", "CRITICAL"),
                ("major", "MAJOR"),
                ("minor", "MINOR"),
                ("minor2", "MINOR"),
            )
        ],
        employees=[
            EmployeeModel(id="E0", skills=[], tasks=[]),
            EmployeeModel(id="E1", skills=["required"], tasks=[]),
        ],
    )


#: Pinned (employee per task, start per task) layouts of _two_employees(), the starts
#: being the engine's chain (each task starts when the previous one on its list ends).
LAYOUTS = [
    ([0, 0, 1, -1], [0, 40, 0, 0]),
    ([1, 1, 1, 1], [0, 40, 80, 120]),
    ([0, 1, 0, 1], [0, 0, 40, 40]),
    ([0, 0, 0, 0], [0, 40, 80, 120]),
    ([-1, -1, -1, -1], [0, 0, 0, 0]),
]

SELECTIONS = {
    "repeat": [
        RequiredSkillCost(),
        RequiredSkillCost(weight=2),
        SquaredMakespanCost(),
        MinorPriorityTaskEndTimeCost(),
        MinorPriorityTaskEndTimeCost(),
        UnassignedTasksCost(),
        UnassignedTasksCost(weight=3),
    ],
    # Hard and medium (it folds below hard) refuse the literal formula with an offset:
    # the literal copies of the hard and medium constraints sit on soft.
    "offsets": [
        RequiredSkillCost(level="soft", offset=1, clip=False),
        SquaredMakespanCost(offset=2000),
        CriticalPriorityTaskEndTimeCost(offset=30, clip=False),
        MajorPriorityTaskEndTimeCost(offset=-5),
        MinorPriorityTaskEndTimeCost(offset=50, clip=False),
        MinorPriorityTaskEndTimeCost(weight=2, offset=60),
        UnassignedTasksCost(level="soft", offset=2, clip=False),
    ],
    "hard-offsets": [
        RequiredSkillCost(weight=2, offset=1),
        RequiredSkillCost(level="soft", weight=2, offset=1, clip=False),
        CriticalPriorityTaskEndTimeCost(level="hard", offset=10),
        UnassignedTasksCost(level="hard", offset=-1),
        UnassignedTasksCost(offset=1),
    ],
    # Each pick on its own level (medium folds below hard): the missing skills on hard
    # and again on medium; the squared makespan beyond 1600 and the critical end times
    # beyond 30 on medium; the minor end times on soft and, beyond 100, on hard; the
    # unassigned tasks on medium and on soft (literal).
    "levels": [
        RequiredSkillCost(),
        RequiredSkillCost(level=Level.MEDIUM, weight=2),
        SquaredMakespanCost(level="medium", weight=1, offset=1600),
        CriticalPriorityTaskEndTimeCost(level="medium", offset=30),
        MinorPriorityTaskEndTimeCost(),
        MinorPriorityTaskEndTimeCost(level=Level.HARD, offset=100),
        UnassignedTasksCost(),
        UnassignedTasksCost(level="soft", offset=1, clip=False),
    ],
}


def _engine_solve(core, selection) -> tuple[int, float]:
    """The engine's own score of `core` under `selection`: a solve with no time keeps
    every task where it is (construction would place an unassigned one, so only call
    this on a layout that assigns every task)."""
    kept = core.task_lists()
    config = domain.SolverConfig()
    config.spent_limit_seconds = 0
    domain.Solver(config).solve_with_constraints(core, selection)
    assert core.task_lists() == kept
    levels = core.score
    return (levels.hard * 10**9 + levels.soft0, levels.soft1 * 10**7 + levels.soft2)


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_pinned_layouts(selection) -> None:
    """Pin each layout: the CP-SAT objective's per-level value, folded like the engine
    (hard * 10^9 + medium), is the engine's score of that solution under the same
    repeated / shifted picks (the score() mirror's, and the engine's own wherever every
    task is assigned), exactly (per employee terms, an idle one included, and one term
    for the global's total)."""
    problem = _two_employees()
    for employees, starts in LAYOUTS:
        proxy: dict[str, int] = {}
        solved = solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_employee_by_task=employees,
            _fixed_starts=starts,
            _proxy_score=proxy,
        )
        core = solved.to_core()
        engine = score(domain.constraint_costs(core, selection), selection)
        if -1 not in employees:
            assert _engine_solve(core, selection) == tuple(engine), employees
        folded = (proxy["hard"] * 10**9 + proxy["medium"], proxy["soft"])
        assert folded == tuple(engine), employees
        assert _engine_levels(solved) == tuple(engine)


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    specs = [
        RequiredSkillCost(),
        UnassignedTasksCost(),
        UnassignedTasksCost(level="soft", weight=3, offset=1, clip=False),
        MinorPriorityTaskEndTimeCost(offset=20, clip=False),
    ]
    dicts = [
        {"name": "required_skill_cost"},
        {"name": "unassigned_tasks_cost"},
        {
            "name": "unassigned_tasks_cost",
            "level": "soft",
            "weight": 3,
            "offset": 1,
            "clip": False,
        },
        {"name": "minor_priority_task_end_time_cost", "offset": 20, "clip": False},
    ]
    problem = _two_employees()
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_repeated_and_shifted_picks_solve_like_the_engine() -> None:
    """A free solve under repeats and offsets reports the engine's score of its solution."""
    selection = SELECTIONS["offsets"]
    solved = solve(_two_employees(), time_limit_seconds=5, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert _engine_levels(solved) == tuple(engine)


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _two_employees(),
            time_limit_seconds=1,
            constraints=[UnassignedTasksCost(offset=0.5)],
        )


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------
# Medium folds below hard here, so it counts toward the hard score and refuses the
# literal formula too (the objective is built with medium_fold="below_hard").

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)
MEDIUM_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be medium here, where medium counts "
    "toward the hard score; use clip=True (a threshold) or level='soft'"
)


def test_the_objective_takes_the_engine_medium_fold() -> None:
    assert MEDIUM_FOLD == "below_hard"


@pytest.mark.parametrize("offset", [2, -2])
@pytest.mark.parametrize(
    ("selection", "refusal"),
    [
        (
            lambda offset: RequiredSkillCost(offset=offset, clip=False),
            HARD_REFUSAL,
        ),  # the default, hard
        (
            lambda offset: {
                "name": "required_skill_cost",
                "offset": offset,
                "clip": False,
            },
            HARD_REFUSAL,
        ),
        (
            lambda offset: UnassignedTasksCost(level="hard", offset=offset, clip=False),
            HARD_REFUSAL,
        ),
        (
            lambda offset: UnassignedTasksCost(offset=offset, clip=False),
            MEDIUM_REFUSAL,
        ),  # the default, medium
        (
            lambda offset: {
                "name": "unassigned_tasks_cost",
                "offset": offset,
                "clip": False,
            },
            MEDIUM_REFUSAL,
        ),
        (
            lambda offset: CriticalPriorityTaskEndTimeCost(
                level=Level.MEDIUM, offset=offset, clip=False
            ),
            MEDIUM_REFUSAL,
        ),
        (
            lambda offset: {
                "name": "squared_makespan_cost",
                "level": "medium",
                "offset": offset,
                "clip": False,
            },
            MEDIUM_REFUSAL,
        ),
    ],
    ids=[
        "required-skill-spec",
        "required-skill-mapping",
        "unassigned-moved-to-hard",
        "unassigned-default-medium",
        "unassigned-mapping-medium",
        "critical-moved-to-medium",
        "makespan-mapping-moved-to-medium",
    ],
)
def test_the_literal_offset_is_refused_like_the_engine(
    selection, refusal, offset
) -> None:
    """The CP-SAT reference refuses the pick with the engine's and score()'s message."""
    pick = selection(offset)
    name = pick["name"] if isinstance(pick, dict) else pick.name
    message = refusal.format(q=repr(name))
    for picks in ([pick], [RequiredSkillCost(), UnassignedTasksCost(), pick]):
        with pytest.raises(ValueError) as refused:
            solve(_two_employees(), time_limit_seconds=1, constraints=picks)
        assert str(refused.value) == message
    if not isinstance(pick, dict):
        # The engine and the score() mirror say the same.
        core = _two_employees().to_core()
        with pytest.raises(ValueError) as refused:
            domain.constraint_costs(core, [pick])
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            score(domain.constraint_costs(core), [pick])
        assert str(refused.value) == message


@pytest.mark.parametrize("offset", [2, -2])
def test_the_same_literal_pick_on_soft_is_accepted(offset) -> None:
    """On soft the literal picks solve and report the engine's score of the solution."""
    selection = [
        RequiredSkillCost(),
        UnassignedTasksCost(),
        RequiredSkillCost(level="soft", offset=offset, clip=False),
        UnassignedTasksCost(level="soft", offset=offset, clip=False),
    ]
    solved = solve(_two_employees(), time_limit_seconds=5, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert _engine_levels(solved) == tuple(engine)


@pytest.mark.parametrize(
    ("pick", "refusal"),
    [
        (RequiredSkillCost(exponent=2, offset=2, clip=False), HARD_REFUSAL),
        (
            UnassignedTasksCost(exponent=2, offset=2, clip=False),
            MEDIUM_REFUSAL,
        ),  # below_hard medium
        (
            {"name": "required_skill_cost", "exponent": 2, "offset": 1, "clip": False},
            HARD_REFUSAL,
        ),
    ],
    ids=["hard", "medium", "mapping"],
)
def test_the_engine_rules_come_before_the_cpsat_exponent_limit(pick, refusal) -> None:
    """A pick the engine refuses gets the engine's words from CP-SAT too, not "exponent is not
    supported by CP-SAT", which comes after them (and across picks)."""
    name = pick["name"] if isinstance(pick, dict) else pick.name
    message = refusal.format(q=repr(name))
    for picks in ([pick], [UnassignedTasksCost(exponent=2), pick]):
        with pytest.raises(ValueError) as refused:
            solve(_two_employees(), time_limit_seconds=1, constraints=picks)
        assert str(refused.value) == message
    if not isinstance(pick, dict):
        core = _two_employees().to_core()
        with pytest.raises(ValueError) as refused:
            domain.constraint_costs(core, [pick])
        assert str(refused.value) == message
    with pytest.raises(ValueError, match="exponent is not supported by CP-SAT"):
        solve(
            _two_employees(),
            time_limit_seconds=1,
            constraints=[UnassignedTasksCost(exponent=2)],
        )


def test_an_offset_that_is_zero_as_a_double_is_accepted_like_the_engine() -> None:
    from fractions import Fraction

    pick = RequiredSkillCost(offset=Fraction(1, 10**400), clip=False)  # 0.0 as a double
    core = _two_employees().to_core()
    assert score(domain.constraint_costs(core), [pick]) == score(
        domain.constraint_costs(core, [pick]), [pick]
    )
    solve(
        _two_employees(),
        time_limit_seconds=1,
        constraints=[RequiredSkillCost(), UnassignedTasksCost(), pick],
    )
