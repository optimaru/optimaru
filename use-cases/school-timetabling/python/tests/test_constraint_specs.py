"""st_timetabling.constraints: the specs, get_constraints and the C++ catalog behind them.

`get_constraints()` — the same entry point as every other use case in the
repository — returns exactly the seventeen default constraints, in
`list_constraints()` order; the specs carry only the five knobs and their
labels; the defaults live in C++ and come back through `constraint_info()`.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import st_timetabling as st
from st_timetabling import canon
from st_timetabling.constraints import (
    Assembly,
    AvoidedPeriodCost,
    ClassConflictCost,
    ClassGapCost,
    FixedSlotsCost,
    NonPreferredPeriodCost,
    ParallelGroupsCost,
    TeacherConflictCost,
    TeacherGapCost,
    TeacherMaxPeriodsCost,
    TeacherWorkdayCost,
    UnassignedLessonsCost,
    get_constraints,
    list_constraints,
    score,
)

NAMES = [
    "unassigned_lessons_cost",
    "class_conflict_cost",
    "teacher_conflict_cost",
    "disallowed_slot_cost",
    "room_capacity_cost",
    "course_max_periods_per_day_cost",
    "block_adjacency_cost",
    "teacher_max_periods_cost",
    "teacher_max_consecutive_periods_cost",
    "teacher_gap_cost",
    "teacher_workday_cost",
    "teacher_free_day_cost",
    "class_gap_cost",
    "avoided_period_cost",
    "non_preferred_period_cost",
    "fixed_slots_cost",
    "parallel_groups_cost",
]

# Every spelling the constraints had before 0.2.0: the engine names, the
# CP-SAT / server / DB names (bridged by the deleted CPSAT_ALIASES) and the
# structural names the engine used to refuse.
OLD_NAMES = [
    "unassigned_lesson",
    "weekly_period_counts",
    "class_conflict",
    "class_group_conflicts",
    "teacher_conflict",
    "teacher_conflicts",
    "slot_not_allowed",
    "room_capacity",
    "course_daily_limit",
    "daily_course_limits",
    "block_adjacency",
    "block_periods",
    "teacher_load_limit",
    "teacher_workload",
    "teacher_consecutive_limit",
    "teacher_consecutive_limits",
    "teacher_gap",
    "teacher_workday",
    "teacher_free_day",
    "class_gap",
    "avoided_period",
    "non_preferred_period",
    "fixed_slots",
    "parallel_groups",
]


def test_get_constraints_defaults_to_the_full_registry():
    """The default list matches the core registry in names and order."""
    assert [spec.name for spec in get_constraints()] == NAMES
    assert list(list_constraints()) == NAMES == list(st.list_constraints()) == list(canon.CONSTRAINT_NAMES)
    assert st._core.list_constraints() == NAMES


def test_get_constraints_matches_the_default_assembly():
    """Must be the same set as `Assembly()` without arguments — only the entry point differs."""
    assert [spec.name for spec in get_constraints()] == [spec.name for spec in Assembly()]


def test_passing_a_list_uses_it_as_given():
    """A given list is used as is — only the listed constraints are scored."""
    picked = get_constraints([TeacherGapCost(weight=40)])
    assert [spec.name for spec in picked] == ["teacher_gap_cost"]
    assert picked[0].weight == 40


def test_empty_list_is_rejected_not_treated_as_the_defaults():
    """An empty list is **not** the default — a filter that happens to come out empty must be an error.

    "Nothing selected" must not silently flip to "score everything"; the C++
    assembly path has the same contract (an empty list is a value_error)."""
    with pytest.raises(ValueError, match="must not be empty"):
        get_constraints([])
    with pytest.raises(ValueError, match="must not be empty"):
        canon.DecomposedCalculator([])


def test_a_constraint_picked_twice_scores_both_picks():
    """Since 0.3.0 a list may name a constraint several times: one instance per pick."""
    picks = [TeacherGapCost(), TeacherGapCost(weight=40)]
    assert get_constraints(picks) == picks
    assert canon.DecomposedCalculator(picks).score(_tiny_table()).soft == -(20 + 40) * 1  # one gap
    assert list(canon.constraint_costs(_tiny_table(), picks)) == ["teacher_gap_cost", "teacher_gap_cost#2"]


def test_returned_list_is_a_fresh_copy():
    """Changing the returned list must not affect the next call."""
    first = get_constraints()
    first.pop()
    assert len(get_constraints()) == 17


def test_module_level_instances_keep_the_default_knobs():
    """Every default instance is at (weight=None, level=None, exponent=1.0): the C++ defaults apply."""
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
        assert getattr(st.constraints, spec.name) is spec


def test_every_spec_has_the_four_labels():
    for spec in get_constraints():
        for field in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(type(spec), field), f"{spec.name}.{field}"
    assert st.TeacherFreeDayCost.label_en == "Worked requested free days"


def test_no_default_classvars_on_the_specs():
    """The defaults live in C++ only (constraint_info()); the specs carry no default_* ClassVars."""
    for spec in get_constraints():
        for field in ("default_weight", "default_hard", "default_level"):
            assert not hasattr(type(spec), field), f"{spec.name}.{field}"


# ----------------------------------------------------------------------- constraint_info()


def test_constraint_info_reports_the_cpp_defaults():
    rows = st.constraint_info()
    assert [row.name for row in rows] == NAMES
    expected = (
        [("hard", 1.0, True, False, True)] * 9
        + [
            ("soft", 20.0, True, False, True),
            ("soft", 3.0, True, False, True),
            ("soft", 30.0, True, False, True),
            ("soft", 10.0, True, False, True),
            ("soft", 5.0, True, False, True),
            ("soft", 1.0, True, False, True),
        ]
        + [("hard", 1.0, False, False, True)] * 2
    )
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == expected


def test_constraint_info_rows_are_plain_values():
    row = st.constraint_info()[0]
    assert isinstance(row, st.ConstraintInfo)
    assert row._asdict() == {
        "name": "unassigned_lessons_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='unassigned_lessons_cost'")
    assert pickle.loads(pickle.dumps(st.constraint_info())) == st.constraint_info()
    assert st.ConstraintInfo.__module__ == st._core.__name__


def test_constraint_costs_metadata_matches_constraint_info(problem):
    converted = st.convert_problem(problem)
    costs = st.constraint_costs(converted.table)
    assert list(costs) == NAMES
    for row in st.constraint_info():
        entry = costs[row.name]
        assert entry["name"] == row.name
        assert entry["level"] == row.default_level
        assert entry["weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["hard"] == (row.default_level == "hard")


def test_weight_none_means_the_component_default(problem):
    """weight=None scores like the explicit C++ defaults, and both equal the monolithic score."""
    converted = st.convert_problem(problem)
    table = converted.table
    st.inject_solution(converted, st.solve(problem, time_limit_seconds=1.0, random_seed=3)["lessons"])
    explicit = [
        type(spec)(weight=row.default_weight) for spec, row in zip(get_constraints(), st.constraint_info(), strict=True)
    ]
    raw = st.constraint_costs(table)
    mono = canon.calculate_score(table)
    assert score(raw) == score(raw, explicit) == (mono.hard, mono.soft)
    assert canon.DecomposedCalculator(explicit).score(table).soft == mono.soft


def test_default_assembly_matches_monolithic_score(problem):
    """A calculator assembled from the default list must score like the monolith."""
    converted = st.convert_problem(problem)
    canon.prepare(converted.table)
    calculator = canon.DecomposedCalculator(get_constraints())
    mono = canon.calculate_score(converted.table)
    plugin = calculator.score(converted.table)
    assert (plugin.hard, plugin.soft) == (mono.hard, mono.soft)


def test_solve_accepts_the_spec_list(problem):
    """`solve(constraints=get_constraints())` behaves like passing an Assembly."""
    result = st.solve(
        problem,
        constraints=get_constraints(),
        time_limit_seconds=10.0,
        random_seed=7,
        stop_at_first_feasible=True,
    )
    assert result["status"] == "FEASIBLE"
    assert result["notes"] == []
    assert len(result["lessons"]) == 40
    assert result["objective_value"] is not None


# ------------------------------------------------------------------------- the knobs


@pytest.mark.parametrize(
    ("knobs", "error", "message"),
    [
        ({"weight": True}, TypeError, "weight must be a number or None, not True"),
        ({"weight": -1.0}, ValueError, "weight must be finite and >= 0, not -1.0"),
        ({"level": 1}, TypeError, "level must be a Level, its name or None, not 1"),
        ({"level": "x"}, ValueError, "level must be 'hard', 'medium' or 'soft', not 'x'"),
        ({"exponent": 0.0}, ValueError, "exponent must be finite and > 0, not 0.0"),
        ({"offset": "5"}, TypeError, "offset must be a number, not '5'"),
        ({"offset": False}, TypeError, "offset must be a number, not False"),
        ({"offset": float("inf")}, ValueError, "offset must be finite, not inf"),
        ({"clip": 1}, TypeError, "clip must be True or False, not 1"),
        ({"clip": "no"}, TypeError, "clip must be True or False, not 'no'"),
    ],
)
def test_spec_and_core_refuse_the_same_knobs(knobs, error, message):
    """The Python base and the C++ binding refuse the same values in the same words."""
    with pytest.raises(error, match=re.escape(message)):
        TeacherGapCost(**knobs)
    pick = SimpleNamespace(name="teacher_gap_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(error, match=f"constraint 'teacher_gap_cost': {re.escape(message)}"):
        canon.DecomposedCalculator([pick])
    with pytest.raises(error, match=re.escape(message)):
        canon.DecomposedCalculator().add("teacher_gap_cost", **knobs)


@pytest.mark.parametrize("spec_class", [FixedSlotsCost, ParallelGroupsCost])
def test_the_structural_constraints_refuse_soft(problem, spec_class):
    """fixed_slots_cost / parallel_groups_cost are hard and not flippable: level="soft" raises on every path
    (the C++ binding's level guard, the solve path and the pure-Python score)."""
    with pytest.raises(ValueError, match="is always hard; level='soft' is not allowed"):
        canon.DecomposedCalculator([spec_class(level="soft")])
    with pytest.raises(ValueError, match="is always hard"):
        canon.DecomposedCalculator().add(spec_class.name, level="soft")
    with pytest.raises(ValueError, match="is always hard"):
        st.solve(problem, time_limit_seconds=0.1, constraints=[spec_class(level="soft")])
    converted = st.convert_problem(problem)
    costs = st.constraint_costs(converted.table)
    with pytest.raises(ValueError, match="is always hard"):
        score(costs, [spec_class(level="soft")])
    # Re-stating the level is a no-op and a weight is accepted (it scales a zero).
    calculator = canon.DecomposedCalculator([spec_class(level="hard", weight=3.0), ClassConflictCost()])
    assert calculator.constraint_costs(converted.table)[spec_class.name]["weight"] == 3.0
    assert costs[spec_class.name]["costs"] == [0.0]


@pytest.mark.parametrize("old", OLD_NAMES)
def test_the_old_names_are_unknown(old):
    """One public name per constraint: neither the old engine nor the old CP-SAT/server spellings resolve."""
    with pytest.raises(ValueError, match="unknown constraint"):
        canon.DecomposedCalculator().add(old)
    with pytest.raises(ValueError, match="unknown constraint"):
        canon.DecomposedCalculator([SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)])
    with pytest.raises(ValueError, match="unknown constraint"):
        Assembly().add(old)
    assert old not in st.list_constraints()


def test_the_alias_layer_is_gone():
    for gone in ("CPSAT_ALIASES", "STRUCTURAL_IN_ENGINE", "ENGINE_ONLY", "resolve_name"):
        assert not hasattr(st.constraints, gone)
    for gone in ("compute_breakdown", "Breakdown"):
        assert not hasattr(canon, gone)


def test_unassigned_lessons_soft_weight_is_the_documented_constant():
    assert st.UNASSIGNED_LESSONS_SOFT_WEIGHT == canon.UNASSIGNED_LESSONS_SOFT_WEIGHT == 10000.0
    relaxed = UnassignedLessonsCost(level="soft", weight=st.UNASSIGNED_LESSONS_SOFT_WEIGHT)
    assert (relaxed.weight, relaxed.level) == (10000.0, "soft")


# ------------------------------------------------------- 0.3.0: repeated picks, offset / clip


def _tiny_table() -> "canon.Timetable":
    """1 day x 4 periods, one teacher and one class, lessons in periods 1 and 3.

    Raw: teacher_gap_cost 1, teacher_workday_cost 1, class_gap_cost 1 (globals);
    the per-slot constraints are 0 in each of the 4 slots.
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
    table.set_assignment([0, 2])
    return table


def _engine(picks):
    result = canon.DecomposedCalculator(picks).score(_tiny_table())
    return result.hard, result.soft


REPEAT = [TeacherGapCost(), TeacherGapCost(weight=0.5, exponent=2)]


def test_the_tiny_table_raw_costs():
    raw = canon.constraint_costs(_tiny_table())
    nonzero = {key: entry["costs"] for key, entry in raw.items() if any(entry["costs"])}
    assert nonzero == {"teacher_gap_cost": [1.0], "teacher_workday_cost": [1.0], "class_gap_cost": [1.0]}
    assert len(raw["avoided_period_cost"]["costs"]) == 4


def test_a_repeated_pick_by_hand():
    """teacher gaps x 20 plus 0.5 x teacher gaps^2 (a global: the one total, 1)."""
    raw = canon.constraint_costs(_tiny_table(), REPEAT)
    assert list(raw) == ["teacher_gap_cost", "teacher_gap_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [1.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (20.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "teacher_gap_cost"
    assert second["default_weight"] == 20.0
    expected = (0, -(20 * 1 + 0.5 * 1**2))
    assert score(raw, REPEAT) == expected
    assert score(canon.constraint_costs(_tiny_table()), REPEAT) == expected  # the default costs hold it once
    assert _engine(REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [ClassGapCost(), ClassGapCost()]
    assert score(canon.constraint_costs(_tiny_table()), picks) == _engine(picks) == (0, -20.0)


def test_the_keys_follow_the_pick_order_of_each_name():
    """Each name's instances sit together, in the catalog order (the calculator lists units first)."""
    picks = [
        TeacherGapCost(weight=1),
        AvoidedPeriodCost(),
        TeacherGapCost(weight=2),
        ClassConflictCost(),
        TeacherGapCost(weight=3),
    ]
    raw = canon.constraint_costs(_tiny_table(), picks)
    assert list(raw) == [
        "class_conflict_cost",
        "teacher_gap_cost",
        "teacher_gap_cost#2",
        "teacher_gap_cost#3",
        "avoided_period_cost",
    ]
    assert [raw[key]["weight"] for key in ("teacher_gap_cost", "teacher_gap_cost#2", "teacher_gap_cost#3")] == [
        1.0,
        2.0,
        3.0,
    ]
    assert score(raw, picks) == _engine(picks) == (0, -6.0)
    calculator = canon.DecomposedCalculator(picks)
    calculator.score(_tiny_table())
    assert [total["name"] for total in calculator.constraint_totals()] == [
        "class_conflict_cost",
        "teacher_gap_cost",
        "teacher_gap_cost",
        "teacher_gap_cost",
        "avoided_period_cost",
    ]


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Globals, the one total shaped once (teacher gaps 1, weight 20; workday 1, weight 3).
        (TeacherGapCost(offset=0.5), (0, -20 * 0.5)),
        (TeacherGapCost(offset=2), (0, 0.0)),
        (TeacherGapCost(offset=2, clip=False), (0, -20 * (1 - 2))),  # a reward below 2
        (TeacherWorkdayCost(offset=-1), (0, -3 * 2)),
        (TeacherWorkdayCost(offset=-1, clip=False, exponent=2), (0, -3 * 2**2)),
        # A per-slot constraint: 0 in each of the 4 slots, so the literal formula charges all of them.
        (AvoidedPeriodCost(offset=1), (0, 0.0)),
        (AvoidedPeriodCost(offset=1, clip=False), (0, -5 * 4 * -1)),
        (NonPreferredPeriodCost(offset=-2, clip=False), (0, -1 * 4 * 2)),
        # Hard, rounded per unit: 3 x (0 - 0.5) = -1.5 -> -2 in each of the 4 slots.
        (TeacherConflictCost(offset=0.5, clip=False, weight=3), (8, 0.0)),
        (TeacherMaxPeriodsCost(offset=1, clip=False), (1, 0.0)),  # a global at 0: one hard reward
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(canon.constraint_costs(_tiny_table()), [pick]) == expected
    raw = canon.constraint_costs(_tiny_table(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected


def test_add_takes_the_offset_and_clip_knobs():
    by_name = canon.DecomposedCalculator()
    by_name.add("teacher_gap_cost")
    by_name.add("teacher_gap_cost", weight=0.5, exponent=2.0, offset=0.5, clip=False)
    picks = [TeacherGapCost(), TeacherGapCost(weight=0.5, exponent=2.0, offset=0.5, clip=False)]
    assert by_name.score(_tiny_table()).soft == _engine(picks)[1]
    with pytest.raises(TypeError):
        canon.DecomposedCalculator().add("teacher_gap_cost", 1.0, None, 1.0, 0.5)  # keyword-only


def test_assembly_keeps_replace_by_name_and_takes_the_new_knobs():
    """An Assembly is a builder: add() replaces the name; repeats go into a plain list."""
    assembly = Assembly().add("teacher_gap_cost", weight=40, offset=1).add(TeacherGapCost(weight=2))
    assert [spec for spec in assembly if spec.name == "teacher_gap_cost"] == [TeacherGapCost(weight=2)]
    shifted = Assembly().add("teacher_gap_cost", exponent=2, offset=3, clip=False)
    assert [spec for spec in shifted if spec.name == "teacher_gap_cost"] == [
        TeacherGapCost(exponent=2, offset=3, clip=False)
    ]
    with pytest.raises(ValueError, match="spec instance"):
        Assembly().add(TeacherGapCost(), offset=1)
    with pytest.raises(ValueError, match="spec instance"):
        Assembly().add(TeacherGapCost(), clip=False)
    repeated = [*Assembly(), TeacherGapCost(exponent=2, offset=3)]
    assert len(get_constraints(repeated)) == 18


def test_offset_zero_reproduces_the_0_2_0_scores(problem):
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    converted = st.convert_problem(problem)
    st.inject_solution(converted, st.solve(problem, time_limit_seconds=1.0, random_seed=3)["lessons"])
    table = converted.table
    plain = [*get_constraints()[:9], TeacherGapCost(exponent=2), *get_constraints()[10:]]
    shifted = [type(spec)(exponent=spec.exponent, offset=0.0, clip=False) for spec in plain]
    raw = canon.constraint_costs(table)
    assert score(raw, shifted) == score(raw, plain)
    first, second = (canon.DecomposedCalculator(picks).score(table) for picks in (plain, shifted))
    assert (first.hard, first.soft) == (second.hard, second.soft)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


PICKS = {
    "repeat": [*get_constraints(), *REPEAT, ClassGapCost(weight=3)],
    "repeat-and-offsets": [
        *get_constraints(),
        TeacherGapCost(weight=2, offset=1, clip=False),
        TeacherGapCost(weight=0.5, exponent=2, offset=2),
        TeacherWorkdayCost(offset=-1, clip=False),
        TeacherMaxPeriodsCost(offset=1, clip=False, weight=2),
        ClassGapCost(offset=1),
        AvoidedPeriodCost(offset=1, clip=False, weight=2),
        NonPreferredPeriodCost(offset=-1),
        TeacherConflictCost(offset=0.5, clip=False),
    ],
}


@pytest.mark.parametrize("picks", list(PICKS.values()), ids=list(PICKS))
def test_full_assert_solves_with_repeats_and_offsets(problem, picks):
    """FULL_ASSERT re-checks the incremental score after every move; the repeated
    teacher-mask instances share the calculator's one TeacherMaskHub."""
    converted = st.convert_problem(problem)
    config = canon.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.step_count_limit = 1500
    config.spent_limit_seconds = 30
    calculator = canon.DecomposedCalculator(picks)
    canon.Solver(config).solve(converted.table, calculator)
    reported = converted.table.score
    raw = canon.constraint_costs(converted.table, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == reported.hard
    assert python.soft == pytest.approx(reported.soft)
    assert score(canon.constraint_costs(converted.table), picks) == python
