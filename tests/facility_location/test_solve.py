"""End-to-end solve tests for the facility_location package.

Requires the compiled extension: uv pip install -e . (or
python setup.py build_ext --inplace).
"""

import random

import optimaru.facility_location as fl


# Also the small problem of tests/_problems.py.
def demo_problem(facility_count=15, consumer_count=30, seed=0):
    """The demo instance shape at half scale: uniform locations over
    the London box, equal capacity/demand shares (5x capacity slack),
    setup cost ~ N(50000, 10000^2)."""
    rng = random.Random(seed)
    facilities = [
        fl.Facility(
            str(f),
            fl.Location(rng.uniform(51.44, 51.56), rng.uniform(-0.16, -0.01)),
            max(1000, int(rng.gauss(50000, 10000))),
            4500 // facility_count,
        )
        for f in range(facility_count)
    ]
    consumers = [
        fl.Consumer(
            str(facility_count + c),
            fl.Location(rng.uniform(51.44, 51.56), rng.uniform(-0.16, -0.01)),
            900 // consumer_count,
        )
        for c in range(consumer_count)
    ]
    return fl.FacilityLocationProblem(facilities, consumers, fl.Location(51.44, -0.16), fl.Location(51.56, -0.01))


def test_solve_assigns_everything_feasibly():
    problem = demo_problem()
    score = fl.solve(problem, time_limit_seconds=2.0, random_seed=7).score
    assert score.is_feasible()
    assert score.hard == 0
    assignment = problem.assignment()
    assert all(f >= 0 for f in assignment)
    # Capacity respected per facility.
    used = [0] * problem.facility_count()
    for consumer, facility in zip(problem.consumers, assignment):
        used[facility] += consumer.demand
    for facility, demand in zip(problem.facilities, used):
        assert demand <= facility.capacity


def test_solver_score_matches_recompute():
    problem = demo_problem()
    score = fl.solve(problem, time_limit_seconds=1.0, random_seed=7).score
    recomputed = fl.calculate_score(problem)
    assert (score.hard, score.soft) == (recomputed.hard, recomputed.soft)
    # Summary getters agree with the score's soft breakdown.
    assert score.soft == -(2 * problem.total_cost() + 6 * problem.total_distance_meters())


def test_seeded_runs_are_reproducible():
    a = demo_problem()
    b = demo_problem()
    fl.solve(a, time_limit_seconds=0.5, random_seed=3)
    fl.solve(b, time_limit_seconds=0.5, random_seed=3)
    assert str(a.score) == str(b.score)
    assert a.assignment() == b.assignment()


def test_stop_at_first_feasible():
    problem = demo_problem()
    score = fl.solve(problem, time_limit_seconds=10.0, random_seed=1, stop_at_first_feasible=True).score
    assert score.hard == 0


def test_on_new_best_callback_fires():
    problem = demo_problem()
    seen = []
    fl.solve(
        problem,
        time_limit_seconds=0.5,
        random_seed=5,
        on_new_best=lambda step, score: seen.append((step, score.hard, score.soft)),
    )
    assert seen  # at least the construction-heuristic best
    assert seen[-1][1:] == (problem.score.hard, problem.score.soft)


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        fl.solve(demo_problem(), time_limit_seconds=30.0, parallel_solver_count=4, on_new_best=boom)
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    import time

    import pytest

    from optimaru.facility_location import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            fl.solve(demo_problem(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. A finite soft limit such as (0, -1e18) would never be reached by such a score.
    """
    import dataclasses
    import time

    import optimaru.facility_location as package
    from optimaru.facility_location.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        demo_problem(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
