"""End-to-end solves of the C++ engine through the Python surface."""

import json
import random
from pathlib import Path

import assembly_line_balancing as alb
from assembly_line_balancing.models import AssemblyLineBalancingProblemModel

WEB_DEMO = Path(__file__).parent / "fixtures" / "web_demo.json"


def salbp(task_count=60, seed=0, cycle_time=30):
    """A SALBP-1-like instance: a random acyclic precedence graph, durations 1..15."""
    rng = random.Random(seed)
    tasks = [alb.Task(f"T{i:03d}", rng.randint(1, 15)) for i in range(task_count)]
    arcs = [alb.Precedence(rng.randrange(t), t) for t in range(1, task_count) for _ in range(rng.randint(0, 2))]
    return alb.AssemblyLineBalancingProblem(cycle_time, tasks, arcs)


def lower_bound(problem):
    total = sum(task.duration for task in problem.tasks)
    return -(-total // problem.cycle_time)


def test_solve_assigns_everything_feasibly():
    problem = salbp()
    score = alb.solve(problem, time_limit_seconds=2.0, random_seed=7)
    assert score.is_feasible() and score.hard == 0
    stations = problem.assignment()
    assert all(station >= 0 for station in stations)
    loads = problem.station_loads()
    assert max(loads) <= problem.cycle_time
    for arc in problem.precedences:
        assert stations[arc.before] <= stations[arc.after]
    assert problem.stations_used() <= lower_bound(problem) + 1


def test_the_web_demo_reaches_the_lower_bound():
    """12 tasks, total duration 50 on cycle 12: 5 stations is optimal, with every zoning rule met."""
    model = AssemblyLineBalancingProblemModel.model_validate(json.loads(WEB_DEMO.read_text()))
    alb.solve(model, time_limit_seconds=0.5, random_seed=42)
    assert (model.score_hard, model.score_soft) == (0, -5.0)
    assert model.stations_used == 5
    stations = {task.id: task.station for task in model.tasks}
    assert stations["Frame"] == 0 and 4 <= stations["Packing"] <= 5
    assert stations["Controls"] == stations["Software"] and stations["Paint"] != stations["Inspection"]


def test_solver_score_matches_recompute():
    problem = salbp()
    score = alb.solve(problem, time_limit_seconds=1.0, random_seed=7)
    recomputed = alb.calculate_score(problem)
    assert (score.hard, score.soft) == (recomputed.hard, recomputed.soft)
    assert score.soft == -problem.stations_used()


def test_seeded_step_limited_runs_are_reproducible():
    def run():
        problem = salbp()
        config = alb.SolverConfig()
        config.spent_limit_seconds = 600
        config.step_count_limit = 3000
        config.random_seed = 3
        alb.solve(problem, config=config)
        return problem.assignment(), str(problem.score)

    assert run() == run()


def test_parallel_multi_start_returns_the_best_worker():
    problem = salbp(seed=4)
    config = alb.SolverConfig()
    config.spent_limit_seconds = 600
    config.step_count_limit = 2000
    config.random_seed = 5
    config.parallel_solver_count = 3
    score = alb.solve(problem, config=config)
    assert score.hard == 0
    recomputed = alb.calculate_score(problem)
    assert (score.hard, score.soft) == (recomputed.hard, recomputed.soft)


def test_stop_at_first_feasible():
    problem = salbp()
    score = alb.solve(problem, time_limit_seconds=10.0, random_seed=1, stop_at_first_feasible=True)
    assert score.hard == 0


def test_on_new_best_callback_fires():
    problem = salbp()
    seen = []
    alb.solve(
        problem,
        time_limit_seconds=0.5,
        random_seed=5,
        on_new_best=lambda step, score: seen.append((step, score.hard, score.soft)),
    )
    assert seen
    assert seen[-1][1:] == (problem.score.hard, problem.score.soft)


def test_empty_problem():
    problem = alb.AssemblyLineBalancingProblem(5, [])
    score = alb.solve(problem, time_limit_seconds=0.1)
    assert (score.hard, score.soft) == (0, 0.0)
    assert problem.station_loads() == [] and problem.stations_used() == 0
