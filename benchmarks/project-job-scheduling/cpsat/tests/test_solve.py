import pytest

import project_job as domain
from project_job.constraints import get_constraints
from project_job.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)
from project_job_cpsat import solve
from project_job import calculate_score


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
