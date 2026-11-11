"""The contract docs/use-cases/README.md states under "One way to solve"."""

import inspect
import subprocess
import sys
import time

import pytest

import optimaru

from ._build import use_case_module
from ._problems import _call, levels_of, score_of, small_problem

SHARED_KEYWORDS = {
    "time_limit_seconds",
    "random_seed",
    "parallel_solver_count",
    "stop_at_first_feasible",
    "full_assert",
    "on_new_best",
    "config",
    "constraints",
}

# Their pydantic layer models a whole request (the input with its time limit, seed and worker
# count). solve() takes it as it takes the compiled problem, and returns the response model.
# Each entry builds a request with a zero time limit: construction, and no search step.
REQUEST_MODELS = {
    "employee_scheduling": _call("optimaru.employee_scheduling.demo", "generate_demo_data", workers=6, days=4),
    "order_picking": _call(".order_picking.test_initial_solution", "problem"),
    "pickup_delivery": _call(".pickup_delivery.test_initial_solution", "problem"),
    "vehicle_routing": _call(".vehicle_routing.test_initial_solution", "problem"),
}
REQUEST_MODEL_USE_CASES = sorted(REQUEST_MODELS)

# Its solve() takes and returns dicts, so the package does not import its pydantic models.
NO_MODELS_ON_IMPORT = {"school_timetabling"}

# solve() answers with the solution document of its API there, not with a problem that carries
# a score: an infeasible document does not say how infeasible, so two of them cannot be ordered.
NO_SCORE_ON_THE_RESULT = {"school_timetabling"}


# The levels of each use case's compiled Score class, most important first. Two levels where a
# use case is not named: the engine's Score.
SCORE_LEVELS = {
    "bed_allocation": ["hard", "medium", "soft"],
    "food_packaging": ["hard", "medium", "soft"],
    "pickup_delivery": ["hard", "medium", "soft"],
    "project_job_scheduling": ["hard", "medium", "soft"],
    "task_assigning": ["hard", "soft0", "soft1", "soft2"],
    "vehicle_routing": ["hard", "medium", "soft"],
}


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_score_class_has_the_levels_of_the_use_case(use_case):
    score = use_case_module(use_case).Score
    levels = [name for name in ("hard", "medium", "soft", "soft0", "soft1", "soft2") if hasattr(score, name)]
    assert levels == SCORE_LEVELS.get(use_case, ["hard", "soft"])
    assert callable(score.is_feasible)


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_solve_takes_the_shared_keywords(use_case):
    module = use_case_module(use_case)
    assert "solve" in module.__all__
    parameters = inspect.signature(module.solve).parameters
    keyword_only = {name for name, parameter in parameters.items() if parameter.kind is parameter.KEYWORD_ONLY}
    assert SHARED_KEYWORDS <= keyword_only
    positional = [name for name, parameter in parameters.items() if parameter.kind is not parameter.KEYWORD_ONLY]
    assert len(positional) == 1  # the problem


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_solve_refuses_what_is_no_problem(use_case):
    with pytest.raises((TypeError, ValueError, AttributeError)):
        use_case_module(use_case).solve(object())


def _request(use_case):
    request = REQUEST_MODELS[use_case]()
    return request.model_copy(update={"timelimit": 0.0})


@pytest.mark.parametrize("use_case", REQUEST_MODEL_USE_CASES)
def test_solve_takes_the_request_model_and_returns_the_response_model(use_case):
    """solve(request) is the solve models.solve_request runs: the same response, and the
    request is left as it was."""
    module = use_case_module(use_case)
    request = _request(use_case)
    before = request.model_dump()
    solved = module.solve(request)
    assert type(solved) is type(module.models.solve_request(request))
    assert solved == module.models.solve_request(request)
    assert request.model_dump() == before


@pytest.mark.parametrize("use_case", REQUEST_MODEL_USE_CASES)
def test_the_keywords_of_solve_replace_what_the_request_says(use_case):
    """A request with a 20 s budget and a zero-second keyword returns at once, and the callback
    of a single start is called with the score of the result."""
    module = use_case_module(use_case)
    request = REQUEST_MODELS[use_case]().model_copy(update={"timelimit": 20.0})
    started = time.monotonic()
    assert module.solve(request, time_limit_seconds=0.0, random_seed=3) == module.models.solve_request(
        _with_seed(request.model_copy(update={"timelimit": 0.0}), 3)
    )
    assert time.monotonic() - started < 10.0
    assert request.timelimit == 20.0

    reported = []
    module.solve(
        request,
        time_limit_seconds=20.0,
        stop_at_first_feasible=True,
        full_assert=True,
        on_new_best=lambda step, score: reported.append(score),
    )
    assert time.monotonic() - started < 15.0
    assert reported and reported[-1].hard == 0


def _with_seed(request, seed):
    if hasattr(request, "random_seed"):
        return request.model_copy(update={"random_seed": seed})
    return request.model_copy(update={"settings": request.settings.model_copy(update={"random_seed": seed})})


def test_an_employee_scheduling_request_keeps_its_rules_and_settings():
    """config and constraints belong to the compiled schedule: a request says both itself.
    And the starts of a request run in processes, which cannot call back."""
    module = use_case_module("employee_scheduling")
    request = _request("employee_scheduling")
    with pytest.raises(ValueError, match="settings and model_input.rules"):
        module.solve(request, config=module.SolverConfig())
    with pytest.raises(ValueError, match="settings and model_input.rules"):
        module.solve(request, constraints=[])
    with pytest.raises(ValueError, match="single start"):
        module.solve(request, parallel_solver_count=2, on_new_best=lambda step, score: None)
    with pytest.raises(ValueError, match="at least 1"):
        module.solve(request, parallel_solver_count=0)
    many = request.model_copy(update={"settings": request.settings.model_copy(update={"multi_start": 3})})
    assert module.solve(many, parallel_solver_count=1) == module.solve(request)


def _feasible(solved) -> bool:
    if isinstance(solved, dict):  # school timetabling's solution document
        return solved["status"] == "FEASIBLE"
    return score_of(solved)[0] == 0


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_solve_stops_at_the_first_feasible_solution_and_reports_it(use_case):
    """The target and the callback reach the engine: a 20 s solve of a small problem ends as
    soon as it is feasible, and on_new_best was called on the way, with steps that never go
    back and, last, the score the result carries. A use case with a score of its own hands
    both to the engine itself, and one that forgot would run out the 20 s."""
    module = use_case_module(use_case)
    reported = []
    started = time.monotonic()
    solved = module.solve(
        small_problem(use_case),
        time_limit_seconds=20.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: reported.append((step, score)),
    )
    assert time.monotonic() - started < 10.0
    assert _feasible(solved)
    steps = [step for step, _ in reported]
    assert steps and all(isinstance(step, int) for step in steps) and steps == sorted(steps)
    assert reported[-1][1].hard == 0
    if use_case not in NO_SCORE_ON_THE_RESULT:
        assert levels_of(reported[-1][1]) == score_of(solved)


@pytest.mark.parametrize("use_case", sorted(set(optimaru.__all__) - NO_SCORE_ON_THE_RESULT))
def test_parallel_starts_return_the_best_single_start(use_case):
    """Start i searches with random_seed + i and the best score wins, so three step-limited
    starts return what the best of the three single-start solves does. A solver that ran one
    start whatever the count says would return the first one's score.

    This needs a start limited by steps to be reproducible: a solver that splits its budget
    by the clock has to yield to a step limit, as conference scheduling's segments do."""
    module = use_case_module(use_case)

    def score(seed, starts):
        config = module.SolverConfig()
        config.spent_limit_seconds = 60.0  # the step limit ends the solve
        config.step_count_limit = 2000
        config.random_seed = seed
        config.parallel_solver_count = starts
        return score_of(module.solve(small_problem(use_case), config=config))

    for seed in (0, 10, 20):
        singles = [score(seed + start, 1) for start in range(3)]
        assert score(seed, 1) == singles[0], "a step-limited start is not reproducible"
        assert score(seed, 3) == max(singles)


@pytest.mark.parametrize("use_case", sorted(set(optimaru.__all__) - NO_SCORE_ON_THE_RESULT))
def test_a_multi_start_solve_with_a_target_returns_the_score_of_its_solution(use_case):
    """A start that reaches the target stops the others, and the best candidate wins, one that
    was stopped on the way included. Whichever start that is, the score on the result is the
    score of the solution on it: a stopped start hands over a scored candidate.

    This checks the scoring only. On problems this small every start is feasible within
    milliseconds, so the stop itself is what tests/engine/engine_test.cpp checks."""
    module = use_case_module(use_case)
    for seed in (0, 1, 2):
        started = time.monotonic()
        solved = module.solve(
            small_problem(use_case),
            time_limit_seconds=20.0,
            random_seed=seed,
            parallel_solver_count=4,
            stop_at_first_feasible=True,
        )
        assert time.monotonic() - started < 10.0
        assert score_of(solved)[0] == 0
        compiled = solved.to_core() if hasattr(solved, "to_core") else solved
        assert levels_of(module.calculate_score(compiled)) == score_of(solved)


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_importing_a_use_case_imports_its_models(use_case):
    """pydantic is a dependency, so a package imports its models with itself and hands out the
    models functions as they are, not wrappers that import them on the first call."""
    module = use_case_module(use_case)
    loaded = subprocess.run(
        [sys.executable, "-c", f"import sys, optimaru.{use_case} as m; print(m.__name__ + '.models' in sys.modules)"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert loaded == str(use_case not in NO_MODELS_ON_IMPORT)
    if hasattr(module, "with_initial_solution"):
        assert module.with_initial_solution is module.models.with_initial_solution
