"""Plugin constraint assembly: the assembly API + the `solve(constraints=...)` path.

The monolithic fast path (default, constraints=None) is unchanged; for the
plugin path this pins down (1) the default assembly scores like the monolith
and solve results match the canonical rescore, (2) weight overrides reach both
the search score and the result's objective_value, (3) dropping a component
works, (4) bad names raise ValueError.
"""

import random

import pytest

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import Assembly, TeacherGap, list_constraints

# cpsat ObjectiveWeights defaults (the same baseline as test_solve.py).
DEFAULT_WEIGHTS = {
    "teacher_gap": 20,
    "teacher_workday": 3,
    "teacher_free_day": 30,
    "class_gap": 10,
    "avoided_period": 5,
    "non_preferred_period": 1,
}


def _tiny_table() -> "canon.Timetable":
    """Smallest hand-scorable instance: 1 day x 4 periods, one teacher/class, 2 lessons.

    Placed in periods 1 and 3: S1 teacher_gap = 1, S2 teacher_workday = 1,
    S4 class_gap = 1, everything else 0.
    """
    lessons = [canon.Lesson(f"L{k}", 0, k, [0]) for k in range(2)]
    table = canon.Timetable([0, 0, 0, 0], [1, 2, 3, 4], lessons)
    table.teacher_count = 1
    table.class_count = 1
    table.teacher_max_per_day = [6]
    table.teacher_max_per_week = [-1]
    table.teacher_max_consecutive = [4]
    table.teacher_preferred_free_days = [0]
    table.course_daily_limit = [1, 1]
    table.set_assignment([0, 2])  # periods 1 and 3 -> one gap between them
    return table


def _weighted(penalties: dict[str, int], weights: dict[str, int]) -> float:
    return float(sum(weights[name] * count for name, count in penalties.items()))


def test_list_constraints_matches_core_registry():
    names = list_constraints()
    assert names == tuple(canon.CONSTRAINT_NAMES)
    assert len(names) == 15
    # The default Assembly holds the whole registry in the same order.
    assert tuple(spec.name for spec in Assembly()) == names


def test_default_assembly_matches_monolithic_score(problem):
    """(1) Default assembly == monolith: bit-identical scores over random assignments."""
    converted = st.convert_problem(problem)
    canon.prepare(converted.table)
    calculator = canon.default_calculator()  # all 15, cpsat default weights
    rng = random.Random(42)
    slot_count = converted.table.slot_count()
    lesson_count = len(converted.table.lessons)
    for _ in range(30):
        converted.table.set_assignment([rng.randrange(-1, slot_count) for _ in range(lesson_count)])
        mono = canon.calculate_score(converted.table)
        plugin = calculator.score(converted.table)
        assert (plugin.hard, plugin.soft) == (mono.hard, mono.soft)


def test_plugin_solve_default_assembly_parity(problem):
    """(1) Default-assembly solve: hard 0, and penalties/objective_value match
    the canonical rescore (monolithic breakdown)."""
    result = st.solve(
        problem,
        constraints=Assembly(),
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []
    assert len(result["lessons"]) == 40

    # Inject the result into a fresh conversion and rescore on the monolithic path — must match exactly.
    converted = st.convert_problem(problem)
    st.inject_solution(converted, result["lessons"])
    breakdown = canon.compute_breakdown(converted.table)
    assert breakdown.hard_total() == 0
    assert dict(breakdown.penalties()) == result["penalties"]
    assert result["objective_value"] == _weighted(result["penalties"], DEFAULT_WEIGHTS)


def test_weight_override_applies_to_score_and_result(problem):
    """(2) Weight override: reaches both the calculator score and the solve result."""
    # Hand-scored instance: teacher_gap is 1, so the weight is the cost.
    table = _tiny_table()
    default_soft = canon.default_calculator().score(table).soft

    doubled = canon.DecomposedCalculator(Assembly().add("teacher_gap", weight=40))
    score = doubled.score(table)
    assert score.hard == 0
    assert score.soft == default_soft - 20  # gap 1 x (40 - 20)
    totals = {t["name"]: t["cost"] for t in doubled.constraint_totals()}
    assert totals["S1 teacher gap"] == 40.0

    # A list of spec instances gives the same assembly.
    spec_list = canon.DecomposedCalculator([TeacherGap(weight=40)])
    assert spec_list.score(table).soft == -40.0

    # solve result: objective_value is rescored with the overridden effective weight.
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40),
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    effective = dict(DEFAULT_WEIGHTS, teacher_gap=40)
    assert result["objective_value"] == _weighted(result["penalties"], effective)


def test_drop_component(problem):
    """(3) Dropping a component: the term leaves the calculator and its rescoring weight becomes 0."""
    assembly = Assembly().drop("teacher_workday")
    assert len(assembly) == 14
    assert "teacher_workday" not in assembly
    assert "teacher_gap" in assembly

    table = _tiny_table()
    calculator = canon.DecomposedCalculator(assembly)
    score = calculator.score(table)
    names = {t["name"] for t in calculator.constraint_totals()}
    assert "S2 teacher workday" not in names
    # Exactly workday 1 x 3 less penalty than the full assembly.
    assert score.soft == canon.default_calculator().score(table).soft + 3

    result = st.solve(
        problem,
        constraints=assembly,
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    effective = dict(DEFAULT_WEIGHTS, teacher_workday=0)
    assert result["objective_value"] == _weighted(result["penalties"], effective)


# ------------------------------------------------------------------ partial timetabling (H1 relaxed)
#
# Demoting H1 to soft makes "leave it out" a search option (unassign moves).
# The same contract as cpsat's WeeklyPeriodCounts(hard=false) = solve_partial.


def _over_constrained(problem: dict) -> dict:
    """The demo school without Friday — 20 weekly periods per class but only 16 cells."""
    problem["calendar"]["periods_per_day"]["4"] = 0
    return problem


def test_relaxed_h1_returns_a_partial_timetable(problem):
    """Unfillable periods stay unassigned instead of being hard violations."""
    problem = _over_constrained(problem)
    relaxed = Assembly().add("weekly_period_counts", hard=False, weight=10_000)

    strict = st.solve(problem, time_limit_seconds=5.0, random_seed=7)
    partial = st.solve(problem, constraints=relaxed, time_limit_seconds=5.0, random_seed=7)

    # Kept hard, the periods that cannot fit remain hard violations: UNKNOWN, without proof.
    assert strict["status"] == "UNKNOWN"
    assert len(strict["lessons"]) == 40, "with H1 hard the search gives every lesson a slot"

    # Relaxed, the rest is a partial solution: status is decided without the unassigned lessons.
    assert partial["status"] == "FEASIBLE"
    assert 0 < len(partial["lessons"]) < 40
    # Unassigned periods are reported under the same name and unit as cpsat's relaxed mode.
    assert partial["penalties"]["weekly_period_counts"] == 40 - len(partial["lessons"])
    assert partial["objective_value"] == _weighted(
        partial["penalties"], dict(DEFAULT_WEIGHTS, weekly_period_counts=10_000)
    )
    # There are only 32 cells (2 classes x 16), so more is impossible to begin with.
    assert len(partial["lessons"]) <= 32


def test_relaxed_h1_does_not_unassign_what_fits(problem):
    """On a solvable problem, relaxing H1 drops no lesson (the penalty is that expensive).

    `stop_at_first_feasible` is not used — with H1 relaxed, "drop everything" is
    hard 0 too, and stopping at the first feasible solution would return it.
    """
    result = st.solve(
        problem,
        constraints=Assembly().add("weekly_period_counts", hard=False, weight=10_000),
        time_limit_seconds=10.0,
        random_seed=7,
    )
    assert result["status"] == "FEASIBLE"
    assert len(result["lessons"]) == 40
    assert result["penalties"]["weekly_period_counts"] == 0
    assert result["notes"] == []


def test_h1_stays_hard_without_the_override(problem):
    """An assembly that leaves H1 alone still treats unassigned lessons as a search failure."""
    problem = _over_constrained(problem)
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap", weight=40),
        time_limit_seconds=5.0,
        random_seed=7,
    )
    assert result["status"] == "UNKNOWN"
    assert len(result["lessons"]) == 40
    assert "weekly_period_counts" not in result["penalties"]


def test_unknown_or_invalid_constraint_raises(problem):
    """(4) Bad names/usages fail immediately with ValueError."""
    with pytest.raises(ValueError, match="unknown constraint"):
        Assembly().add("teacher_gaps")
    with pytest.raises(ValueError, match="cannot drop"):
        Assembly().drop("no_such_constraint")
    with pytest.raises(ValueError, match="unknown constraint"):
        canon.DecomposedCalculator().add("no_such_constraint")
    # A spec instance cannot be mixed with keyword overrides.
    with pytest.raises(ValueError, match="spec instance"):
        Assembly().add(TeacherGap(), weight=40)
    # constraints are canonical-mode only.
    with pytest.raises(ValueError, match="canonical"):
        st.solve(problem, mode="quickstart", constraints=Assembly())
    # The rescoring weights (ObjectiveWeights) are integers, so S weights must be integral.
    with pytest.raises(ValueError, match="integer"):
        st.solve(problem, constraints=Assembly().add("teacher_gap", weight=2.5), time_limit_seconds=1.0)


# ---------------------------------------------------------------- cpsat name aliases


def test_cpsat_names_resolve_to_engine_names():
    """The same assembly declaration must go to both backends unchanged (the premise of a performance comparison)."""
    from st_timetabling.constraints import CPSAT_ALIASES, resolve_name

    assert resolve_name("weekly_period_counts") == "unassigned_lesson"
    assert resolve_name("teacher_conflicts") == "teacher_conflict"
    # Names that already match, and engine names, pass through unchanged.
    assert resolve_name("room_capacity") == "room_capacity"
    assert resolve_name("teacher_gap") == "teacher_gap"
    assert resolve_name("unassigned_lesson") == "unassigned_lesson"

    assert set(CPSAT_ALIASES.values()) <= set(list_constraints())


def test_alias_map_covers_every_cpsat_constraint():
    """Every name in the cpsat catalog must be an engine spec or structurally guaranteed.

    Runs only when the cpsat package is installed — catches the two packages drifting apart.
    """
    catalog = pytest.importorskip("teacher_scheduling.api").catalog
    from st_timetabling.constraints import STRUCTURAL_IN_ENGINE, resolve_name

    engine_names = set(list_constraints())
    for spec in catalog():
        if spec.name in STRUCTURAL_IN_ENGINE:
            continue
        assert resolve_name(spec.name) in engine_names, f"no engine counterpart: {spec.name}"


def test_structural_constraints_are_rejected_with_a_reason():
    """Parallel sections / fixed placements cannot be assembled in the engine — no silent ignore."""
    with pytest.raises(ValueError, match="guaranteed by the conversion step"):
        Assembly().add("parallel_groups")
    with pytest.raises(ValueError, match="guaranteed by the conversion step"):
        Assembly().add("fixed_slots")


def test_assembly_accepts_cpsat_names_end_to_end(problem):
    """Assembling with cpsat names gives the same result as with engine names."""
    by_cpsat_name = Assembly().add("teacher_gap", weight=40).drop("teacher_workday")
    by_engine_name = Assembly().add("teacher_gap", weight=40).drop("teacher_workday")
    assert repr(by_cpsat_name) == repr(by_engine_name)

    # A name that needs an alias still replaces the same entry (it is not a second copy).
    assembly = Assembly().add("weekly_period_counts", hard=False, weight=10_000)
    assert len(assembly) == len(Assembly())
    assert "unassigned_lesson" in assembly
    assert "weekly_period_counts" in assembly, "__contains__ accepts aliases too"

    result = st.solve(problem, constraints=assembly, time_limit_seconds=5.0, random_seed=7)
    assert result["status"] in ("FEASIBLE", "UNKNOWN")
