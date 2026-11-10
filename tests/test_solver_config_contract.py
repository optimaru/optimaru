"""The part of SolverConfig every use case shares (optimaru::python::bind_solver_config)."""

import copy

import pytest

import optimaru

from ._build import use_case_module

SHARED_FIELDS = [
    "spent_limit_seconds",
    "unimproved_spent_limit_seconds",
    "step_count_limit",
    "unimproved_step_count_limit",
    "random_seed",
    "parallel_solver_count",
    "late_acceptance_size",
    "full_assert",
]
# No automatic late-acceptance size: 0 is refused there, and means "auto" everywhere else.
NO_AUTO_LATE_ACCEPTANCE = {"employee_scheduling"}


def _config(use_case):
    return use_case_module(use_case).SolverConfig()


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_shared_fields_exist_and_copy(use_case):
    config = _config(use_case)
    for name in SHARED_FIELDS:
        assert hasattr(config, name), name
    config.random_seed = 123
    config.full_assert = True
    config.step_count_limit = 7
    config.unimproved_step_count_limit = 5
    config.spent_limit_seconds = 1.5
    clone = copy.copy(config)
    assert (clone.random_seed, clone.full_assert) == (123, True)
    assert (clone.step_count_limit, clone.unimproved_step_count_limit, clone.spent_limit_seconds) == (7, 5, 1.5)


@pytest.mark.parametrize("use_case", optimaru.__all__)
def test_the_shared_fields_refuse_the_same_values_with_the_same_words(use_case):
    config = _config(use_case)
    with pytest.raises(ValueError, match="spent_limit_seconds must be finite and >= 0"):
        config.spent_limit_seconds = -1
    with pytest.raises(ValueError, match="unimproved_spent_limit_seconds must be at most 1e9 seconds"):
        config.unimproved_spent_limit_seconds = 1e10
    with pytest.raises(ValueError, match="parallel_solver_count must be positive"):
        config.parallel_solver_count = 0
    if use_case in NO_AUTO_LATE_ACCEPTANCE:
        with pytest.raises(ValueError, match="late_acceptance_size must be positive"):
            config.late_acceptance_size = 0
    else:
        config.late_acceptance_size = 0
        assert config.late_acceptance_size == 0
        with pytest.raises(ValueError, match=r"late_acceptance_size must be >= 0 \(0 = auto\)"):
            config.late_acceptance_size = -1
    if hasattr(config, "ruin_recreate_rewards"):
        with pytest.raises(ValueError, match="ruin_recreate_rewards must contain exactly 4 values"):
            config.ruin_recreate_rewards = [1.0]
