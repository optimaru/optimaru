"""Parity gate: st::canon engine score == cpsat validate.py (ground truth).

The same canonical problem is (i) solved with CP-SAT (teacher_scheduling), the
solution injected into the st::canon domain to compute the engine's raw
constraint costs (`constraint_costs`), and (ii) compared with cpsat's
validate_timetable (the hard constraints) + compute_penalties (the soft ones,
keyed by the same *_cost names).

Gate conditions (per problem):
- hard-violation counts match exactly: the cpsat solution has 0 violations
  under validate, and every engine hard raw cost is 0 too.
- raw soft counts match key by key.
- weighted penalty sums match exactly: the engine's score of the soft costs at
  `weights` == validate's weighted sum == the objective_value cpsat reports.

The reverse direction (engine solve -> cpsat validate) is checked as well, so
the semantics of the whole pipeline, conversion and reverse conversion
included, are pinned down.
"""

import pytest
import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import UNASSIGNED_LESSONS_SOFT_WEIGHT, Level, UnassignedLessonsCost, get_constraints

from teacher_scheduling import SolverConfig as CpsatConfig
from teacher_scheduling import Timetable, TimetableProblem
from teacher_scheduling import solve as solve_cpsat
from teacher_scheduling.sample import build_sample_problem
from teacher_scheduling.validate import compute_penalties, validate_timetable

# Problem 3: a small instance with parallel sections (members with different preferred/avoided
# masks) + a block + forbidden times + class unavailability + a teacher consecutive cap and a free day.
_FEATURE_WEIGHTS = {
    "teacher_gap_cost": 7,
    "teacher_workday_cost": 2,
    "teacher_free_day_cost": 11,
    "class_gap_cost": 3,
    "avoided_period_cost": 5,
    "non_preferred_period_cost": 2,
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

    # Engine: inject the same solution into st::canon and read the raw constraint costs.
    converted = st.convert_problem(problem_dict)
    st.inject_solution(converted, [lesson.model_dump(mode="json") for lesson in cpsat_result.lessons])
    costs = canon.constraint_costs(converted.table)
    raw = {name: sum(entry["costs"]) for name, entry in costs.items()}

    # Hard-violation counts match exactly (a valid cpsat solution => 0 on both sides).
    assert violations == []
    assert all(raw[name] == 0 for name, entry in costs.items() if entry["hard"])

    # Soft: raw counts match per key (one name on both sides) + the weighted sums match (cpsat's
    # reported value included).
    soft = {name: int(raw[name]) for name, entry in costs.items() if entry["level"] == "soft"}
    assert soft == dict(truth_penalties)
    picks = [type(spec)(weight=weights[spec.name]) if spec.name in weights else spec for spec in get_constraints()]
    engine_soft = -st.constraints.score(costs, picks).soft
    assert engine_soft == _weighted(truth_penalties, weights)
    assert engine_soft == cpsat_result.objective_value

    # The score paths must give the same value too: the monolith at these weights, and the plugin
    # assembly at these weights.
    score = canon.calculate_score(converted.table, canon.ObjectiveWeights(**weights))
    assert score.hard == 0
    assert -score.soft == engine_soft
    assert -canon.DecomposedCalculator(picks).score(converted.table).soft == engine_soft


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
    """Partial timetabling (unassigned lessons relaxed) follows the same contract — cpsat solve_partial is the
    ground truth.

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
        constraints=[
            *get_constraints()[1:],
            UnassignedLessonsCost(level="soft", weight=UNASSIGNED_LESSONS_SOFT_WEIGHT),
        ],
        time_limit_seconds=10.0,
        random_seed=7,
    )

    # Unassigned lessons are a result, not a search failure; both sides report them under one name.
    assert result["status"] == "FEASIBLE"
    assert result["penalties"]["unassigned_lessons_cost"] == required - len(result["lessons"])
    assert optimum.penalties["unassigned_lessons_cost"] == required - len(optimum.lessons)
    # It cannot fill more than the optimum (cpsat proves optimality).
    assert 0 < len(result["lessons"]) <= len(optimum.lessons)

    # The placed part respects every hard constraint — viewed as the problem with the
    # weekly periods reduced to the placed counts, cpsat validate must find no
    # violation (the same as checking everything but unassigned_lessons_cost).
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


def test_cpsat_defaults_are_the_engine_defaults():
    """The CP-SAT registry is the engine's specs: same names, order and defaults (constraint_info())."""
    from teacher_scheduling.solver.registry import CONSTRAINT_TYPES, ENGINE_DEFAULTS, default_constraints

    assert [c.name for c in CONSTRAINT_TYPES] == list(st.list_constraints())
    for constraint_type in CONSTRAINT_TYPES:
        assert issubclass(constraint_type, getattr(st.constraints, constraint_type.__name__))
    for constraint, row in zip(default_constraints(), st.constraint_info(), strict=True):
        assert (constraint.weight, constraint.level, constraint.exponent) == (None, None, 1.0)
        assert ENGINE_DEFAULTS[constraint.name] == row
        assert constraint.effective_level == row.default_level
        assert constraint.effective_weight == row.default_weight
    assert CpsatConfig().weights.model_dump() == {
        row.name: row.default_weight for row in st.constraint_info() if row.default_level == "soft"
    }


def test_teacher_gap_made_hard_is_forbidden_on_both_paths():
    """level="hard" on a soft constraint: CP-SAT forbids every violation; the engine scores it on the hard level."""
    from helpers import NO_WEIGHTS, course, make_problem, slot, teacher

    from teacher_scheduling.domain.solution import SolveStatus
    from teacher_scheduling.solver.registry import TeacherGapCost as CpsatTeacherGapCost
    from teacher_scheduling.solver.registry import default_constraints, merge

    config = CpsatConfig(time_limit_seconds=5.0, num_workers=4, random_seed=7, weights=NO_WEIGHTS)
    constraints = merge(default_constraints(NO_WEIGHTS), CpsatTeacherGapCost(level="hard"))
    # One fixed lesson in period 1: the other must sit in period 2 (period 3 would leave a gap).
    problem = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 1)}))],
        [teacher()],
        days=1,
        periods=3,
    )
    result = solve_cpsat(problem, config, constraints=constraints)
    assert result.status.has_solution
    assert {lesson.slot.period for lesson in result.lessons} == {1, 2}
    assert "teacher_gap_cost" not in result.penalties  # a hard term reports no penalty

    # The engine scores the same timetable at hard 0 under the same selection.
    problem_dict = problem.model_dump(mode="json")
    converted = st.convert_problem(problem_dict)
    st.inject_solution(converted, [lesson.model_dump(mode="json") for lesson in result.lessons])
    selection = [
        st.TeacherGapCost(level="hard") if spec.name == "teacher_gap_cost" else spec for spec in get_constraints()
    ]
    assert canon.DecomposedCalculator(selection).score(converted.table).hard == 0

    # Fixed lessons in periods 1 and 3 leave a gap that level="hard" cannot accept.
    gapped = make_problem(
        [course(periods_per_week=2, max_periods_per_day=2, fixed_slots=frozenset({slot(0, 1), slot(0, 3)}))],
        [teacher()],
        days=1,
        periods=3,
    )
    assert solve_cpsat(gapped, config, constraints=constraints).status == SolveStatus.INFEASIBLE


# ---------------------------------------------------------------- 0.3.0: repeated picks, offset / clip


def _tiny_problem():
    """1 day x 5 periods, one teacher and class; MATH prefers periods 1-2 and avoids 5."""
    from helpers import course, make_problem, teacher

    from teacher_scheduling.domain.entities import Subject

    subjects = [Subject(id="SUB", name="SUB", preferred_periods=frozenset({1, 2}), avoided_periods=frozenset({5}))]
    return make_problem(
        [course(periods_per_week=3, max_periods_per_day=3)], [teacher()], subjects=subjects, days=1, periods=5
    )


def _engine_soft(problem: TimetableProblem, result: Timetable, picks) -> float:
    """The engine's soft score of the CP-SAT timetable under the same picks (the CP-SAT plugins are the
    engine's specs, so they go into canon.constraint_costs as they are)."""
    problem_dict = problem.model_dump(mode="json")
    converted = st.convert_problem(problem_dict)
    st.inject_solution(converted, [lesson.model_dump(mode="json") for lesson in result.lessons])
    engine = st.constraints.score(canon.constraint_costs(converted.table, picks), picks)
    assert engine.hard == 0
    assert canon.DecomposedCalculator(picks).score(converted.table).soft == engine.soft
    return engine.soft


_REPEATS = {
    "repeat": [("teacher_gap_cost", {}), ("teacher_gap_cost", {"weight": 2})],
    "identical-twice": [("class_gap_cost", {}), ("class_gap_cost", {})],
    "threshold-and-literal": [
        ("teacher_gap_cost", {"offset": 1}),
        ("teacher_gap_cost", {"weight": 2, "offset": 1, "clip": False}),
        ("teacher_workday_cost", {"offset": -1}),
    ],
    "per-slot": [
        ("non_preferred_period_cost", {"weight": 5, "offset": 1, "clip": False}),
        ("avoided_period_cost", {"weight": 7, "offset": -1}),
        ("avoided_period_cost", {"weight": 3, "offset": 1, "clip": False}),
    ],
}


def _cpsat_picks(entries):
    from helpers import NO_WEIGHTS

    from teacher_scheduling.solver.registry import CONSTRAINT_TYPE_BY_NAME, default_constraints

    hard = [c for c in default_constraints(NO_WEIGHTS) if c.effective_level == "hard"]
    return [*hard, *(CONSTRAINT_TYPE_BY_NAME[name](**knobs) for name, knobs in entries)]


@pytest.mark.parametrize("entries", list(_REPEATS.values()), ids=list(_REPEATS))
def test_repeated_and_shifted_picks_score_like_the_engine(entries):
    """Every entry adds its own (shaped) objective term: the CP-SAT objective_value is the
    engine's soft score of the same timetable under the same picks, and the optimum."""
    from itertools import combinations

    from teacher_scheduling.domain.solution import ScheduledLesson, SolveStatus

    problem = _tiny_problem()
    picks = _cpsat_picks(entries)
    config = CpsatConfig(time_limit_seconds=10.0, num_workers=1, random_seed=7)
    result = solve_cpsat(problem, config, constraints=picks)
    assert result.status == SolveStatus.OPTIMAL
    assert -_engine_soft(problem, result, picks) == result.objective_value
    # Brute force over the 10 ways to place 3 periods in 5: CP-SAT's optimum is the engine's best.
    course = problem.courses[0]
    best = None
    for periods in combinations(range(1, 6), 3):
        lessons = tuple(
            ScheduledLesson(
                course_id=course.id,
                class_group_id=course.class_group_id,
                subject_id=course.subject_id,
                teacher_ids=course.teacher_ids,
                slot=lesson.slot.model_copy(update={"period": period}),
            )
            for lesson, period in zip(result.lessons, periods, strict=True)
        )
        soft = _engine_soft(problem, Timetable(status=SolveStatus.FEASIBLE, lessons=lessons), picks)
        best = soft if best is None else max(best, soft)
    assert -best == result.objective_value
    # Each name's raw count is reported once, under its name.
    names = {name for name, _ in entries}
    assert set(result.penalties) == names


def _engine_score(problem: TimetableProblem, result: Timetable, picks) -> tuple[int, float]:
    """The engine's (hard, soft) of a timetable under `picks`: the calculator and the Python
    mirror agree on both levels."""
    converted = st.convert_problem(problem.model_dump(mode="json"))
    st.inject_solution(converted, [lesson.model_dump(mode="json") for lesson in result.lessons])
    mirror = st.constraints.score(canon.constraint_costs(converted.table, picks), picks)
    engine = canon.DecomposedCalculator(picks).score(converted.table)
    assert (engine.hard, engine.soft) == (mirror.hard, mirror.soft)
    return mirror.hard, mirror.soft


def test_each_repeat_keeps_its_own_level_on_cpsat():
    """One constraint on two levels: avoided_period_cost hard (CP-SAT forbids period 5, the
    engine charges it on the hard level) plus a soft literal repeat; teacher_gap_cost twice
    with offsets. CP-SAT's optimum is the engine's best feasible timetable, mirror included."""
    from itertools import combinations

    from teacher_scheduling.domain.solution import ScheduledLesson, SolveStatus

    entries = [
        ("avoided_period_cost", {"level": "hard"}),
        ("avoided_period_cost", {"weight": 3, "offset": 1, "clip": False}),
        ("non_preferred_period_cost", {"weight": 5, "offset": -1}),
        ("teacher_gap_cost", {"weight": 2, "offset": 1, "clip": False}),
        ("teacher_gap_cost", {"level": Level.SOFT}),
    ]
    problem = _tiny_problem()
    picks = _cpsat_picks(entries)
    assert [pick.effective_level for pick in picks[-5:]] == ["hard", "soft", "soft", "soft", "soft"]
    config = CpsatConfig(time_limit_seconds=10.0, num_workers=1, random_seed=7)
    result = solve_cpsat(problem, config, constraints=picks)
    assert result.status == SolveStatus.OPTIMAL
    hard, soft = _engine_score(problem, result, picks)
    assert hard == 0 and -soft == result.objective_value
    course = problem.courses[0]
    scores = []
    for periods in combinations(range(1, 6), 3):
        lessons = tuple(
            ScheduledLesson(
                course_id=course.id,
                class_group_id=course.class_group_id,
                subject_id=course.subject_id,
                teacher_ids=course.teacher_ids,
                slot=lesson.slot.model_copy(update={"period": period}),
            )
            for lesson, period in zip(result.lessons, periods, strict=True)
        )
        scores.append(_engine_score(problem, Timetable(status=SolveStatus.FEASIBLE, lessons=lessons), picks))
    assert min(hard for hard, _ in scores) < 0  # period 5 costs on the hard level
    assert max(scores) == (0, soft)


@pytest.mark.parametrize("level", [Level.MEDIUM, "medium"])
def test_medium_is_refused_by_cpsat(level):
    """No medium level in this use case: the CP-SAT plugins refuse it like the engine, per instance."""
    from teacher_scheduling.solver.registry import TeacherGapCost as CpsatTeacherGapCost

    problem = _tiny_problem()
    config = CpsatConfig(time_limit_seconds=1.0, num_workers=1)
    picks = [*_cpsat_picks([]), CpsatTeacherGapCost(), CpsatTeacherGapCost(level=level)]
    with pytest.raises(ValueError, match="this use case has no medium level"):
        solve_cpsat(problem, config, constraints=picks)


def test_an_offset_cpsat_cannot_shape_is_refused():
    from helpers import course, make_problem, teacher

    from teacher_scheduling.solver.registry import ClassConflictCost, TeacherGapCost, default_constraints, merge

    problem = make_problem([course(periods_per_week=1)], [teacher()], days=1, periods=2)
    config = CpsatConfig(time_limit_seconds=1.0, num_workers=1)
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve_cpsat(problem, config, constraints=merge(default_constraints(), TeacherGapCost(offset=0.5)))
    with pytest.raises(ValueError, match="offset is not supported by this CP-SAT model on the hard level"):
        solve_cpsat(problem, config, constraints=merge(default_constraints(), ClassConflictCost(offset=1)))
    with pytest.raises(ValueError, match="offset is not supported by this CP-SAT model on the hard level"):
        solve_cpsat(problem, config, constraints=merge(default_constraints(), TeacherGapCost(level="hard", offset=1)))


# ------------------------------------------- 0.3.0: clip=False with an offset stays off hard

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


@pytest.mark.parametrize("offset", [1, -1])
@pytest.mark.parametrize(
    ("name", "knobs"),
    [
        ("teacher_conflict_cost", {}),  # the default level, hard
        ("teacher_gap_cost", {"level": "hard"}),  # soft moved to hard
        ("fixed_slots_cost", {}),  # structural, never flippable
        ("parallel_groups_cost", {"level": Level.HARD}),  # structural
    ],
    ids=["teacher-conflict-default-hard", "teacher-gap-moved-to-hard", "fixed-slots", "parallel-groups"],
)
def test_the_literal_offset_is_refused_on_hard_like_the_engine(name, knobs, offset):
    """The CP-SAT plugins refuse the pick in the engine's words, before their own limits (a
    hard offset is a model constraint here), as a plugin and through the API DTOs; the
    engine and the score() mirror say the same about the same pick."""
    from helpers import course, make_problem, teacher
    from pydantic import TypeAdapter

    from teacher_scheduling.api import ConstraintDto, to_constraints
    from teacher_scheduling.solver.registry import CONSTRAINT_TYPE_BY_NAME, default_constraints, merge

    message = HARD_REFUSAL.format(q=repr(name))
    pick = CONSTRAINT_TYPE_BY_NAME[name](**knobs, offset=offset, clip=False)
    problem = make_problem([course(periods_per_week=1)], [teacher()], days=1, periods=2)
    config = CpsatConfig(time_limit_seconds=1.0, num_workers=1)
    for picks in ([pick], merge(default_constraints(), pick), [*default_constraints(), pick]):
        with pytest.raises(ValueError) as refused:
            solve_cpsat(problem, config, constraints=picks)
        assert str(refused.value) == message
    payload = [{"type": name, **{key: str(value) for key, value in knobs.items()}, "offset": offset, "clip": False}]
    (dto_pick,) = to_constraints(TypeAdapter(list[ConstraintDto]).validate_python(payload))
    with pytest.raises(ValueError) as refused:
        solve_cpsat(problem, config, constraints=[dto_pick])
    assert str(refused.value) == message
    # The engine and the Python mirror refuse the same pick in the same words.
    converted = st.convert_problem(problem.model_dump(mode="json"))
    with pytest.raises(ValueError) as refused:
        canon.DecomposedCalculator([pick])
    assert str(refused.value) == message
    with pytest.raises(ValueError) as refused:
        st.constraints.score(canon.constraint_costs(converted.table), [pick])
    assert str(refused.value) == message


@pytest.mark.parametrize("offset", [1, -1])
def test_the_same_literal_pick_on_soft_scores_like_the_engine(offset):
    """teacher_gap_cost on its default soft level takes the literal formula: CP-SAT's optimum
    is the engine's soft score of its timetable."""
    from teacher_scheduling.domain.solution import SolveStatus

    problem = _tiny_problem()
    picks = _cpsat_picks([("teacher_gap_cost", {"level": "soft", "weight": 2, "offset": offset, "clip": False})])
    config = CpsatConfig(time_limit_seconds=10.0, num_workers=1, random_seed=7)
    result = solve_cpsat(problem, config, constraints=picks)
    assert result.status == SolveStatus.OPTIMAL
    assert -_engine_soft(problem, result, picks) == result.objective_value
