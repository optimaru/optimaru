"""High-level solve(): canonical (default) / quickstart modes and the config/weights arguments."""

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

    # H3/H4: a class / a teacher (team-teaching co-teachers included) has one lesson at a time.
    class_slots = set()
    teacher_slots = set()
    for lesson in result["lessons"]:
        slot = (lesson["slot"]["day"], lesson["slot"]["period"])
        assert (lesson["class_group_id"], slot) not in class_slots
        class_slots.add((lesson["class_group_id"], slot))
        for teacher_id in lesson["teacher_ids"]:
            assert (teacher_id, slot) not in teacher_slots
            teacher_slots.add((teacher_id, slot))

    # H5 extension: Kim (T_KOR) is unavailable Monday period 1.
    for lesson in result["lessons"]:
        if "T_KOR" in lesson["teacher_ids"]:
            assert (lesson["slot"]["day"], lesson["slot"]["period"]) != (0, 1)

    # H1: the periods of every course are kept.
    per_course: dict[str, int] = {}
    for lesson in result["lessons"]:
        per_course[lesson["course_id"]] = per_course.get(lesson["course_id"], 0) + 1
    for course in problem["courses"]:
        assert per_course[course["id"]] == course["periods_per_week"]

    # H6: every special-room lesson gets a room (on hard 0 the greedy pass always succeeds).
    for lesson in result["lessons"]:
        if lesson["subject_id"] == "SCI":
            assert lesson["room_id"] == "R_SCI"
        elif lesson["subject_id"] == "PE":
            assert lesson["room_id"] == "R_GYM"
        else:
            assert lesson["room_id"] is None

    # penalties are the raw S1–S6 counts, objective_value the sum weighted by the defaults.
    assert set(result["penalties"]) == {
        "teacher_gap",
        "teacher_workday",
        "teacher_free_day",
        "class_gap",
        "avoided_period",
        "non_preferred_period",
    }
    default_weights = {
        "teacher_gap": 20,
        "teacher_workday": 3,
        "teacher_free_day": 30,
        "class_gap": 10,
        "avoided_period": 5,
        "non_preferred_period": 1,
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
    weights = {"teacher_gap": 0, "teacher_workday": 1, "class_gap": 2}
    result = st.solve(
        problem,
        time_limit_seconds=5.0,
        random_seed=3,
        stop_at_first_feasible=True,
        weights=weights,
    )
    # Keys not given keep the cpsat defaults.
    effective = {
        "teacher_gap": 0,
        "teacher_workday": 1,
        "teacher_free_day": 30,
        "class_gap": 2,
        "avoided_period": 5,
        "non_preferred_period": 1,
    }
    assert result["objective_value"] == sum(effective[name] * count for name, count in result["penalties"].items())

    with pytest.raises(ValueError, match="canonical"):
        st.solve(problem, mode="quickstart", weights=weights)
    with pytest.raises(ValueError, match="mode"):
        st.solve(problem, mode="cpsat")
