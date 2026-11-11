"""The contract docs/use-cases/README.md states under "One way to solve"."""

import inspect
import subprocess
import sys
import time
import typing

import pytest
from pydantic import BaseModel

import optimaru
from optimaru._models import SolveRequest

from ._build import use_case_module
from ._problems import levels_of, score_of, small_problem

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

# Their pydantic layer models a whole request: solve() takes the compiled problem only.
REQUEST_MODEL_USE_CASES = ["employee_scheduling", "order_picking", "pickup_delivery", "vehicle_routing"]

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


@pytest.mark.parametrize("use_case", REQUEST_MODEL_USE_CASES)
def test_solve_points_a_request_model_at_solve_request(use_case):
    with pytest.raises(TypeError, match="solve_request"):
        use_case_module(use_case).solve(object())


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
    assert loaded == "True"
    if hasattr(module, "with_initial_solution"):
        assert module.with_initial_solution is module.models.with_initial_solution


# The request fields every use case's models.solve_request reads, named like solve()'s keywords.
REQUEST_FIELDS = {
    "name",
    "time_limit_seconds",
    "random_seed",
    "parallel_solver_count",
    "stop_at_first_feasible",
    "full_assert",
    "solver_parameters",
}


def _request_class(use_case):
    solve_request = use_case_module(use_case).models.solve_request
    first = next(iter(inspect.signature(solve_request).parameters))
    return typing.get_type_hints(solve_request)[first]


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_every_use_case_solves_a_pydantic_request_with_solve_request(use_case):
    """A compiled problem goes to solve(), a pydantic request to solve_request(): both in every
    use case, the package exporting both, and the request carrying the settings under the
    names of solve()'s keywords."""
    module = use_case_module(use_case)
    assert {"solve", "solve_request"} <= set(module.__all__)
    assert module.solve_request is module.models.solve_request
    request = _request_class(use_case)
    assert issubclass(request, SolveRequest)
    assert REQUEST_FIELDS <= request.model_fields.keys()
    shared = REQUEST_FIELDS - {"name", "solver_parameters"}
    assert shared <= SHARED_KEYWORDS
    parameters = inspect.signature(module.solve_request).parameters
    assert "constraints" in parameters and parameters["constraints"].kind is inspect.Parameter.KEYWORD_ONLY


def test_the_earlier_request_field_names_are_aliases():
    """timelimit and num_workers are read as time_limit_seconds and parallel_solver_count, in JSON,
    as keywords and as attributes; a model dumps the new names."""
    models = use_case_module("vehicle_routing").models
    RoutePlanRequest, RoutingInput = models.RoutePlanRequest, models.RoutingInput

    data = {"model_input": RoutingInput(vehicles=[], visits=[]).model_dump(), "timelimit": 2, "num_workers": 3}
    request = RoutePlanRequest.model_validate(data)
    assert (request.time_limit_seconds, request.parallel_solver_count) == (2, 3)
    assert (request.timelimit, request.num_workers) == (2, 3)
    assert RoutePlanRequest.model_validate_json(request.model_dump_json()) == request
    assert {"time_limit_seconds", "parallel_solver_count"} <= request.model_dump().keys()
    request.timelimit = 5
    assert request.time_limit_seconds == 5


def test_an_employee_scheduling_seed_in_settings_still_counts():
    """settings.random_seed and settings.stop_at_first_feasible are the request's fields under
    another name: the top-level one wins once it is given."""
    ScheduleRequest = use_case_module("employee_scheduling").models.ScheduleRequest

    model_input = small_request_input("employee_scheduling")
    request = ScheduleRequest(model_input=model_input, settings={"random_seed": 7})
    assert request.random_seed == 7 and request._effective("random_seed") == 7
    request.settings.random_seed = 9
    assert request._effective("random_seed") == 9
    request.random_seed = 3
    assert request._effective("random_seed") == 3


@pytest.mark.parametrize("mode", ["canonical", "quickstart"])
def test_a_school_timetabling_request_is_the_dict_solve(mode):
    """TimetableRequest -> solve(dict, ...) -> TimetableResultModel, in both modes: the model's
    JSON is a problem the converters read, and the result dict has no key the model refuses."""
    module = use_case_module("school_timetabling")
    problem = small_problem("school_timetabling")
    request = module.models.TimetableRequest(
        model_input=problem,
        mode=mode,
        time_limit_seconds=60.0,
        random_seed=3,
        solver_parameters={"step_count_limit": 300},
    )
    before = request.model_dump()
    solved = module.solve_request(request)
    config = (module.canon.SolverConfig if mode == "canonical" else module.SolverConfig)()
    config.step_count_limit = 300
    direct = module.solve(problem, mode=mode, time_limit_seconds=60.0, random_seed=3, config=config)
    assert isinstance(solved, module.models.TimetableResultModel)
    assert (solved.status, solved.penalties, solved.objective_value) == (
        direct["status"],
        direct["penalties"],
        direct["objective_value"],
    )
    assert len(solved.lessons) == len(direct["lessons"])
    assert request.model_dump() == before
    resumed = module.solve_request(request.model_copy(update={"initial_solution": solved, "time_limit_seconds": 0.0}))
    assert resumed.status == solved.status


def small_request_input(use_case):
    from optimaru.employee_scheduling.demo import generate_demo_data

    return generate_demo_data(workers=6, days=4).model_input


MODEL_USE_CASES = sorted(set(optimaru.__all__) - set(REQUEST_MODEL_USE_CASES) - {"school_timetabling"})


@pytest.mark.parametrize("use_case", MODEL_USE_CASES)
def test_solve_request_is_solve_with_the_request_settings(use_case):
    """Where the request wraps the problem model, solve_request(request) is solve(model, ...) with
    the request's settings as the keywords: the same step-limited result, the request unchanged."""
    module = use_case_module(use_case)
    problem = small_problem(use_case)
    model = problem if isinstance(problem, BaseModel) else _model_of(module, problem)
    request = _request_class(use_case)(
        model_input=model, time_limit_seconds=60.0, random_seed=3, solver_parameters={"step_count_limit": 500}
    )
    before = request.model_dump()
    solved = module.solve_request(request)
    config = module.SolverConfig()
    config.step_count_limit = 500
    direct = module.solve(model, time_limit_seconds=60.0, random_seed=3, config=config)
    assert type(solved) is type(direct)
    assert score_of(solved) == score_of(direct)
    assert request.model_dump() == before


def _model_of(module, problem):
    model_class = _request_class(module.__name__.rsplit(".", 1)[1]).model_fields["model_input"].annotation
    return model_class.from_core(problem)
