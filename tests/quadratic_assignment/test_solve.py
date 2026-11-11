"""quadratic_assignment.solve / Solver / SolverConfig — the engine surface shared with the other C++ packages."""

import itertools
import json
import time
from pathlib import Path

import pytest

import optimaru.quadratic_assignment as qap
from optimaru.quadratic_assignment.constraints import get_constraints
from optimaru.quadratic_assignment.models import QuadraticAssignmentProblemModel

FIXTURES = Path(__file__).parent / "fixtures"


# Also the small problem of tests/_problems.py.
def demo() -> QuadraticAssignmentProblemModel:
    return QuadraticAssignmentProblemModel.model_validate(json.loads((FIXTURES / "web_demo.json").read_text()))


def steps(count: int, seed: int = 1) -> qap.SolverConfig:
    config = qap.SolverConfig()
    config.step_count_limit = count
    config.random_seed = seed
    return config


def test_solver_config_defaults():
    config = qap.SolverConfig()
    assert config.spent_limit_seconds == 30.0
    assert (config.step_count_limit, config.unimproved_step_count_limit) == (0, 0)
    assert config.unimproved_spent_limit_seconds == 0.0
    assert (config.random_seed, config.parallel_solver_count, config.late_acceptance_size) == (0, 1, 0)
    assert config.acceptor_type == qap.AcceptorType.LATE_ACCEPTANCE
    assert config.entity_tabu_size == 7
    assert config.full_assert is False
    assert config.on_new_best is None


@pytest.mark.parametrize(
    "attribute, value",
    [
        ("spent_limit_seconds", -1.0),
        ("unimproved_spent_limit_seconds", -1.0),
        ("parallel_solver_count", 0),
        ("late_acceptance_size", -1),
        ("entity_tabu_size", -1),
    ],
)
def test_solver_config_refuses_invalid_values(attribute, value):
    config = qap.SolverConfig()
    with pytest.raises(ValueError):
        setattr(config, attribute, value)


def test_no_permutation_breaking_knob_is_exposed():
    """Nothing that could break the permutation (change moves, ruin-and-recreate, descent) is exposed."""
    config = qap.SolverConfig()
    breaking = ("change_move_weight", "swap_move_weight", "ruin_recreate", "descent_weight", "nearby_probability")
    for name in breaking:
        assert not hasattr(config, name)
        with pytest.raises(AttributeError):
            setattr(config, name, 1)


def test_solve_on_the_engine_object_and_on_the_model_agree():
    core = demo().to_core()
    assert qap.solve(core, config=steps(3000, seed=7)) is core
    problem = demo()
    model = qap.solve(problem, config=steps(3000, seed=7))
    assert isinstance(model, QuadraticAssignmentProblemModel) and model is not problem
    assert problem.model_dump() == demo().model_dump()  # a model is not solved in place
    assert isinstance(core.score, qap.Score)
    assert (core.score.hard, core.score.soft) == (model.score_hard, model.score_soft)
    ids = [facility.id for facility in core.facilities]
    assert model.assignment == {location: ids[f] for location, f in zip(model.locations, core.assignment())}
    assert model.total_cost == core.total_cost()


def test_the_web_demo_solves_to_a_feasible_bijection():
    model = qap.solve(demo(), time_limit_seconds=1.0, random_seed=1)
    assert model.score_hard >= 0  # feasible
    assert sorted(model.assignment) == sorted(model.locations)
    assert sorted(model.assignment.values()) == sorted(model.facilities)
    assert model.assignment["Annex"] == "Office"  # pinned
    assert model.assignment["Annex"] != "Assembly"  # forbidden
    assert model.score_soft == -model.total_cost
    assert model.total_cost <= 374  # never worse than the identity start


def test_the_web_demo_reaches_its_brute_force_optimum():
    model = demo()
    best = None
    for perm in itertools.permutations(model.facilities):
        candidate = model.model_copy(update={"assignment": dict(zip(model.locations, perm))})
        result = qap.calculate_score(candidate)
        key = (result.hard, result.soft)
        best = key if best is None or key > best else best
    model = qap.solve(model, config=steps(20000, seed=3))
    assert (model.score_hard, model.score_soft) == best


def test_a_step_limit_reproduces_the_same_solve():
    first = demo().to_core()
    second = demo().to_core()
    qap.solve(first, config=steps(2000, seed=11))
    qap.solve(second, config=steps(2000, seed=11))
    assert first.assignment() == second.assignment()
    assert (first.score.hard, first.score.soft) == (second.score.hard, second.score.soft)


def test_keyword_arguments_override_the_config():
    config = steps(100, seed=5)
    calls = []
    qap.solve(
        demo(),
        time_limit_seconds=2.0,
        random_seed=9,
        parallel_solver_count=2,
        on_new_best=lambda step, score: calls.append((step, score.hard, score.soft)),
        config=config,
    )
    # The overrides applied (on_new_best fired) to a copy: the caller's config is unchanged.
    assert calls and all(isinstance(step, int) for step, _, _ in calls)
    assert (config.spent_limit_seconds, config.random_seed, config.parallel_solver_count) == (30.0, 5, 1)
    assert config.step_count_limit == 100 and config.on_new_best is None


def test_stop_at_first_feasible_leaves_the_callers_config_alone():
    config = qap.SolverConfig()
    before = config.best_score_limit
    qap.solve(demo(), time_limit_seconds=5.0, stop_at_first_feasible=True, config=config)
    assert config.best_score_limit == before  # the score limit went to a copy


def test_multi_start_returns_a_consistent_best():
    core = demo().to_core()
    qap.solve(core, time_limit_seconds=0.3, random_seed=1, parallel_solver_count=4)
    assert core.is_bijection()
    fresh = demo().to_core()
    fresh.set_assignment(core.assignment())
    rescored = qap.calculate_score(fresh)
    assert (core.score.hard, core.score.soft) == (rescored.hard, rescored.soft)


@pytest.mark.parametrize("acceptor", ["LATE_ACCEPTANCE", "ENTITY_TABU", "HILL_CLIMBING"])
def test_full_assert_solves_without_corruption(acceptor):
    config = steps(1500, seed=3)
    config.full_assert = True
    config.acceptor_type = getattr(qap.AcceptorType, acceptor)
    core = demo().to_core()
    qap.Solver(config).solve(core)
    plugin = demo().to_core()
    qap.Solver(config).solve_with_constraints(plugin, get_constraints())
    assert core.is_bijection() and plugin.is_bijection()
    assert core.score.is_feasible() and plugin.score.is_feasible()


def test_the_constraints_path_scores_with_the_picks():
    core = demo().to_core()
    qap.Solver(steps(2000)).solve_with_constraints(core, get_constraints())
    default = qap.calculate_score(core)
    assert (core.score.hard, core.score.soft) == (default.hard, default.soft)


def test_zero_budget_returns_the_identity_start():
    model = demo()
    model = qap.solve(model, time_limit_seconds=0)
    assert model.assignment == dict(zip(model.locations, model.facilities))
    assert model.total_cost == 374


def test_calculate_score_fills_the_model():
    model = demo()
    model.assignment = dict(zip(model.locations, model.facilities))
    result = qap.calculate_score(model)
    assert (result.hard, result.soft) == (0, -374.0)
    assert (model.total_cost, model.score_hard, model.score_soft) == (374, 0, -374.0)
    picked = qap.calculate_score(model, get_constraints()[-1:])
    assert picked == (0, -374.0) and isinstance(picked, tuple)


def test_a_warm_start_must_be_a_bijection():
    core = demo().to_core()
    core.warm_start = True
    with pytest.raises(ValueError, match="bijection"):
        qap.solve(core, time_limit_seconds=0)
    core.set_assignment([5, 4, 3, 2, 1, 0])
    qap.solve(core, time_limit_seconds=0)
    assert core.assignment() == [5, 4, 3, 2, 1, 0] and core.warm_start is False


def test_empty_and_single_problems_solve():
    empty = qap.QuadraticAssignmentProblem([], [], [], [])
    score = qap.solve(empty, time_limit_seconds=1).score
    assert (score.hard, score.soft) == (0, 0.0)
    one = qap.QuadraticAssignmentProblem([qap.Facility("F")], [qap.Location("L")], [[2]], [[3]])
    score = qap.solve(one, time_limit_seconds=1).score
    assert one.assignment() == [0] and (score.hard, score.soft) == (0, -6.0)


def test_inputs_of_the_wrong_type_are_refused():
    with pytest.raises(TypeError, match="QuadraticAssignmentProblemModel or a QuadraticAssignmentProblem"):
        qap.solve({"facilities": [], "locations": []})
    with pytest.raises(TypeError):
        qap.constraint_costs([])


def _boom(_step, _score):
    raise RuntimeError("boom")


def test_a_raising_on_new_best_raises_from_a_multi_start_solve():
    """Two parallel starts: the callback's exception reaches Python instead of aborting the process,
    and the other start stops at once instead of running out its 10 s budget."""
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        qap.solve(
            demo(),
            time_limit_seconds=10.0,
            parallel_solver_count=2,
            on_new_best=_boom,
        )
    assert time.monotonic() - started < 8.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    from optimaru.quadratic_assignment import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            qap.solve(demo(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def _random_instance(n: int, seed: int) -> QuadraticAssignmentProblemModel:
    import random

    rng = random.Random(seed)
    return QuadraticAssignmentProblemModel.model_validate(
        {
            "facilities": [f"F{i}" for i in range(n)],
            "locations": [f"L{i}" for i in range(n)],
            "flows": [[0 if i == j else rng.randint(0, 9) for j in range(n)] for i in range(n)],
            "distances": [[0 if i == j else rng.randint(1, 9) for j in range(n)] for i in range(n)],
        }
    )


def _new_bests(acceptor, **knobs) -> list:
    config = steps(3000, seed=7)
    config.acceptor_type = acceptor
    for name, value in knobs.items():
        setattr(config, name, value)
    calls = []
    qap.solve(_random_instance(15, 3).to_core(), config=config, on_new_best=lambda step, score: calls.append(step))
    return calls


def test_simulated_annealing_is_not_hill_climbing():
    """SA gets an automatic starting temperature (the engine default (0, 0) made it hill climbing)."""
    config = qap.SolverConfig()
    assert config.simulated_annealing_starting_temperature == (0, 0.0)
    assert config.simulated_annealing_temperature_fraction == pytest.approx(0.01)
    with pytest.raises(ValueError):
        config.simulated_annealing_temperature_fraction = 0
    assert _new_bests(qap.AcceptorType.SIMULATED_ANNEALING) != _new_bests(qap.AcceptorType.HILL_CLIMBING)


def test_great_deluge_rises_by_its_water_level_step():
    """The water level starts at the start's score and stays there at the default step (0, 0);
    with a step it rises, and the search is another one."""
    great_deluge = qap.AcceptorType.GREAT_DELUGE
    assert _new_bests(great_deluge) == _new_bests(great_deluge, great_deluge_water_level_step=(0, 0.0))
    assert _new_bests(great_deluge) != _new_bests(great_deluge, great_deluge_water_level_step=(0, 0.5))


def test_step_counting_hill_climbing_reads_its_size():
    step_counting = qap.AcceptorType.STEP_COUNTING_HILL_CLIMBING
    assert _new_bests(step_counting) == _new_bests(step_counting, step_counting_hill_climbing_size=400)
    assert _new_bests(step_counting) != _new_bests(step_counting, step_counting_hill_climbing_size=5)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. A limit of (0, -1e18) would never be reached by such a score.
    """
    import dataclasses
    import time

    import optimaru.quadratic_assignment as package
    from optimaru.quadratic_assignment.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        demo(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
