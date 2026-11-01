from datetime import date

import pytest

import maintenance_scheduling as domain
from maintenance_scheduling.constraints import (
    AfterIdealEndDateCost,
    MaxEndDateCost,
    get_constraints,
    score,
)
from maintenance_scheduling.models import CrewModel, JobModel, MaintenanceScheduleModel
from maintenance_scheduling_cpsat import catalog, solve
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


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """No hand-copied defaults: the CP-SAT catalog equals constraint_info().

    Before 0.2.0 the plugins restated after_ideal_end_date (1'000'000) and
    tag_conflict (1'000) as ClassVars; now both solvers read the C++ constants."""
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
        assert entry["kind"] == "either"  # every maintenance constraint is flippable
        assert entry["params"] == {"weight": None, "level": None, "exponent": 1.0}
    assert {
        name: (row.default_level, row.default_weight) for name, row in engine.items()
    } == {
        "min_start_date_cost": ("hard", 1.0),
        "max_end_date_cost": ("hard", 1.0),
        "before_ideal_end_date_cost": ("soft", 1.0),
        "after_ideal_end_date_cost": ("soft", 1_000_000.0),
        "crew_conflict_cost": ("hard", 1.0),
        "tag_conflict_cost": ("soft", 1_000.0),
    }


def test_default_solve_scores_like_the_engine() -> None:
    """The default selection: CP-SAT's reported score is the engine's score of the same
    solution (the after_ideal_end_date_cost 1'000'000 weight comes from C++ on both)."""
    problem = _parity_problem()
    solved = solve(problem, time_limit_seconds=5)
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)
    engine = score(domain.constraint_costs(solved.to_core()))
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_demoting_max_end_date_scores_like_the_engine() -> None:
    """level="soft" lands on soft in both solvers.

    Job a may not start before Wed and must end by Wed, which its 3 days cannot meet,
    so max_end_date_cost (or min_start_date_cost) is violated in every solution.
    Demoted to soft, the hard level only keeps what the other hard rules cost, and
    CP-SAT's reported score equals the engine's score of the same solution under the
    same selection."""
    problem = _parity_problem()
    selection = [
        MaxEndDateCost(level="soft") if spec.name == "max_end_date_cost" else spec
        for spec in get_constraints()
    ]
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    raw = domain.constraint_costs(solved.to_core())
    engine = score(raw, selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    assert solved.score_hard == -(
        sum(raw["min_start_date_cost"]["costs"])
        + sum(raw["crew_conflict_cost"]["costs"])
    )


def test_reweighting_after_ideal_end_date_scores_like_the_engine() -> None:
    """An explicit weight replaces the engine default on both paths."""
    problem = _parity_problem()
    selection = [
        AfterIdealEndDateCost(weight=7)
        if spec.name == "after_ideal_end_date_cost"
        else spec
        for spec in get_constraints()
    ]
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
