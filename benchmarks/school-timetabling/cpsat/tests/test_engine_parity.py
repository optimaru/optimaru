"""Parity gate: st::canon engine score == cpsat validate.py (ground truth).

The same canonical problem is (i) solved with CP-SAT (teacher_scheduling), the
solution injected into the st::canon domain to compute the engine breakdown,
and (ii) compared with cpsat's validate_timetable (hard H1–H10) +
compute_penalties (soft S1–S6).

Gate conditions (per problem):
- hard-violation counts match exactly: the cpsat solution has 0 violations
  under validate, and the engine's hard_total() is 0 too.
- raw soft counts (S1–S6) match key by key.
- weighted penalty sums match exactly: engine soft_penalty(weights) ==
  validate's weighted sum == the objective_value cpsat reports.

The reverse direction (engine solve -> cpsat validate) is checked as well, so
the semantics of the whole pipeline, conversion and reverse conversion
included, are pinned down.
"""

import pytest

pytest.importorskip("teacher_scheduling")

from teacher_scheduling import SolverConfig as CpsatConfig
from teacher_scheduling import Timetable, TimetableProblem
from teacher_scheduling import solve as solve_cpsat
from teacher_scheduling.sample import build_sample_problem
from teacher_scheduling.validate import compute_penalties, validate_timetable

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import Assembly

# Problem 3: a small instance with parallel sections (members with different preferred/avoided
# masks) + a block + forbidden times + class unavailability + a teacher consecutive cap and a free day.
_FEATURE_WEIGHTS = {
    "teacher_gap": 7,
    "teacher_workday": 2,
    "teacher_free_day": 11,
    "class_gap": 3,
    "avoided_period": 5,
    "non_preferred_period": 2,
}


def feature_problem() -> dict:
    return {
        "calendar": {"periods_per_day": {"0": 5, "1": 5, "2": 5, "3": 5, "4": 5}},
        "subjects": [
            {"id": "MATH", "name": "Math", "preferred_periods": [1, 2]},
            {"id": "KOR", "name": "Korean"},
            {"id": "SCI", "name": "Science", "required_room_type": "science lab", "avoided_periods": [5]},
            {"id": "TECH", "name": "Technology", "required_room_type": "tech room", "avoided_periods": [4]},
            {"id": "HOME", "name": "Home economics", "preferred_periods": [3, 4]},
            {"id": "ART", "name": "Art", "avoided_periods": [1]},
            {"id": "HR", "name": "Homeroom"},
        ],
        "teachers": [
            {"id": "T1", "name": "Kim (Math)", "max_consecutive_periods": 2},
            {"id": "T2", "name": "Lee (Korean)", "unavailable": [{"day": 0, "period": 1}], "preferred_free_days": [4]},
            {"id": "T3", "name": "Park (Science)"},
            {"id": "T4", "name": "Kwon (Technology)"},
            {"id": "T5", "name": "Song (Home economics)"},
            {"id": "T6", "name": "Koo (Art)"},
            {"id": "T7", "name": "Homeroom teacher"},
        ],
        "class_groups": [{"id": "C1", "name": "Grade 1 Class 1", "grade": 1, "unavailable": [{"day": 4, "period": 5}]}],
        "rooms": [
            {"id": "R_SCI", "name": "Science lab", "room_type": "science lab"},
            {"id": "R_TECH", "name": "Tech room", "room_type": "tech room"},
        ],
        "courses": [
            {
                "id": "C1-MATH",
                "class_group_id": "C1",
                "subject_id": "MATH",
                "teacher_ids": ["T1"],
                "periods_per_week": 4,
            },
            {
                "id": "C1-KOR",
                "class_group_id": "C1",
                "subject_id": "KOR",
                "teacher_ids": ["T2"],
                "periods_per_week": 3,
                "forbidden_slots": [{"day": 1, "period": 1}],
            },
            # Team teaching: two teachers together.
            {
                "id": "C1-SCI",
                "class_group_id": "C1",
                "subject_id": "SCI",
                "teacher_ids": ["T3", "T7"],
                "periods_per_week": 3,
            },
            # Parallel sections: two courses with different avoided/preferred masks at the same time.
            {
                "id": "C1-TECH",
                "class_group_id": "C1",
                "subject_id": "TECH",
                "teacher_ids": ["T4"],
                "periods_per_week": 2,
                "parallel_group": "C1-split",
            },
            {
                "id": "C1-HOME",
                "class_group_id": "C1",
                "subject_id": "HOME",
                "teacher_ids": ["T5"],
                "periods_per_week": 2,
                "parallel_group": "C1-split",
            },
            # One 2-period block a week.
            {
                "id": "C1-ART",
                "class_group_id": "C1",
                "subject_id": "ART",
                "teacher_ids": ["T6"],
                "periods_per_week": 2,
                "block_periods": 2,
            },
            {
                "id": "C1-HR",
                "class_group_id": "C1",
                "subject_id": "HR",
                "teacher_ids": ["T7"],
                "periods_per_week": 1,
                "fixed_slots": [{"day": 0, "period": 1}],
            },
        ],
    }


def _weighted(penalties: dict[str, int], weights: dict[str, int]) -> int:
    return sum(weights[name] * count for name, count in penalties.items())


def _assert_parity(problem_dict: dict, cpsat_result: Timetable, weights: dict[str, int]) -> None:
    """Injects the cpsat solution into the engine and checks hard/soft against validate.py."""
    problem = TimetableProblem.model_validate(problem_dict)

    # Ground truth: cpsat validate's hard violations + raw soft counts.
    violations = validate_timetable(problem, cpsat_result)
    truth_penalties = compute_penalties(problem, cpsat_result)

    # Engine: inject the same solution into st::canon and recompute the breakdown.
    converted = st.convert_problem(problem_dict)
    converted.table.weights = canon.ObjectiveWeights(**weights)
    st.inject_solution(converted, [lesson.model_dump(mode="json") for lesson in cpsat_result.lessons])
    breakdown = canon.compute_breakdown(converted.table)

    # Hard-violation counts match exactly (a valid cpsat solution => 0 on both sides).
    assert violations == []
    assert breakdown.hard_total() == 0

    # Soft: raw counts match per key + the weighted sums match (cpsat's reported value included).
    assert dict(breakdown.penalties()) == dict(truth_penalties)
    engine_soft = breakdown.soft_penalty(canon.ObjectiveWeights(**weights))
    assert engine_soft == _weighted(truth_penalties, weights)
    assert engine_soft == cpsat_result.objective_value

    # The score path must give the same value too (calculate_score == breakdown).
    score = canon.calculate_score(converted.table)
    assert score.hard == 0
    assert -score.soft == engine_soft


def _solve_and_assert(problem_dict: dict, *, weights: dict[str, int] | None = None, time_limit: float = 10.0) -> None:
    weights = weights or CpsatConfig().weights.model_dump()
    config = CpsatConfig(time_limit_seconds=time_limit, num_workers=8, random_seed=7, weights=weights)
    result = solve_cpsat(TimetableProblem.model_validate(problem_dict), config)
    assert result.status.has_solution
    _assert_parity(problem_dict, result, weights)


def test_parity_sample_problem():
    """The cpsat demo school (34 cells fully packed: blocks, parallel sections, team teaching, fixed times, caps)."""
    _solve_and_assert(build_sample_problem().model_dump(mode="json"))


def test_parity_demo_fixture(problem):
    """The python demo problem (20 cells fully packed: team teaching, special rooms, fixed and unavailable times)."""
    _solve_and_assert(problem, time_limit=10.0)


def test_parity_feature_problem_with_custom_weights():
    """Small all-features instance + non-default weights — checks weighted-sum parity too."""
    _solve_and_assert(feature_problem(), weights=_FEATURE_WEIGHTS, time_limit=10.0)


def test_parity_partial_solution(problem):
    """Partial timetabling (H1 relaxed) follows the same contract — cpsat solve_partial is the ground truth.

    The unassigned count cannot be matched (local search proves no optimality).
    What is pinned down is the **semantics**: unassigned lessons are not a hard
    violation (FEASIBLE), the placed part respects every hard constraint, and the
    unassigned periods are reported as the penalty term with cpsat's name — they
    can be worse than cpsat's optimum by that much, never better.
    """
    from teacher_scheduling.partial import solve_partial

    problem["calendar"]["periods_per_day"]["4"] = 0  # remove Friday: 20 periods / 16 cells
    required = sum(course["periods_per_week"] for course in problem["courses"])

    # Ground truth: the optimal partial solution of cpsat's relaxed mode.
    optimum = solve_partial(
        TimetableProblem.model_validate(problem),
        CpsatConfig(time_limit_seconds=10.0, num_workers=8, random_seed=7),
    )
    assert optimum.status.has_solution

    result = st.solve(
        problem,
        constraints=Assembly().add("weekly_period_counts", hard=False, weight=10_000),
        time_limit_seconds=10.0,
        random_seed=7,
    )

    # Unassigned lessons are a result, not a search failure.
    assert result["status"] == "FEASIBLE"
    assert result["penalties"]["weekly_period_counts"] == required - len(result["lessons"])
    # It cannot fill more than the optimum (cpsat proves optimality).
    assert 0 < len(result["lessons"]) <= len(optimum.lessons)

    # The placed part respects every hard constraint — viewed as the problem with the
    # weekly periods reduced to the placed counts, cpsat validate must find no
    # violation (the same as checking everything but H1).
    timetable = Timetable.model_validate(result)
    scheduled: dict[str, int] = {}
    for lesson in timetable.lessons:
        scheduled[lesson.course_id] = scheduled.get(lesson.course_id, 0) + 1
    shrunk = {
        **problem,
        "courses": [c | {"periods_per_week": n} for c in problem["courses"] if (n := scheduled.get(c["id"], 0))],
    }
    assert validate_timetable(TimetableProblem.model_validate(shrunk), timetable) == []


def test_parity_engine_solution_backwards():
    """Reverse direction: re-validate the engine's canonical solve result with cpsat validate."""
    problem_dict = build_sample_problem().model_dump(mode="json")
    result = st.solve(
        problem_dict,
        time_limit_seconds=15.0,
        random_seed=42,
        parallel_solver_count=4,
        stop_at_first_feasible=True,  # parity does not depend on quality — the first hard 0 is enough
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []

    problem = TimetableProblem.model_validate(problem_dict)
    timetable = Timetable.model_validate(result)
    assert validate_timetable(problem, timetable) == []

    truth_penalties = compute_penalties(problem, timetable)
    assert dict(result["penalties"]) == dict(truth_penalties)
    default_weights = CpsatConfig().weights.model_dump()
    assert result["objective_value"] == _weighted(truth_penalties, default_weights)
