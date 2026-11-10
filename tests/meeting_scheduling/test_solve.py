"""Demo solve: the demo instance reaches feasible, pinned assignments are kept
and parallel starts run."""

import time

import pytest

import optimaru.meeting_scheduling as ms
from optimaru.meeting_scheduling.models import MeetingScheduleModel, demo_data


def test_demo_solve_reaches_feasible():
    model = demo_data()
    result = ms.solve(model, time_limit_seconds=10.0, random_seed=42, stop_at_first_feasible=True)
    assert isinstance(result, MeetingScheduleModel)
    assert result.score_hard == 0  # feasible
    assert all(a.grain_room is not None for a in result.meeting_assignments)
    # Assignment values must be inside the encoding range: [0, 160 * 3).
    assert all(0 <= a.grain_room < 160 * 3 for a in result.meeting_assignments)
    assert result.score_soft <= 0


def test_solve_core_in_place():
    core = ms.demo_data()
    result = ms.solve(core, time_limit_seconds=5.0, random_seed=1, stop_at_first_feasible=True)
    assert result is core
    assert core.score.is_feasible()
    assert all(v >= 0 for v in core.assignment())


def test_pinned_assignment_kept():
    core = ms.demo_data()
    pinned_value = core.grain_room_of(0, 2)  # first grain, Room 3
    assignments = core.meeting_assignments  # copy
    assignments[0].grain_room = pinned_value
    assignments[0].pinned = True
    core.meeting_assignments = assignments  # takes effect only when the whole list is written back
    ms.solve(core, time_limit_seconds=5.0, random_seed=7, stop_at_first_feasible=True)
    assert core.meeting_assignments[0].grain_room == pinned_value
    assert core.meeting_assignments[0].pinned is True
    assert core.score.hard == 0


def test_stop_at_first_feasible_and_callback():
    events = []
    result = ms.solve(
        demo_data(),
        time_limit_seconds=10.0,
        random_seed=3,
        stop_at_first_feasible=True,
        on_new_best=lambda step, score: events.append((step, score.hard)),
    )
    assert result.score_hard == 0
    assert events, "the on_new_best callback was never called"


def test_a_zero_soft_score_is_positive_zero():
    """The unsolved demo has no soft penalty: it scores 0soft, not -0soft (IEEE negative zero)."""
    import math

    import optimaru.meeting_scheduling as _pkg

    score = _pkg.calculate_score(_pkg.demo_data())
    assert score.soft == 0.0
    assert math.copysign(1.0, score.soft) == 1.0
    assert "-0soft" not in repr(score)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. A limit of (0, -1e18) would never be reached by such a score.
    """
    import dataclasses

    import optimaru.meeting_scheduling as package
    from optimaru.meeting_scheduling.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        ms.demo_data(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18


def test_parallel_starts_return_the_best_single_start():
    """Start i of a multi-start solve searches with random_seed + i and the best score wins,
    so four step-limited starts return what the best of the four single-start solves does.
    A solve that ran one start whatever the count says returns the first one's score instead,
    and the first start is not the best one for every seed below."""

    def score(seed, starts):
        config = ms.SolverConfig()
        config.spent_limit_seconds = 60.0  # the step limit ends the solve
        config.step_count_limit = 2000
        config.random_seed = seed
        config.parallel_solver_count = starts
        solved = ms.solve(ms.demo_data(), config=config)
        return (solved.score.hard, solved.score.soft)

    first_was_best = []
    for seed in range(0, 60, 10):
        singles = [score(seed + start, 1) for start in range(4)]
        assert score(seed, 4) == max(singles)
        first_was_best.append(singles[0] == max(singles))
    assert not all(first_was_best)


def _boom(_step, _score):
    raise RuntimeError("boom")


def test_a_raising_on_new_best_raises_from_a_multi_start_solve():
    """Four parallel starts: the callback's exception reaches Python instead of aborting the
    process, and the other starts stop at once instead of running out their 30 s budget."""
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        ms.solve(ms.demo_data(), time_limit_seconds=30.0, parallel_solver_count=4, on_new_best=_boom)
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path would call std::terminate."""
    from optimaru.meeting_scheduling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            ms.solve(ms.demo_data(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)
