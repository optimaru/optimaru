"""with_initial_solution: a seed reaches the search untouched and bad input never reaches C++."""

import pytest

pytest.importorskip("pydantic")

import project_job as pj
from project_job.demo import generate_demo_data
from project_job.models import ProjectJobScheduleModel


def _plan(model):
    return {a.id: (a.execution_mode_id, a.delay) for a in model.allocations}


def _score(model):
    return (model.score_hard, model.score_medium, model.score_soft)


def _construct(model):
    """Construction only: step_count_limit = 0 means "no limit", a zero time budget stops before the first step."""
    return pj.solve(model, time_limit_seconds=0, random_seed=1)


def _standard(model):
    """The allocations of STANDARD jobs; SOURCE and SINK are always pinned."""
    job_types = {job.id: job.job_type for job in model.jobs}
    return [a for a in model.allocations if job_types[a.job_id] == "STANDARD"]


def _non_greedy_seed(problem):
    """Every STANDARD allocation on its next mode and a few days later than construction puts it."""
    seed = _construct(problem)
    jobs = {job.id: job for job in problem.jobs}
    for allocation in _standard(seed):
        mode_ids = [mode.id for mode in jobs[allocation.job_id].execution_modes]
        allocation.execution_mode_id = mode_ids[(mode_ids.index(allocation.execution_mode_id) + 1) % len(mode_ids)]
        allocation.delay += 3
    return seed


def test_exported_from_the_package():
    assert "with_initial_solution" in pj.__all__


def test_complete_seed_skips_construction():
    problem = generate_demo_data()
    seed = _non_greedy_seed(problem)
    result = _construct(pj.with_initial_solution(problem, seed))
    assert _plan(result) == _plan(seed)
    assert _plan(result) != _plan(_construct(problem))


@pytest.mark.parametrize("as_json", [False, True])
def test_solved_result_feeds_back(as_json):
    problem = generate_demo_data()
    solved = pj.solve(problem, time_limit_seconds=0.3, random_seed=1)
    solution = solved.model_dump(mode="json") if as_json else solved
    result = _construct(pj.with_initial_solution(problem, solution))
    assert isinstance(result, ProjectJobScheduleModel)
    assert _plan(result) == _plan(solved)
    assert _score(result) == _score(solved)


def test_partial_seed_keeps_its_part_and_fills_the_rest():
    problem = generate_demo_data()
    seed = _non_greedy_seed(problem)
    standard_ids = {a.id for a in _standard(seed)}
    kept = [a for a in seed.model_dump(mode="json")["allocations"] if a["id"] in standard_ids][:8]
    kept_ids = {a["id"] for a in kept}

    seeded = pj.with_initial_solution(problem, {"allocations": kept})
    assert all(_plan(seeded)[i] == (None, None) for i in standard_ids - kept_ids)

    result = _construct(seeded)
    assert all(_plan(result)[i] == _plan(seed)[i] for i in kept_ids)
    assert all(mode_id is not None for mode_id, _ in _plan(result).values())


def test_allocations_are_matched_by_id_and_prior_values_are_replaced():
    solved = _non_greedy_seed(generate_demo_data())
    last = _standard(solved)[-1]
    solution = {"allocations": [{"id": last.id, "execution_mode_id": last.execution_mode_id, "delay": last.delay}]}
    seeded = pj.with_initial_solution(solved, solution)
    assert _plan(seeded)[last.id] == (last.execution_mode_id, last.delay)
    assert all(_plan(seeded)[a.id] == (None, None) for a in _standard(solved) if a.id != last.id)
    assert all(a.start_date is None and a.end_date is None for a in _standard(seeded))
    assert _score(seeded) == (None, None, None)


def test_unknown_allocation_raises_naming_it():
    with pytest.raises(ValueError, match="no-such-allocation"):
        pj.with_initial_solution(generate_demo_data(), {"allocations": [{"id": "no-such-allocation"}]})


def test_mode_of_another_job_raises_naming_the_allocation():
    problem = generate_demo_data()
    first, second = _standard(problem)[:2]
    jobs = {job.id: job for job in problem.jobs}
    foreign_mode = jobs[second.job_id].execution_modes[0].id
    assert foreign_mode not in [mode.id for mode in jobs[first.job_id].execution_modes]
    with pytest.raises(ValueError, match=f"allocation '{first.id}'"):
        pj.with_initial_solution(problem, {"allocations": [{"id": first.id, "execution_mode_id": foreign_mode}]})


@pytest.mark.parametrize("mode_id", ["no-such-mode", 0, ["mode"], True])
def test_unknown_mode_raises_naming_the_allocation(mode_id):
    problem = generate_demo_data()
    allocation = _standard(problem)[0]
    with pytest.raises(ValueError, match=f"allocation '{allocation.id}'"):
        pj.with_initial_solution(problem, {"allocations": [{"id": allocation.id, "execution_mode_id": mode_id}]})


def _mode_of(problem, allocation):
    return next(job for job in problem.jobs if job.id == allocation.job_id).execution_modes[0].id


def test_highest_delay_is_accepted():
    problem = generate_demo_data()
    allocation = _standard(problem)[0]
    delay = problem.delay_range_size - 1
    solution = {
        "allocations": [{"id": allocation.id, "execution_mode_id": _mode_of(problem, allocation), "delay": delay}]
    }
    seeded = pj.with_initial_solution(problem, solution)
    assert _plan(_construct(seeded))[allocation.id][1] == delay


@pytest.mark.parametrize("delay", [-1, 500, 10**9, "1", 1.5, True])
def test_out_of_range_delay_raises_naming_the_allocation(delay):
    problem = generate_demo_data()
    allocation = _standard(problem)[0]
    entry = {"id": allocation.id, "execution_mode_id": _mode_of(problem, allocation), "delay": delay}
    with pytest.raises(ValueError, match=f"allocation '{allocation.id}'"):
        pj.with_initial_solution(problem, {"allocations": [entry]})


def test_out_of_range_delay_on_a_model_raises():
    problem = generate_demo_data()
    seed = _construct(problem)
    allocation = _standard(seed)[0]
    allocation.delay = problem.delay_range_size  # plain assignment skips the pydantic validator
    with pytest.raises(ValueError, match=f"allocation '{allocation.id}'"):
        pj.with_initial_solution(problem, seed)


def test_delay_without_a_mode_raises_naming_the_allocation():
    problem = generate_demo_data()
    allocation = _standard(problem)[0]
    with pytest.raises(ValueError, match=f"allocation '{allocation.id}'"):
        pj.with_initial_solution(problem, {"allocations": [{"id": allocation.id, "delay": 3}]})


def test_repeated_allocation_raises():
    problem = generate_demo_data()
    allocation_id = _standard(problem)[0].id
    with pytest.raises(ValueError, match=f"'{allocation_id}'"):
        pj.with_initial_solution(problem, {"allocations": [{"id": allocation_id}, {"id": allocation_id}]})


@pytest.mark.parametrize(
    "solution", [None, [], {}, {"allocations": "x"}, {"allocations": ["x"]}, {"allocations": [{}]}]
)
def test_malformed_solution_raises(solution):
    with pytest.raises(ValueError):
        pj.with_initial_solution(generate_demo_data(), solution)


def test_inputs_are_not_mutated():
    problem = _construct(generate_demo_data())
    seed = _non_greedy_seed(problem)
    problem_before, seed_before = problem.model_dump(), seed.model_dump()
    seeded = pj.with_initial_solution(problem, seed)
    assert problem.model_dump() == problem_before
    assert seed.model_dump() == seed_before
    assert seeded is not problem and seeded.allocations[0] is not problem.allocations[0]


def test_pinned_allocation_keeps_its_value_whatever_the_solution_says():
    problem = _construct(generate_demo_data())
    seed = _non_greedy_seed(problem)
    allocation = _standard(problem)[0]
    allocation.pinned = True
    pinned = (allocation.execution_mode_id, allocation.delay)
    assert pinned != _plan(seed)[allocation.id]

    seeded = pj.with_initial_solution(problem, seed)
    assert _plan(seeded)[allocation.id] == pinned
    assert _plan(pj.solve(seeded, time_limit_seconds=0.2, random_seed=1))[allocation.id] == pinned

    garbage = {"allocations": [{"id": allocation.id, "execution_mode_id": "no-such-mode", "delay": 10**9}]}
    assert _plan(pj.with_initial_solution(problem, garbage))[allocation.id] == pinned


def test_source_and_sink_keep_their_value_without_a_pinned_flag():
    problem = generate_demo_data()
    jobs = {job.id: job for job in problem.jobs}
    milestones = [a for a in problem.allocations if jobs[a.job_id].job_type != "STANDARD"]
    assert milestones and not any(a.pinned for a in milestones)
    before = _plan(problem)
    entries = [{"id": a.id, "execution_mode_id": "no-such-mode", "delay": 7} for a in milestones]
    seeded = pj.with_initial_solution(problem, {"allocations": entries})
    assert all(_plan(seeded)[a.id] == before[a.id] for a in milestones)


def test_pinned_flag_comes_from_the_problem():
    problem = generate_demo_data()
    allocation_id = _standard(problem)[0].id
    solution = {"allocations": [{"id": allocation_id, "pinned": True}]}
    seeded = pj.with_initial_solution(problem, solution)
    assert next(a for a in seeded.allocations if a.id == allocation_id).pinned is False


def test_invalid_pinned_value_on_the_problem_raises():
    problem = generate_demo_data()
    allocation = _standard(problem)[0]
    allocation.execution_mode_id = _mode_of(problem, allocation)
    allocation.delay = problem.delay_range_size
    allocation.pinned = True
    with pytest.raises(ValueError, match=f"allocation '{allocation.id}'"):
        pj.with_initial_solution(problem, {"allocations": []})


def test_seeded_solve_is_not_worse_than_the_seed():
    problem = generate_demo_data()
    seeded = pj.with_initial_solution(problem, _non_greedy_seed(problem))
    start = _construct(seeded)
    assert _score(pj.solve(seeded, time_limit_seconds=0.3, random_seed=1)) >= _score(start)
