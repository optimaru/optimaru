"""Demo instance solve: reaches feasible (hard 0) and keeps assignments/pins."""

import conference_scheduling as cs


def test_demo_data_shape():
    schedule = cs.demo_conference()
    assert schedule.talk_count() == 15
    assert schedule.timeslot_count() == 6
    assert schedule.room_count() == 5
    assert len(schedule.speakers) == 12
    # Reproduces the java.util.Random(0) draws: S01's theme/content tags are deterministic.
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
    b = cs.compute_breakdown(schedule)
    assert b.talk_type_compatibility == 0
    assert b.room_conflict == 0
    assert b.speaker_conflict == 0


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
