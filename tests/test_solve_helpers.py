"""optimaru._solve: the steps every use case's solve() shares (pure Python, no extension)."""

import copy

import pytest

from optimaru._solve import FIRST_FEASIBLE_LIMIT, solve_in_place, solver_config


class Config:
    """A stand-in for a compiled SolverConfig: the fields solver_config() writes."""

    def __init__(self):
        self.spent_limit_seconds = 30.0
        self.random_seed = 0
        self.parallel_solver_count = 1
        self.best_score_limit = None
        self.full_assert = False
        self.on_new_best = None


def test_no_config_starts_from_the_defaults():
    config = solver_config(Config)
    assert isinstance(config, Config)
    assert vars(config) == vars(Config())


def test_overrides_go_on_a_copy_and_leave_the_callers_config_alone():
    base = Config()
    base.random_seed = 7
    before = copy.copy(vars(base))
    report = print
    config = solver_config(
        Config,
        base,
        time_limit_seconds=2.5,
        random_seed=11,
        parallel_solver_count=4,
        stop_at_first_feasible=True,
        full_assert=True,
        on_new_best=report,
    )
    assert config is not base
    assert vars(base) == before
    assert (config.spent_limit_seconds, config.random_seed, config.parallel_solver_count) == (2.5, 11, 4)
    assert config.best_score_limit == FIRST_FEASIBLE_LIMIT == (0, float("-inf"))
    assert config.full_assert is True and config.on_new_best is report


def test_a_keyword_left_out_keeps_the_base_setting():
    base = Config()
    base.spent_limit_seconds = 9.0
    base.full_assert = True
    base.best_score_limit = (0, -5.0)
    config = solver_config(Config, base)
    assert config.spent_limit_seconds == 9.0
    assert config.full_assert is True  # full_assert=False never turns it off
    assert config.best_score_limit == (0, -5.0)


def test_a_use_case_with_more_score_levels_passes_its_own_first_feasible_limit():
    limit = (0, -(2**63), -(2**63))
    config = solver_config(Config, stop_at_first_feasible=True, first_feasible_limit=limit)
    assert config.best_score_limit == limit


def test_a_misspelled_keyword_is_refused():
    with pytest.raises(TypeError):
        solver_config(Config, time_limit=1.0)


class Solver:
    def __init__(self):
        self.calls = []

    def solve(self, problem):
        self.calls.append(("default", problem))

    def solve_with_constraints(self, problem, constraints):
        self.calls.append(("picked", problem, constraints))


def test_solve_in_place_takes_the_default_path_or_the_picked_constraints():
    solver = Solver()
    solve_in_place(solver, "problem")
    solve_in_place(solver, "problem", ["a pick"])
    assert solver.calls == [("default", "problem"), ("picked", "problem", ["a pick"])]
