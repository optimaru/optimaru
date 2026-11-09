"""Demo instance solve: reaches feasible (hard 0) and keeps assignments/pins."""

import conference_scheduling as cs


def test_demo_data_shape():
    schedule = cs.demo_conference()
    assert schedule.talk_count() == 15
    assert schedule.timeslot_count() == 6
    assert schedule.room_count() == 5
    assert len(schedule.speakers) == 12
    # The tag draws are seeded: S01's theme/content tags are deterministic.
    assert schedule.talks[0].theme_track_tags[0] in ("Optimization", "AI", "Cloud")
    codes = [t.code for t in schedule.talks]
    assert codes[0] == "S01" and codes[-1] == "S15"
    # S05 has S02 as a prerequisite.
    assert schedule.talks[4].prerequisite_talks == [1]
    # S11 requires a Recorded room.
    assert schedule.talks[10].required_room_tags == ["Recorded"]


def test_demo_solve_feasible():
    schedule = cs.demo_conference()
    best = []
    cs.solve(
        schedule,
        time_limit_seconds=10.0,
        random_seed=42,
        on_new_best=lambda step, score: best.append((step, score.hard)),
    )
    assert schedule.score.is_feasible(), str(schedule.score)
    assert schedule.score.hard == 0
    assert all(v >= 0 for v in schedule.assignment())
    assert best, "on_new_best should fire at least once"
    # The final score matches a full recompute.
    recomputed = cs.calculate_score(schedule)
    assert recomputed.hard == schedule.score.hard
    assert recomputed.soft == schedule.score.soft
    # Type compatibility: Lab talks sit only in Lab slots/rooms (what synthetic hard 0 means).
    raw = cs.constraint_costs(schedule)
    assert raw["talk_type_incompatibility_cost"]["costs"] == [0]
    assert raw["room_conflict_cost"]["costs"] == [0]
    assert raw["speaker_conflict_cost"]["costs"] == [0]


def test_pinned_talk_survives_solve():
    schedule = cs.demo_conference()
    pinned_value = schedule.slot_room_of(0, 4)  # S01 (Lab) -> T1 x Room E
    talks = schedule.talks
    talks[0].slot_room = pinned_value
    talks[0].pinned = True
    schedule.talks = talks

    cs.solve(schedule, time_limit_seconds=2.0, random_seed=1, stop_at_first_feasible=True)
    assert schedule.talks[0].slot_room == pinned_value
    assert schedule.score.hard == 0


def test_solve_config_mirror():
    from conference_scheduling.models import SolveConfig

    cfg = SolveConfig(time_limit_seconds=1.0, random_seed=7, stop_at_first_feasible=True)
    schedule = cs.demo_conference()
    cs.solve(schedule, config=cfg)
    assert schedule.score.hard == 0


def test_parallel_solver_count_workers():
    """parallelSolverCount>1 is handled by a domain-level fan-out rather than the engine's
    built-in multiStart (the domain drives the restartSegments budget). Checks that each
    worker's solve ends normally and the best is adopted."""
    for workers in (2, 4):
        schedule = cs.demo_conference()
        cs.solve(
            schedule,
            time_limit_seconds=2.0,
            random_seed=42,
            parallel_solver_count=workers,
            stop_at_first_feasible=True,
        )
        assert schedule.score.hard == 0
        recomputed = cs.calculate_score(schedule)
        assert recomputed.hard == schedule.score.hard
        assert recomputed.soft == schedule.score.soft


def test_on_new_best_fires_on_the_constraints_path():
    """solve(constraints=...) reports progress like the default path (the callback was dropped)."""
    from conference_scheduling.constraints import RoomConflictCost, SpeakerConflictCost, get_constraints

    for picks in (get_constraints(), [RoomConflictCost(), SpeakerConflictCost(weight=3)]):
        calls = []
        cs.solve(
            cs.demo_conference(),
            time_limit_seconds=0.5,
            random_seed=1,
            constraints=picks,
            on_new_best=lambda step, score, calls=calls: calls.append(step),
        )
        assert calls, "on_new_best should fire on the constraints= path"


def test_a_raising_on_new_best_stops_every_parallel_worker():
    """Four workers and a 30 s budget: the callback's exception (worker 0) stops the other
    workers too, so it reaches the caller at once instead of after their whole budget."""
    import time

    import pytest

    def boom(_step, _score):
        raise RuntimeError("boom")

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        cs.solve(
            cs.demo_conference(),
            time_limit_seconds=30.0,
            parallel_solver_count=4,
            on_new_best=boom,
        )
    assert time.monotonic() - started < 10.0


def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting():
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    import time

    import pytest

    from conference_scheduling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            cs.solve(cs.demo_conference(), time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total():
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. The first 0.2.0 limit, (0, -1e18), was never reached by such a score.
    """
    import dataclasses
    import time

    import conference_scheduling as package
    from conference_scheduling.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        cs.demo_conference(),
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
