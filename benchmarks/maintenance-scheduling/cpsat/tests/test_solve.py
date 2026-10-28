from datetime import date

import pytest

import maintenance_scheduling as domain
from maintenance_scheduling.constraints import get_constraints
from maintenance_scheduling.models import CrewModel, JobModel, MaintenanceScheduleModel
from maintenance_scheduling_cpsat import solve
from maintenance_scheduling import calculate_score


def test_avoids_crew_overlap() -> None:
    problem = MaintenanceScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 1, 12),
        crews=[CrewModel(id="crew", name="Crew")],
        jobs=[
            JobModel(id="a", name="A", duration_in_days=2),
            JobModel(id="b", name="B", duration_in_days=2),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert (
        solved.jobs[0].end_date <= solved.jobs[1].start_date
        or solved.jobs[1].end_date <= solved.jobs[0].start_date
    )
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> MaintenanceScheduleModel:
    return MaintenanceScheduleModel(
        from_date=date(2026, 1, 5),
        to_date=date(2026, 1, 12),
        crews=[CrewModel(id="c0", name="Crew 0"), CrewModel(id="c1", name="Crew 1")],
        jobs=[
            JobModel(
                id="a",
                name="A",
                duration_in_days=3,
                min_start_date=date(2026, 1, 7),
                max_end_date=date(2026, 1, 7),
                ideal_end_date=date(2026, 1, 6),
                tags=["downtown"],
            ),
            JobModel(
                id="b",
                name="B",
                duration_in_days=1,
                ideal_end_date=date(2026, 1, 9),
                tags=["downtown", "crane"],
            ),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_jobs(constraint) -> None:
    problem = _parity_problem()
    fixed = [(0, 0), (0, 0)]
    candidate = problem.model_copy(deep=True)
    for job, (crew, _day) in zip(candidate.jobs, fixed, strict=True):
        job.crew_id = candidate.crews[crew].id
        job.start_date = candidate.from_date
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_options=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)
