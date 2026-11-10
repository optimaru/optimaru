"""The contract docs/use-cases/README.md states under "One way to solve"."""

import inspect
import subprocess
import sys
import time

import pytest

import optimaru

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

# Its solve() takes and returns dicts, so the package does not import its pydantic models.
NO_MODELS_ON_IMPORT = {"school_timetabling"}

# solve() answers with the solution document of its API there, not with a problem that carries
# a score: an infeasible document does not say how infeasible, so two of them cannot be ordered.
NO_SCORE_ON_THE_RESULT = {"school_timetabling"}


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
