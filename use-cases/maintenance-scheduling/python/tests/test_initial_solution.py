"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

from datetime import date, timedelta

import pytest

pytest.importorskip("pydantic")

import maintenance_scheduling as mnt
from maintenance_scheduling.models import MaintenanceScheduleModel, epoch_to_date


def _problem():
    return MaintenanceScheduleModel.from_core(mnt.generate_demo_data("SMALL"))


def _plan(model):
    return {job.id: (job.crew_id, job.start_date) for job in model.jobs}


def _score(model):
    return (model.score_hard, model.score_soft)


def _solve(model, seconds):
    core = model.to_core()
    mnt.solve(core, time_limit_seconds=seconds, random_seed=1)
    return MaintenanceScheduleModel.from_core(core)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return _solve(model, 0)


def _non_greedy_seed(problem):
    """Every job one crew and one work day along from where construction puts it."""
    seed = _construct(problem)
    crew_ids = [crew.id for crew in problem.crews]
    work_days = [epoch_to_date(day) for day in problem.to_core().start_dates()]
    for job in seed.jobs:
        job.crew_id = crew_ids[(crew_ids.index(job.crew_id) + 1) % len(crew_ids)]
        job.start_date = work_days[(work_days.index(job.start_date) + 1) % len(work_days)]
    return seed


def _saturday(problem):
    day = problem.from_date
    while day.weekday() != 5:
        day += timedelta(days=1)
    return day


def test_exported_from_the_package():
    assert "with_initial_solution" in mnt.__all__


def test_complete_seed_skips_construction():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    result = _construct(mnt.with_initial_solution(problem, seed))
    assert _plan(result) == _plan(seed)
    assert _plan(result) != _plan(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = _problem()
    solved = _solve(problem, 0.3)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(mnt.with_initial_solution(problem, solution))
    assert _plan(result) == _plan(solved)
    assert _score(result) == _score(solved)
    assert [job.end_date for job in result.jobs] == [job.end_date for job in solved.jobs]


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = _problem()
    seed = _non_greedy_seed(problem)
    solution = seed.model_dump(mode="json")
    solution["jobs"] = solution["jobs"][:6]
    kept = {job["id"] for job in solution["jobs"]}

    seeded = mnt.with_initial_solution(problem, solution)
    assert all(value == (None, None) for job_id, value in _plan(seeded).items() if job_id not in kept)

    result = _construct(seeded)
    assert all(_plan(result)[job_id] == _plan(seed)[job_id] for job_id in kept)
    assert all(None not in value for value in _plan(result).values())


def test_jobs_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(_problem())
    last = solved.jobs[-1]
    solution = {"jobs": [{"id": last.id, "crew_id": last.crew_id, "start_date": last.start_date.isoformat()}]}
    seeded = mnt.with_initial_solution(solved, solution)
    assert _plan(seeded) == {
        job.id: (last.crew_id, last.start_date) if job.id == last.id else (None, None) for job in solved.jobs
    }
    assert all(job.end_date is None for job in seeded.jobs)
    assert _score(seeded) == (None, None)


def test_unknown_job_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-job"):
        mnt.with_initial_solution(_problem(), {"jobs": [{"id": "no-such-job"}]})


@pytest.mark.parametrize("crew_id", ["no-such-crew", 0, ["crew"], True])
def test_unknown_crew_raises_naming_the_job(crew_id):
    problem = _problem()
    job = {"id": problem.jobs[0].id, "crew_id": crew_id, "start_date": problem.from_date.isoformat()}
    with pytest.raises(ValueError, match=problem.jobs[0].id):
        mnt.with_initial_solution(problem, {"jobs": [job]})


@pytest.mark.parametrize("start", ["crew_only", "date_only"])
def test_half_set_job_raises_naming_it(start):
    problem = _problem()
    job = {"id": problem.jobs[0].id}
    if start == "crew_only":
        job["crew_id"] = problem.crews[0].id
    else:
        job["start_date"] = problem.from_date.isoformat()
    with pytest.raises(ValueError, match=problem.jobs[0].id):
        mnt.with_initial_solution(problem, {"jobs": [job]})


def test_half_set_job_on_a_model_raises():
    problem = _problem()
    seed = _construct(problem)
    seed.jobs[2].start_date = None
    with pytest.raises(ValueError, match=seed.jobs[2].id):
        mnt.with_initial_solution(problem, seed)


def test_start_date_outside_the_calendar_raises_naming_the_job():
    problem = _problem()
    crew_id = problem.crews[0].id
    for start in (_saturday(problem), problem.from_date - timedelta(days=7), problem.to_date):
        job = {"id": problem.jobs[0].id, "crew_id": crew_id, "start_date": start.isoformat()}
        with pytest.raises(ValueError, match=problem.jobs[0].id):
            mnt.with_initial_solution(problem, {"jobs": [job]})


@pytest.mark.parametrize("start", ["not-a-date", ["2026-11-02"], "2026-13-40"])
def test_start_date_of_the_wrong_kind_raises_naming_the_job(start):
    problem = _problem()
    job = {"id": problem.jobs[0].id, "crew_id": problem.crews[0].id, "start_date": start}
    with pytest.raises(ValueError, match=problem.jobs[0].id):
        mnt.with_initial_solution(problem, {"jobs": [job]})


def test_repeated_job_raises():
    problem = _problem()
    job_id = problem.jobs[0].id
    with pytest.raises(ValueError, match=job_id):
        mnt.with_initial_solution(problem, {"jobs": [{"id": job_id}, {"id": job_id}]})


def test_repeated_id_on_the_problem_raises():
    problem = _problem()
    problem.jobs[1].id = problem.jobs[0].id
    with pytest.raises(ValueError, match=problem.jobs[0].id):
        mnt.with_initial_solution(problem, {"jobs": []})


@pytest.mark.parametrize("solution", [None, [], {}, {"jobs": "x"}, {"jobs": ["x"]}, {"jobs": [{}]}])
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        mnt.with_initial_solution(_problem(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(_problem())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = mnt.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.jobs[0] is not problem.jobs[0]


def test_pinned_job_keeps_its_value_whatever_the_solution_says():
    problem = _construct(_problem())
    seed = _non_greedy_seed(problem)
    problem.jobs[0].pinned = True
    pinned = (problem.jobs[0].crew_id, problem.jobs[0].start_date)
    assert pinned != (seed.jobs[0].crew_id, seed.jobs[0].start_date)

    seeded = mnt.with_initial_solution(problem, seed)
    assert (seeded.jobs[0].crew_id, seeded.jobs[0].start_date) == pinned
    assert seeded.jobs[0].pinned is True
    result = _solve(seeded, 0.2)
    assert (result.jobs[0].crew_id, result.jobs[0].start_date) == pinned

    garbage = {"jobs": [{"id": problem.jobs[0].id, "crew_id": "no-such-crew", "start_date": date(1900, 1, 1)}]}
    seeded = mnt.with_initial_solution(problem, garbage)
    assert (seeded.jobs[0].crew_id, seeded.jobs[0].start_date) == pinned


def test_pinned_flag_comes_from_the_problem():
    problem = _problem()
    solution = _construct(problem).model_dump(mode="json")
    solution["jobs"][0]["pinned"] = True
    assert mnt.with_initial_solution(problem, solution).jobs[0].pinned is False


def test_invalid_pinned_value_on_the_problem_raises():
    problem = _problem()
    problem.jobs[0].crew_id = problem.crews[0].id
    problem.jobs[0].start_date = _saturday(problem)
    problem.jobs[0].pinned = True
    with pytest.raises(ValueError, match=problem.jobs[0].id):
        mnt.with_initial_solution(problem, {"jobs": []})


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = _problem()
    seeded = mnt.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(_solve(seeded, 0.3)) >= _score(start)
