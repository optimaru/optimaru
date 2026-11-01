"""High-level solve(): canonical (default) / quickstart modes and the config/weights arguments."""

import time

import pytest

import st_timetabling as st
from st_timetabling.models import SolveConfig


def test_demo_problem_solves_feasible(problem):
    result = st.solve(
        problem,
        time_limit_seconds=20.0,
        random_seed=42,
        stop_at_first_feasible=True,
    )

    assert result["status"] == "FEASIBLE"
    assert result["wall_time_seconds"] > 0.0
    assert result["best_objective_bound"] is None
    assert len(result["lessons"]) == 40
    # Canonical mode has full parity, so there are no 'unsupported' notes.
    assert result["notes"] == []

    # class_conflict_cost / teacher_conflict_cost: a class / a teacher (team-teaching co-teachers
    # included) has one lesson at a time.
    class_slots = set()
    teacher_slots = set()
    for lesson in result["lessons"]:
        slot = (lesson["slot"]["day"], lesson["slot"]["period"])
        assert (lesson["class_group_id"], slot) not in class_slots
        class_slots.add((lesson["class_group_id"], slot))
        for teacher_id in lesson["teacher_ids"]:
            assert (teacher_id, slot) not in teacher_slots
            teacher_slots.add((teacher_id, slot))

    # disallowed_slot_cost: Kim (T_KOR) is unavailable Monday period 1.
    for lesson in result["lessons"]:
        if "T_KOR" in lesson["teacher_ids"]:
            assert (lesson["slot"]["day"], lesson["slot"]["period"]) != (0, 1)

    # unassigned_lessons_cost: the periods of every course are kept.
    per_course: dict[str, int] = {}
    for lesson in result["lessons"]:
        per_course[lesson["course_id"]] = per_course.get(lesson["course_id"], 0) + 1
    for course in problem["courses"]:
        assert per_course[course["id"]] == course["periods_per_week"]

    # room_capacity_cost: every special-room lesson gets a room (on hard 0 the greedy pass always succeeds).
    for lesson in result["lessons"]:
        if lesson["subject_id"] == "SCI":
            assert lesson["room_id"] == "R_SCI"
        elif lesson["subject_id"] == "PE":
            assert lesson["room_id"] == "R_GYM"
        else:
            assert lesson["room_id"] is None

    # penalties are the raw soft counts keyed by the soft constraint names, objective_value the
    # sum weighted by the defaults.
    assert set(result["penalties"]) == {
        "teacher_gap_cost",
        "teacher_workday_cost",
        "teacher_free_day_cost",
        "class_gap_cost",
        "avoided_period_cost",
        "non_preferred_period_cost",
    }
    default_weights = {
        "teacher_gap_cost": 20,
        "teacher_workday_cost": 3,
        "teacher_free_day_cost": 30,
        "class_gap_cost": 10,
        "avoided_period_cost": 5,
        "non_preferred_period_cost": 1,
    }
    assert result["objective_value"] == sum(
        default_weights[name] * count for name, count in result["penalties"].items()
    )


def test_solve_quickstart_mode(problem):
    result = st.solve(
        problem,
        mode="quickstart",
        time_limit_seconds=10.0,
        random_seed=42,
        stop_at_first_feasible=True,
    )
    assert result["status"] in ("FEASIBLE", "INFEASIBLE")
    assert len(result["lessons"]) == 40
    # The limits of the approximate quickstart conversion are left in notes (penalties are empty).
    assert any("soft" in note for note in result["notes"])
    assert result["penalties"] == {}


def test_solve_accepts_config_objects(problem):
    # models.SolveConfig works as config in both modes.
    model_cfg = SolveConfig(late_acceptance_size=100)
    result = st.solve(
        problem,
        time_limit_seconds=10.0,
        random_seed=1,
        stop_at_first_feasible=True,
        config=model_cfg,
    )
    assert result["status"] in ("FEASIBLE", "INFEASIBLE")
    assert len(result["lessons"]) == 40

    # A mode's _core config can be passed directly too.
    cfg = st.canon.SolverConfig()
    cfg.late_acceptance_size = 100
    result = st.solve(
        problem,
        time_limit_seconds=5.0,
        random_seed=1,
        stop_at_first_feasible=True,
        config=cfg,
    )
    assert result["status"] in ("FEASIBLE", "INFEASIBLE")

    # A config type that does not fit the mode is a clear ValueError.
    with pytest.raises(ValueError, match="config"):
        st.solve(problem, config=st.SolverConfig())
    with pytest.raises(ValueError, match="config"):
        st.solve(problem, mode="quickstart", config=st.canon.SolverConfig())


def test_solve_weights_override(problem):
    weights = {"teacher_gap_cost": 0, "teacher_workday_cost": 1, "class_gap_cost": 2}
    result = st.solve(
        problem,
        time_limit_seconds=5.0,
        random_seed=3,
        stop_at_first_feasible=True,
        weights=weights,
    )
    # Keys not given keep the component defaults.
    effective = {
        "teacher_gap_cost": 0,
        "teacher_workday_cost": 1,
        "teacher_free_day_cost": 30,
        "class_gap_cost": 2,
        "avoided_period_cost": 5,
        "non_preferred_period_cost": 1,
    }
    assert result["objective_value"] == sum(effective[name] * count for name, count in result["penalties"].items())

    with pytest.raises(ValueError, match="canonical"):
        st.solve(problem, mode="quickstart", weights=weights)
    with pytest.raises(ValueError, match="mode"):
        st.solve(problem, mode="cpsat")


def test_solve_weights_take_non_integer_values(problem):
    """The fast path's soft weights are plain numbers now (the selection path never needed integers)."""
    result = st.solve(
        problem, time_limit_seconds=2.0, random_seed=3, stop_at_first_feasible=True, weights={"teacher_gap_cost": 2.5}
    )
    defaults = dict(st.constraints.default_weights(), teacher_gap_cost=2.5)
    assert result["objective_value"] == pytest.approx(
        sum(defaults[name] * count for name, count in result["penalties"].items())
    )


@pytest.mark.parametrize("old", ["teacher_gap", "class_gap", "non_preferred_period"])
def test_solve_weights_refuse_the_old_keys(problem, old):
    with pytest.raises(ValueError, match="unknown soft weight"):
        st.solve(problem, time_limit_seconds=0.1, weights={old: 1})


def test_weights_and_constraints_do_not_mix(problem):
    """A selection carries its own weights: the fast path's weights next to it would be ignored, so they raise."""
    with pytest.raises(ValueError, match="fast path"):
        st.solve(problem, time_limit_seconds=0.1, weights={"teacher_gap_cost": 1}, constraints=st.Assembly())
    cfg = st.canon.SolverConfig()
    cfg.weights = st.canon.ObjectiveWeights(teacher_gap_cost=1)
    with pytest.raises(ValueError, match="fast path"):
        st.solve(problem, time_limit_seconds=0.1, config=cfg, constraints=st.Assembly())


def test_default_weights_go_with_constraints(problem):
    """weights= equal to the defaults changes nothing, so it is accepted next to a selection like config.weights."""
    defaults = st.constraints.default_weights()  # the six soft names, as the server sends them
    for weights in (defaults, {"teacher_gap_cost": defaults["teacher_gap_cost"]}):
        result = st.solve(problem, time_limit_seconds=0.5, random_seed=3, weights=weights, constraints=st.Assembly())
        assert result["lessons"]


@pytest.mark.parametrize("bad", [-20, float("nan"), float("inf")])
def test_solve_weights_refuse_non_finite_and_negative_values(problem, bad):
    with pytest.raises(ValueError, match="teacher_gap_cost: weight must be finite and >= 0"):
        st.solve(problem, time_limit_seconds=0.1, weights={"teacher_gap_cost": bad})


def test_solve_weights_refuse_bools(problem):
    with pytest.raises(TypeError, match="teacher_gap_cost: weight must be a number, not True"):
        st.solve(problem, time_limit_seconds=0.1, weights={"teacher_gap_cost": True})


def _boom(_step, _score):
    raise RuntimeError("boom")


@pytest.mark.parametrize("mode", ["canonical", "quickstart"])
def test_a_raising_on_new_best_raises_from_a_multi_start_solve(problem, mode):
    """Four parallel starts (canonical: its own worker pool; quickstart: the engine's
    multi-start) and a 30 s budget: the callback's exception reaches Python instead of
    aborting the process, and the other starts stop at once instead of using up the budget."""
    started = time.monotonic()
    with pytest.raises(RuntimeError, match="boom"):
        st.solve(problem, mode=mode, time_limit_seconds=30.0, parallel_solver_count=4, on_new_best=_boom)
    assert time.monotonic() - started < 10.0


@pytest.mark.parametrize("mode", ["canonical", "quickstart"])
def test_a_worker_thread_that_cannot_start_raises_instead_of_aborting(problem, mode):
    """Creating worker thread 1 fails (std::system_error, injected through the module's test
    seam): the worker already running is stopped and joined, and the error reaches the caller
    at once. A joinable std::thread destroyed on that path used to call std::terminate."""
    from st_timetabling import _core

    _core._fail_worker_thread_start(1)
    try:
        started = time.monotonic()
        with pytest.raises(RuntimeError, match="injected worker thread creation failure"):
            st.solve(problem, mode=mode, time_limit_seconds=30.0, parallel_solver_count=4)
        assert time.monotonic() - started < 10.0
    finally:
        _core._fail_worker_thread_start(-1)


def test_stop_at_first_feasible_stops_whatever_the_soft_total(problem):
    """stop_at_first_feasible compares the hard level only (best_score_limit (0, -inf)).

    Every enabled soft penalty at weight 1e21 puts the soft score far below -1e18 at the
    first feasible solution: the solve still stops there instead of running its whole
    30 s. The first 0.2.0 limit, (0, -1e18), was never reached by such a score.
    """
    import dataclasses
    import time

    import st_timetabling as package
    from st_timetabling.constraints import get_constraints

    info = {row.name: row for row in package.constraint_info()}

    def heavy(spec):
        row = info[spec.name]
        soft_penalty = row.default_level == "soft" and row.default_enabled and not row.reward
        return dataclasses.replace(spec, weight=1e21) if soft_penalty else spec

    seen = []
    started = time.monotonic()
    package.solve(
        problem,
        time_limit_seconds=30.0,
        random_seed=1,
        stop_at_first_feasible=True,
        on_new_best=lambda _step, score: seen.append(score),
        constraints=[heavy(spec) for spec in get_constraints()],
    )
    assert time.monotonic() - started < 20.0
    assert seen[-1].hard == 0 and seen[-1].soft < -1e18
