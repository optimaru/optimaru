import pytest

import project_job as domain
from project_job.constraints import (
    MakespanCost,
    NonRenewableResourceCapacityCost,
    ProjectDelayCost,
    get_constraints,
    non_renewable_resource_capacity_cost,
    project_delay_cost,
    renewable_resource_capacity_cost,
    score,
)
from project_job.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)
from project_job_cpsat import catalog, solve
from project_job import calculate_score

MEDIUM_UNIT = 1_000_000_000  # the engine domain's medium fold (into soft)


def test_precedence_chain_sets_shadow_dates() -> None:
    problem = ProjectJobScheduleModel(
        projects=[ProjectModel(id="p", critical_path_duration=2)],
        resources=[],
        jobs=[
            JobModel(
                id="a",
                project_id="p",
                execution_modes=[ExecutionModeModel(id="m", duration=2)],
                successor_job_ids=["b"],
            ),
            JobModel(
                id="b",
                project_id="p",
                job_type="SINK",
                execution_modes=[ExecutionModeModel(id="m", duration=0)],
            ),
        ],
        allocations=[
            AllocationModel(id="aa", job_id="a"),
            AllocationModel(id="ab", job_id="b"),
        ],
        delay_range_size=2,
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert solved.allocations[0].start_date == 0
    assert solved.allocations[0].end_date == solved.allocations[1].start_date == 2
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_medium, solved.score_soft) == (
        shared.hard,
        shared.medium,
        shared.soft,
    )


def _parity_problem() -> ProjectJobScheduleModel:
    resources = [
        ResourceModel(id="renew", capacity=10, renewable=True, project_id="p"),
        ResourceModel(id="non", capacity=10, renewable=False, project_id="p"),
    ]

    def job(identifier, renew, non):
        return JobModel(
            id=identifier,
            project_id="p",
            job_type="SINK",
            execution_modes=[
                ExecutionModeModel(
                    id="m",
                    duration=10,
                    resource_requirements=[
                        ResourceRequirementModel(
                            resource_id="renew", requirement=renew
                        ),
                        ResourceRequirementModel(resource_id="non", requirement=non),
                    ],
                )
            ],
        )

    jobs = [job("a", 7, 7), job("b", 5, 6)]
    return ProjectJobScheduleModel(
        projects=[ProjectModel(id="p", critical_path_duration=100)],
        resources=resources,
        jobs=jobs,
        allocations=[
            AllocationModel(id="aa", job_id="a"),
            AllocationModel(id="ab", job_id="b"),
        ],
        delay_range_size=102,
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_allocations(constraint) -> None:
    problem = _parity_problem()
    modes = [0, 0]
    delays = [100, 101]
    candidate = problem.model_copy(deep=True)
    for allocation, delay in zip(candidate.allocations, delays, strict=True):
        allocation.execution_mode_id = "m"
        allocation.delay = delay
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_modes=modes,
        _fixed_delays=delays,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)


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
        assert entry["kind"] == "either"  # every project-job constraint is flippable
        assert entry["params"] == {"weight": None, "hard": None, "exponent": 1.0}
    # The medium constraint sits on the medium level at weight 1 (medium units).
    assert {name: (row.default_level, row.default_weight) for name, row in engine.items()} == {
        "non_renewable_resource_capacity_cost": ("hard", 1.0),
        "renewable_resource_capacity_cost": ("hard", 1.0),
        "project_delay_cost": ("medium", 1.0),
        "makespan_cost": ("soft", 1.0),
    }


def _fast_or_slow() -> ProjectJobScheduleModel:
    """Two projects, each with one job that runs FAST (needing the single unit of a
    non-renewable stock) or SLOW (needing nothing). P (critical path 0, so its delay is
    its end day): fast 1 day, slow 3. Q (critical path 100, never late): fast 2, slow 5.
    P fast + Q slow: 1 day of delay at makespan 5; P slow + Q fast: 3 days at makespan 3."""
    projects, jobs, allocations = [], [], []
    for project, critical_path, fast, slow in (("P", 0, 1, 3), ("Q", 100, 2, 5)):
        projects.append(ProjectModel(id=project, critical_path_duration=critical_path))
        modes = [
            ExecutionModeModel(
                id=f"{project}-fast",
                duration=fast,
                resource_requirements=[ResourceRequirementModel(resource_id="S", requirement=1)],
            ),
            ExecutionModeModel(id=f"{project}-slow", duration=slow),
        ]
        jobs.append(
            JobModel(
                id=f"{project}-job",
                project_id=project,
                execution_modes=modes,
                successor_job_ids=[f"{project}-end"],
            )
        )
        jobs.append(
            JobModel(
                id=f"{project}-end",
                project_id=project,
                job_type="SINK",
                execution_modes=[ExecutionModeModel(id=f"{project}-end-mode", duration=0)],
            )
        )
        allocations.append(AllocationModel(id=f"{project}-job", job_id=f"{project}-job"))
        allocations.append(
            AllocationModel(
                id=f"{project}-end",
                job_id=f"{project}-end",
                execution_mode_id=f"{project}-end-mode",
                delay=0,
            )
        )
    return ProjectJobScheduleModel(
        projects=projects,
        resources=[ResourceModel(id="S", capacity=1, renewable=False, project_id="P")],
        jobs=jobs,
        allocations=allocations,
        delay_range_size=3,
    )


_CAPACITY = [non_renewable_resource_capacity_cost, renewable_resource_capacity_cost]


@pytest.mark.parametrize(
    ("selection", "expected"),
    [
        # Default (medium): the project delay dominates a doubled makespan.
        ([*_CAPACITY, project_delay_cost, MakespanCost(weight=2)], (0, -1, -10)),
        # hard=False demotes medium to SOFT on both paths (before 0.2.0 CP-SAT kept it
        # on medium and the engine ignored it): 3 + 2 * 3 = 9 beats 1 + 2 * 5 = 11.
        ([*_CAPACITY, ProjectDelayCost(hard=False), MakespanCost(weight=2)], (0, 0, -9)),
        # hard=True: one hard unit per day of delay, not 10^9.
        ([*_CAPACITY, ProjectDelayCost(hard=True), MakespanCost(weight=2)], (-1, 0, -10)),
    ],
    ids=["default-medium", "medium-demoted", "medium-promoted"],
)
def test_medium_knobs_score_like_the_engine(selection, expected) -> None:
    problem = _fast_or_slow()
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    cpsat = (solved.score_hard, solved.score_medium, solved.score_soft)
    core = problem.to_core()
    domain.solve(core, time_limit_seconds=0.3, random_seed=1, constraints=selection)
    engine = (core.score.hard, core.score.medium, core.score.soft)
    assert cpsat == expected
    assert engine == expected
    folded = score(domain.constraint_costs(solved.to_core()), selection)
    assert (cpsat[0], cpsat[1] * MEDIUM_UNIT + cpsat[2]) == pytest.approx(tuple(folded))


def test_demoting_a_hard_constraint_scores_like_the_engine() -> None:
    """hard=False on a hard constraint lands on soft in both solvers too: one unit over
    the stock (1 soft) lets both jobs run fast (delay 1, makespan 2)."""
    problem = _fast_or_slow()
    selection = [NonRenewableResourceCapacityCost(hard=False), *get_constraints()[1:]]
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_medium, solved.score_soft) == (0, -1, -3)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_medium * MEDIUM_UNIT + solved.score_soft) == pytest.approx(
        tuple(engine)
    )
