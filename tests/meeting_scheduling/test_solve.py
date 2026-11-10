"""Demo solve: the demo instance reaches feasible
and pinned assignments are kept."""

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
    import time

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
