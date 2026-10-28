"""End-to-end solve tests for the facility_location package.

Requires the compiled extension: uv pip install -e . (or
python setup.py build_ext --inplace).
"""

import random

import facility_location as fl


def demo_problem(facility_count=15, consumer_count=30, seed=0):
    """The Java DemoDataGenerator shape at half scale: uniform locations over
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
    score = fl.solve(problem, time_limit_seconds=2.0, random_seed=7)
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
    score = fl.solve(problem, time_limit_seconds=1.0, random_seed=7)
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
    score = fl.solve(problem, time_limit_seconds=10.0, random_seed=1, stop_at_first_feasible=True)
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
