"""with_initial_solution: a solve starts from an earlier result, matched by id."""

import copy
import json
import time

import pytest

pytest.importorskip("pydantic")

import es_scheduling as es  # noqa: E402
from es_scheduling.demo import generate_demo_data  # noqa: E402
from es_scheduling.models import (  # noqa: E402
    ScheduleSolution,
    ShiftAssignmentModel,
    SolverSettings,
    solve_request,
    with_initial_solution,
)


def assignment_of(solution):
    return {entry.shift_id: worker for worker, entries in solution.schedule.items() for entry in entries}


def zero_step(request):
    """No time budget: construction runs, local search and repair do not."""
    limited = request.model_copy(deep=True)
    limited.timelimit = 0
    return limited


def as_solution(request, assignment):
    """A `ScheduleSolution` holding `assignment` ({shift id: worker id}), with the
    rest of the shifts left out."""
    schedule = {worker.id: [] for worker in request.model_input.workers}
    for shift in request.model_input.shifts:
        if shift.id in assignment:
            schedule[assignment[shift.id]].append(
                ShiftAssignmentModel(shift_id=shift.id, start=shift.start, end=shift.end)
            )
    return ScheduleSolution(schedule=schedule, score_hard=0, score_soft=0.0, feasible=True)


def round_robin_seed(request):
    """A complete assignment no construction heuristic would produce: backwards
    through the workers, whatever their skills and days off."""
    workers = [worker.id for worker in request.model_input.workers]
    return {shift.id: workers[-1 - i % len(workers)] for i, shift in enumerate(request.model_input.shifts)}


@pytest.fixture
def request_():
    return generate_demo_data(seed=3, workers=8, days=7)


def test_a_complete_seed_with_no_budget_comes_back_unchanged(request_):
    seed = round_robin_seed(request_)
    constructed = assignment_of(solve_request(zero_step(request_)))
    assert seed != constructed  # the test must not pass by matching the greedy result

    solved = solve_request(zero_step(with_initial_solution(request_, as_solution(request_, seed))))

    assert assignment_of(solved) == seed
    assert solved.unassigned == []


@pytest.mark.parametrize("greedy", [True, False], ids=["greedy-construction", "engine-construction"])
def test_construction_leaves_a_complete_seed_alone(request_, greedy):
    request_.settings.employee_greedy_construction = greedy
    seed = round_robin_seed(request_)
    solved = solve_request(zero_step(with_initial_solution(request_, as_solution(request_, seed))))
    assert assignment_of(solved) == seed


def test_a_solved_result_restarts_from_where_it_ended(request_):
    request_.timelimit = 0.3
    first = solve_request(request_)

    again = solve_request(zero_step(with_initial_solution(request_, first)))

    assert assignment_of(again) == assignment_of(first)
    assert again.score_hard == first.score_hard
    assert again.score_soft == pytest.approx(first.score_soft)


def test_a_dict_works_the_same_as_the_model(request_):
    solution = as_solution(request_, round_robin_seed(request_))
    from_model = with_initial_solution(request_, solution)
    from_dict = with_initial_solution(request_, solution.model_dump(mode="json"))
    from_json = with_initial_solution(request_, json.loads(solution.model_dump_json()))
    assert from_dict == from_model
    assert from_json == from_model


def test_the_order_of_the_solution_does_not_matter(request_):
    solution = as_solution(request_, round_robin_seed(request_)).model_dump(mode="json")
    backwards = {
        "schedule": {worker: entries[::-1] for worker, entries in reversed(solution["schedule"].items())},
        "unassigned": [],
    }
    assert with_initial_solution(request_, backwards) == with_initial_solution(request_, solution)


def test_only_the_shift_id_of_an_assignment_is_read(request_):
    shift = request_.model_input.shifts[0]
    worker = request_.model_input.workers[-1]
    seeded = with_initial_solution(request_, {"schedule": {worker.id: [{"shift_id": shift.id}]}})
    assert seeded.model_input.shifts[0].worker == worker.id


@pytest.mark.parametrize("greedy", [True, False], ids=["greedy-construction", "engine-construction"])
def test_a_partial_seed_keeps_its_part_and_fills_the_rest(request_, greedy):
    request_.settings.employee_greedy_construction = greedy
    full = round_robin_seed(request_)
    kept = dict(list(full.items())[::2])
    assert len(kept) < len(full)

    solved = solve_request(zero_step(with_initial_solution(request_, as_solution(request_, kept))))

    assignment = assignment_of(solved)
    assert solved.unassigned == []
    assert set(assignment) == set(full)
    assert {shift_id: assignment[shift_id] for shift_id in kept} == kept


def test_only_the_left_out_shifts_are_constructed(request_):
    """Construction with one shift left out touches that shift and no other."""
    seed = round_robin_seed(request_)
    left_out = next(iter(seed))
    partial = {shift_id: worker for shift_id, worker in seed.items() if shift_id != left_out}

    assignment = assignment_of(
        solve_request(zero_step(with_initial_solution(request_, as_solution(request_, partial))))
    )

    assert {shift_id: assignment[shift_id] for shift_id in partial} == partial
    assert left_out in assignment


def test_a_left_out_shift_loses_its_earlier_worker(request_):
    shifts = request_.model_input.shifts
    shifts[0].worker = request_.model_input.workers[0].id
    shifts[1].worker = request_.model_input.workers[0].id

    seeded = with_initial_solution(request_, as_solution(request_, {shifts[1].id: request_.model_input.workers[1].id}))

    assert seeded.model_input.shifts[0].worker is None
    assert seeded.model_input.shifts[1].worker == request_.model_input.workers[1].id


def test_a_shift_listed_as_unassigned_is_constructed(request_):
    seed = round_robin_seed(request_)
    left_out = next(iter(seed))
    solution = as_solution(request_, {k: v for k, v in seed.items() if k != left_out})
    solution.unassigned = [left_out]

    seeded = with_initial_solution(request_, solution)

    assert seeded.model_input.shifts[0].worker is None
    assert left_out in assignment_of(solve_request(zero_step(seeded)))


def test_an_empty_solution_is_a_plain_construction(request_):
    empty = {"schedule": {}, "unassigned": []}
    assert with_initial_solution(request_, empty) == request_


def test_a_pinned_shift_stays_with_its_worker(request_):
    shift = request_.model_input.shifts[0]
    shift.worker, shift.pinned = request_.model_input.workers[0].id, True

    left_out = with_initial_solution(request_, as_solution(request_, {}))
    same = with_initial_solution(request_, as_solution(request_, {shift.id: shift.worker}))

    assert left_out.model_input.shifts[0].worker == shift.worker
    assert same.model_input.shifts[0].worker == shift.worker
    assert same.model_input.shifts[0].pinned


def test_a_pinned_shift_cannot_be_moved_or_unassigned(request_):
    shift = request_.model_input.shifts[0]
    shift.worker, shift.pinned = request_.model_input.workers[0].id, True
    other = request_.model_input.workers[1].id

    with pytest.raises(ValueError, match=rf"shift {shift.id!r} is pinned"):
        with_initial_solution(request_, as_solution(request_, {shift.id: other}))
    with pytest.raises(ValueError, match=rf"shift {shift.id!r} is pinned"):
        with_initial_solution(request_, {"schedule": {}, "unassigned": [shift.id]})


def test_a_heavily_pinned_roster_keeps_every_pinned_shift():
    """Every move, ruin and repair path on, and pins that keep the roster
    infeasible so the repair pass runs too."""
    request = generate_demo_data(seed=3, workers=12, days=14)
    seed = round_robin_seed(request)
    for index, shift in enumerate(request.model_input.shifts):
        shift.worker = seed[shift.id]
        shift.pinned = index % 10 != 0
    request.timelimit = 0.6
    request.settings = SolverSettings(
        smart_change_moves=True,
        employee_unassign_moves=True,
        custom_move_probability=0.6,
        ruin_recreate_unimproved_step_trigger=50,
        ruin_recreate_as_move_weight=0.2,
        greedy_repair_budget_fraction=0.5,
        employee_run_repair_max_days=1,
    )

    solved = solve_request(request)

    assert solved.score_hard < 0
    assigned = assignment_of(solved)
    assert {s.id: s.worker for s in request.model_input.shifts if s.pinned} == {
        s.id: assigned.get(s.id) for s in request.model_input.shifts if s.pinned
    }


def test_parallel_starts_each_start_from_the_given_solution(request_):
    pinned = request_.model_input.shifts[0]
    pinned.worker, pinned.pinned = request_.model_input.workers[-1].id, True
    first = solve_request(zero_step(request_))
    seeded = with_initial_solution(zero_step(request_), first)
    seeded.num_workers = 2

    candidates = []

    def select(solutions):
        candidates.extend(solutions)
        return solutions[0]

    solve_request(seeded, select=select)

    assert [assignment_of(candidate) for candidate in candidates] == [assignment_of(first)] * 2


def test_repair_stops_at_the_time_limit_on_a_large_infeasible_roster():
    """Two blocks of workers each work half the horizon without a day off, so a
    single repair scan has millions of swaps to score."""
    days, workers = 60, 300
    request = generate_demo_data(seed=3, workers=workers, days=days)
    worker_ids = [worker.id for worker in request.model_input.workers]
    next_on_day = {}
    for shift in request.model_input.shifts:
        day = shift.start // (24 * 60)
        index = next_on_day.get(day, 0)
        next_on_day[day] = index + 1
        shift.worker = worker_ids[index * 2 + (day >= days // 2)]
    request.timelimit = 1.0
    request.settings = SolverSettings(
        step_count_limit=1,
        greedy_repair_budget_fraction=0.8,
        employee_run_repair_max_days=1,
        employee_day_repair_moves=False,
    )

    started = time.perf_counter()
    solved = solve_request(request)

    assert solved.score_hard < 0
    assert time.perf_counter() - started < 6.0


def test_a_fully_pinned_roster_comes_back_unchanged(request_):
    """All on one worker, so moving any of them would improve the balance rule."""
    worker = request_.model_input.workers[0].id
    for shift in request_.model_input.shifts:
        shift.worker, shift.pinned = worker, True
    request_.timelimit = 0.2
    request_.settings = SolverSettings(smart_change_moves=True, custom_move_probability=0.9)

    assert set(assignment_of(solve_request(request_)).values()) == {worker}


@pytest.mark.parametrize(
    "bad_solution, message",
    [
        ({"schedule": {"Worker 99": []}}, "unknown worker 'Worker 99'"),
        ({"schedule": {"Worker 01": [{"shift_id": "Day 99"}]}}, "unknown shift 'Day 99'"),
        ({"schedule": {}, "unassigned": ["Day 99"]}, "unknown unassigned shift 'Day 99'"),
        ({"schedule": {"Worker 01": [{}]}}, "worker 'Worker 01': an assignment needs a shift_id"),
        ({"schedule": {"Worker 01": ["Shift 1"]}}, "worker 'Worker 01': an assignment needs a shift_id"),
        ({"schedule": {"Worker 01": [{"shift_id": 7}]}}, "worker 'Worker 01': an assignment needs a shift_id"),
        ({"schedule": {"Worker 01": {}}}, "worker 'Worker 01': assignments must be a list"),
        ({"schedule": {}, "unassigned": "Shift 1"}, "'unassigned' must be a list"),
        ({"schedule": {}, "unassigned": [["Shift 1"]]}, "unknown unassigned shift"),
        ({"schedule": []}, "'schedule' mapping"),
        ({}, "'schedule' mapping"),
        ([], "'schedule' mapping"),
        (None, "'schedule' mapping"),
    ],
)
def test_a_bad_solution_is_refused_by_name(request_, bad_solution, message):
    before = copy.deepcopy(bad_solution)
    with pytest.raises(ValueError, match=message):
        with_initial_solution(request_, bad_solution)
    assert bad_solution == before


def test_a_shift_assigned_twice_is_refused(request_):
    shift_id = request_.model_input.shifts[0].id
    entry = {"shift_id": shift_id}
    for schedule in (
        {"Worker 01": [entry], "Worker 02": [entry]},
        {"Worker 01": [entry, entry]},
    ):
        with pytest.raises(ValueError, match=rf"shift {shift_id!r} is assigned twice"):
            with_initial_solution(request_, {"schedule": schedule})


def test_a_shift_both_assigned_and_unassigned_is_refused(request_):
    shift_id = request_.model_input.shifts[0].id
    solution = {"schedule": {"Worker 01": [{"shift_id": shift_id}]}, "unassigned": [shift_id]}
    with pytest.raises(ValueError, match=rf"shift {shift_id!r} is both assigned"):
        with_initial_solution(request_, solution)


def test_nothing_out_of_range_reaches_the_core(request_):
    """The core does no bounds check of its own: an employee index only ever comes
    from a worker id that `with_initial_solution` has already found."""
    seeded = with_initial_solution(request_, as_solution(request_, round_robin_seed(request_)))
    worker_ids = {worker.id for worker in seeded.model_input.workers}
    assert all(shift.worker in worker_ids for shift in seeded.model_input.shifts)


def test_the_inputs_are_not_mutated(request_):
    solution = as_solution(request_, round_robin_seed(request_))
    request_before = request_.model_dump()
    solution_before = solution.model_dump()

    seeded = with_initial_solution(request_, solution)
    seeded.model_input.shifts[0].worker = None
    seeded.model_input.workers[0].skills.append("Skill Z")
    seeded.model_input.rules.pop()

    assert request_.model_dump() == request_before
    assert solution.model_dump() == solution_before
    assert all(shift.worker is None for shift in request_.model_input.shifts)


def test_the_top_level_package_exports_it(request_):
    assert es.with_initial_solution is with_initial_solution
    assert "with_initial_solution" in es.__all__
