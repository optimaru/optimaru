"""The contract docs/use-cases/README.md states under "One way to solve"."""

import inspect

import pytest

import optimaru

from ._build import use_case_module

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
