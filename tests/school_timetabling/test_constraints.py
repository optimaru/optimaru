"""Plugin constraint assembly: the assembly API + the `solve(constraints=...)` path.

The monolithic fast path (default, constraints=None) is unchanged; for the
plugin path this pins down (1) the default assembly scores like the monolith
and solve results match the canonical rescore, (2) weight overrides reach both
the search score and the result's objective_value, (3) dropping a component
works, (4) bad names raise ValueError, (5) the CP-SAT reference uses the very
same names (one public name per constraint, no alias table).
"""

import random

import pytest

import optimaru.school_timetabling as st
from optimaru.school_timetabling import canon
from optimaru.school_timetabling.constraints import (
    UNASSIGNED_LESSONS_SOFT_WEIGHT,
    Assembly,
    TeacherGapCost,
    UnassignedLessonsCost,
    list_constraints,
)

# The soft components' default weights (the same baseline as test_solve.py).
DEFAULT_WEIGHTS = {
    "teacher_gap_cost": 20,
    "teacher_workday_cost": 3,
    "teacher_free_day_cost": 30,
    "class_gap_cost": 10,
    "avoided_period_cost": 5,
    "non_preferred_period_cost": 1,
}


def _tiny_table() -> "canon.Timetable":
    """Smallest hand-scorable instance: 1 day x 4 periods, one teacher/class, 2 lessons.

    Placed in periods 1 and 3: teacher_gap_cost = 1, teacher_workday_cost = 1,
    class_gap_cost = 1, everything else 0.
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
    assert names == list(canon.CONSTRAINT_NAMES) == st._core.list_constraints()  # a list, like the other packages
    assert len(names) == 17  # fixed_slots_cost / parallel_groups_cost included
    # The default Assembly holds the whole registry in the same order.
    assert [spec.name for spec in Assembly()] == names


def test_default_assembly_matches_monolithic_score(problem):
    """(1) Default assembly == monolith: bit-identical scores over random assignments."""
    converted = st.convert_problem(problem)
    canon.prepare(converted.table)
    calculator = canon.default_calculator()  # all 17, the component default weights
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
    rescore = canon.calculate_score(converted.table)
    costs = canon.constraint_costs(converted.table)
    assert rescore.hard == 0
    assert {name: int(sum(costs[name]["costs"])) for name in DEFAULT_WEIGHTS} == result["penalties"]
    assert result["objective_value"] == _weighted(result["penalties"], DEFAULT_WEIGHTS) == -rescore.soft


def test_weight_override_applies_to_score_and_result(problem):
    """(2) Weight override: reaches both the calculator score and the solve result."""
    # Hand-scored instance: teacher_gap_cost is 1, so the weight is the cost.
    table = _tiny_table()
    default_soft = canon.default_calculator().score(table).soft

    doubled = canon.DecomposedCalculator(Assembly().add("teacher_gap_cost", weight=40))
    score = doubled.score(table)
    assert score.hard == 0
    assert score.soft == default_soft - 20  # gap 1 x (40 - 20)
    totals = {t["name"]: t for t in doubled.constraint_totals()}
    assert totals["teacher_gap_cost"]["cost"] == 40.0
    assert totals["teacher_gap_cost"]["level"] == "soft"
    assert totals["teacher_gap_cost"]["label"] == ""

    # A list of spec instances gives the same assembly.
    spec_list = canon.DecomposedCalculator([TeacherGapCost(weight=40)])
    assert spec_list.score(table).soft == -40.0

    # solve result: objective_value is rescored with the overridden effective weight.
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap_cost", weight=40),
        time_limit_seconds=20.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    effective = dict(DEFAULT_WEIGHTS, teacher_gap_cost=40)
    assert result["objective_value"] == _weighted(result["penalties"], effective)


def test_constraint_totals_follow_the_catalog_order():
    """constraint_totals() lists the constraints in list_constraints() order, like constraint_costs()."""
    table = _tiny_table()
    calculator = canon.default_calculator()
    calculator.score(table)
    assert [t["name"] for t in calculator.constraint_totals()] == list_constraints()
    assert list(calculator.constraint_costs(table)) == list_constraints()


def test_drop_component(problem):
    """(3) Dropping a component: the term leaves the calculator and its rescoring weight becomes 0."""
    assembly = Assembly().drop("teacher_workday_cost")
    assert len(assembly) == 16  # 17 defaults (the two structural constraints included) - 1
    assert "teacher_workday_cost" not in assembly
    assert "teacher_gap_cost" in assembly

    table = _tiny_table()
    calculator = canon.DecomposedCalculator(assembly)
    score = calculator.score(table)
    names = {t["name"] for t in calculator.constraint_totals()}
    assert "teacher_workday_cost" not in names
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
    effective = dict(DEFAULT_WEIGHTS, teacher_workday_cost=0)
    assert result["objective_value"] == _weighted(result["penalties"], effective)


# ------------------------------------------------- partial timetabling (unassigned_lessons_cost relaxed)
#
# Demoting unassigned_lessons_cost to soft makes "leave it out" a search option
# (unassign moves). The same contract as the CP-SAT reference's
# UnassignedLessonsCost(level="soft") = solve_partial.


def _over_constrained(problem: dict) -> dict:
    """The demo school without Friday — 20 weekly periods per class but only 16 cells."""
    problem["calendar"]["periods_per_day"]["4"] = 0
    return problem


def test_relaxed_unassigned_lessons_return_a_partial_timetable(problem):
    """Unfillable periods stay unassigned instead of being hard violations."""
    problem = _over_constrained(problem)
    relaxed = Assembly().add("unassigned_lessons_cost", level="soft", weight=UNASSIGNED_LESSONS_SOFT_WEIGHT)

    strict = st.solve(problem, time_limit_seconds=5.0, random_seed=7)
    partial = st.solve(problem, constraints=relaxed, time_limit_seconds=5.0, random_seed=7)

    # Kept hard, the periods that cannot fit remain hard violations: INFEASIBLE, without proof.
    assert strict["status"] == "INFEASIBLE"
    assert len(strict["lessons"]) == 40, "with unassigned_lessons_cost hard the search gives every lesson a slot"

    # Relaxed, the rest is a partial solution: status is decided without the unassigned lessons.
    assert partial["status"] == "FEASIBLE"
    assert 0 < len(partial["lessons"]) < 40
    # Unassigned periods are reported under the constraint's name, in cpsat's relaxed-mode unit.
    assert partial["penalties"]["unassigned_lessons_cost"] == 40 - len(partial["lessons"])
    assert partial["objective_value"] == _weighted(
        partial["penalties"], dict(DEFAULT_WEIGHTS, unassigned_lessons_cost=10_000)
    )
    # There are only 32 cells (2 classes x 16), so more is impossible to begin with.
    assert len(partial["lessons"]) <= 32


def test_relaxed_unassigned_lessons_do_not_unassign_what_fits(problem):
    """On a solvable problem, relaxing unassigned_lessons_cost drops no lesson (the penalty is that expensive).

    `stop_at_first_feasible` is not used — with the constraint relaxed, "drop
    everything" is hard 0 too, and stopping at the first feasible solution would
    return it.
    """
    result = st.solve(
        problem,
        constraints=[*st.get_constraints()[1:], UnassignedLessonsCost(level="soft", weight=10_000)],
        time_limit_seconds=10.0,
        random_seed=7,
    )
    assert result["status"] == "FEASIBLE"
    assert len(result["lessons"]) == 40
    assert result["penalties"]["unassigned_lessons_cost"] == 0
    assert result["notes"] == []


def test_unassigned_lessons_stay_hard_without_the_override(problem):
    """An assembly that leaves unassigned_lessons_cost alone still treats unassigned lessons as a search failure."""
    problem = _over_constrained(problem)
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap_cost", weight=40),
        time_limit_seconds=5.0,
        random_seed=7,
    )
    assert result["status"] == "INFEASIBLE"
    assert len(result["lessons"]) == 40
    assert "unassigned_lessons_cost" not in result["penalties"]


def test_unknown_or_invalid_constraint_raises(problem):
    """(4) Bad names/usages fail immediately with ValueError."""
    with pytest.raises(ValueError, match="unknown constraint"):
        Assembly().add("teacher_gap_costs")
    with pytest.raises(ValueError, match="cannot drop"):
        Assembly().drop("no_such_constraint")
    with pytest.raises(ValueError, match="unknown constraint"):
        canon.DecomposedCalculator().add("no_such_constraint")
    # A spec instance cannot be mixed with keyword overrides.
    with pytest.raises(ValueError, match="spec instance"):
        Assembly().add(TeacherGapCost(), weight=40)
    # constraints are canonical-mode only.
    with pytest.raises(ValueError, match="canonical"):
        st.solve(problem, mode="quickstart", constraints=Assembly())
    # An empty plain list is an error, not the defaults.
    with pytest.raises(ValueError, match="must not be empty"):
        st.solve(problem, constraints=[], time_limit_seconds=0.1)


def test_a_plain_list_may_pick_a_constraint_twice(problem):
    """A plain list goes through get_constraints: a name picked twice is two instances, not last-wins.

    objective_value adds the instances' weights (20 + 4 for the teacher gaps), the
    exact linear contribution of the two picks."""
    picks = [*Assembly(), "teacher_gap_cost", TeacherGapCost(weight=4)]
    result = st.solve(problem, constraints=picks, time_limit_seconds=5.0, random_seed=7, stop_at_first_feasible=True)
    assert result["status"] == "FEASIBLE"
    effective = dict(DEFAULT_WEIGHTS, teacher_gap_cost=20 + 20 + 4)
    assert result["objective_value"] == pytest.approx(_weighted(result["penalties"], effective))


def test_one_hard_instance_keeps_unassigned_lessons_hard(problem):
    """unassigned_lessons_cost picked twice, once hard: no partial timetable (no unassign moves)."""
    picks = [
        *[spec for spec in Assembly() if spec.name != "unassigned_lessons_cost"],
        UnassignedLessonsCost(level="soft", weight=UNASSIGNED_LESSONS_SOFT_WEIGHT),
        UnassignedLessonsCost(),
    ]
    result = st.solve(_over_constrained(problem), constraints=picks, time_limit_seconds=2.0, random_seed=7)
    assert result["status"] == "INFEASIBLE"
    assert len(result["lessons"]) == 40, "one hard instance: the search gives every lesson a slot"
    assert "unassigned_lessons_cost" not in result["penalties"]
    # Both soft: a partial timetable, each unplaced period costing the sum of the weights.
    soft_twice = [
        *[spec for spec in Assembly() if spec.name != "unassigned_lessons_cost"],
        UnassignedLessonsCost(level="soft", weight=UNASSIGNED_LESSONS_SOFT_WEIGHT),
        UnassignedLessonsCost(level="soft", weight=1),
    ]
    partial = st.solve(_over_constrained(problem), constraints=soft_twice, time_limit_seconds=5.0, random_seed=7)
    assert partial["status"] == "FEASIBLE"
    unassigned = partial["penalties"]["unassigned_lessons_cost"]
    assert unassigned > 0
    rest = {name: count for name, count in partial["penalties"].items() if name != "unassigned_lessons_cost"}
    assert partial["objective_value"] == pytest.approx(
        _weighted(rest, DEFAULT_WEIGHTS) + (UNASSIGNED_LESSONS_SOFT_WEIGHT + 1) * unassigned
    )


def test_soft_weights_need_not_be_integers(problem):
    """The rescoring weights are plain numbers: a fractional soft weight reaches objective_value as given."""
    result = st.solve(
        problem,
        constraints=Assembly().add("teacher_gap_cost", weight=2.5),
        time_limit_seconds=5.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    effective = dict(DEFAULT_WEIGHTS, teacher_gap_cost=2.5)
    assert result["objective_value"] == pytest.approx(_weighted(result["penalties"], effective))


# ------------------------------------------------------------- one name in every layer


def test_structural_constraints_assemble_like_any_other(problem):
    """fixed_slots_cost / parallel_groups_cost are real components: they assemble, cost 0, stay hard."""
    assembly = Assembly([st.constraints.fixed_slots_cost, st.constraints.parallel_groups_cost])
    assert [spec.name for spec in assembly] == ["fixed_slots_cost", "parallel_groups_cost"]
    converted = st.convert_problem(problem)
    calculator = canon.DecomposedCalculator(assembly)
    assert calculator.score(converted.table).hard == 0
    assert {t["name"]: t["cost"] for t in calculator.constraint_totals()} == {
        "fixed_slots_cost": 0.0,
        "parallel_groups_cost": 0.0,
    }
    result = st.solve(problem, constraints=Assembly(), time_limit_seconds=5.0, random_seed=7)
    assert result["status"] in ("FEASIBLE", "INFEASIBLE")
