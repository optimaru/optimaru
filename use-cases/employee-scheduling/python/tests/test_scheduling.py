"""Correctness tests: the hand-computed reference score, plugin assembly
equivalence, incremental (delta) scoring exactness, cost shaping and hard/soft
overrides.

Requires the compiled extension: uv sync --all-groups --all-extras
"""

import threading
import time

import pytest
from _reference import REFERENCE_SCORE, reference_schedule

import es_scheduling as es


@pytest.mark.parametrize("use_calculator", [False, True], ids=["built-in", "plugin"])
def test_solve_releases_the_gil_and_on_new_best_still_fires(use_calculator):
    schedule = es.make_random_roster(days=8, shifts_per_day=6, employees=9, seed=23)
    steps = []
    cfg = es.SolverConfig()
    cfg.spent_limit_seconds = 0.3
    cfg.on_new_best = lambda step, score: steps.append(step)

    ticks = 0
    stop = threading.Event()

    def tick():
        nonlocal ticks
        while not stop.is_set():
            ticks += 1
            time.sleep(0.001)

    ticker = threading.Thread(target=tick)
    ticker.start()
    try:
        if use_calculator:
            es.Solver(cfg).solve(schedule, es.default_calculator())
        else:
            es.Solver(cfg).solve(schedule)
    finally:
        stop.set()
        ticker.join()

    assert steps
    assert ticks > 10


def test_solver_config_exposes_engine_knobs():
    cfg = es.SolverConfig()
    cfg.step_count_limit = 123
    cfg.unimproved_step_count_limit = 45
    cfg.unimproved_spent_limit_seconds = 0.25
    cfg.no_improvement_step_count_limit = 44
    cfg.no_improvement_limit_seconds = 0.15
    cfg.ruin_recreate = False
    cfg.employee_greedy_construction = False
    cfg.ruin_recreate_unimproved_step_trigger = 50
    cfg.ruin_recreate_min_ruin_fraction = 0.2
    cfg.ruin_recreate_max_ruin_fraction = 0.4
    cfg.ruin_recreate_max_ruin_count = 12
    cfg.ruin_recreate_operator_decay = 0.6
    cfg.ruin_recreate_rewards = [10.0, 5.0, 1.0, 0.0]
    cfg.ruin_recreate_polish_steps = 20
    cfg.ruin_recreate_as_move_weight = 0.05
    cfg.ruin_recreate_as_move_max_ruin = 8
    cfg.descent_weight = 0.02
    cfg.descent_max_sweeps = 3
    cfg.change_move_weight = 1.0
    cfg.swap_move_weight = 0.0
    cfg.smart_change_moves = True
    cfg.smart_change_candidate_count = 4
    cfg.employee_day_repair_moves = True
    cfg.employee_run_repair_moves = False
    cfg.employee_run_repair_max_days = 5
    cfg.employee_window_moves = True
    cfg.employee_window_max_days = 3
    cfg.custom_move_probability = 0.15
    cfg.employee_ruin_moves = False
    cfg.or_opt_move_weight = 0.15

    assert cfg.step_count_limit == 123
    assert cfg.unimproved_step_count_limit == 44
    assert cfg.no_improvement_step_count_limit == 44
    assert cfg.unimproved_spent_limit_seconds == pytest.approx(0.15)
    assert cfg.no_improvement_limit_seconds == pytest.approx(0.15)
    assert cfg.ruin_recreate is False
    assert cfg.employee_greedy_construction is False
    assert cfg.ruin_recreate_rewards == [10.0, 5.0, 1.0, 0.0]
    assert cfg.change_move_weight == 1.0
    assert cfg.swap_move_weight == 0.0
    assert cfg.smart_change_moves is True
    assert cfg.smart_change_candidate_count == 4
    assert cfg.employee_day_repair_moves is True
    assert cfg.employee_run_repair_moves is False
    assert cfg.employee_run_repair_max_days == 5
    assert cfg.employee_window_moves is True
    assert cfg.employee_window_max_days == 3
    assert cfg.custom_move_probability == 0.15
    assert cfg.employee_ruin_moves is False
    assert cfg.or_opt_move_weight == 0.15
    assert cfg.ruin_recreate_as_move_weight == 0.05
    assert cfg.ruin_recreate_as_move_max_ruin == 8
    assert cfg.descent_weight == 0.02
    assert cfg.descent_max_sweeps == 3
    with pytest.raises(ValueError, match="positive"):
        cfg.late_acceptance_size = 0
    with pytest.raises(ValueError, match="positive"):
        cfg.smart_change_candidate_count = 0
    with pytest.raises(ValueError, match="positive"):
        cfg.employee_run_repair_max_days = 0
    with pytest.raises(ValueError, match="positive"):
        cfg.employee_window_max_days = 0
    with pytest.raises(ValueError, match="between 0 and 1"):
        cfg.custom_move_probability = 1.1
    with pytest.raises(ValueError, match="non-negative"):
        cfg.no_improvement_limit_seconds = -0.1
    with pytest.raises(ValueError, match="non-negative"):
        cfg.no_improvement_step_count_limit = -1
    with pytest.raises(ValueError, match="exactly 4"):
        cfg.ruin_recreate_rewards = [1.0, 2.0, 3.0]


def test_reference_score_is_the_hand_computed_one():
    schedule = reference_schedule()
    assert str(es.calculate_score(schedule)) == REFERENCE_SCORE


def test_default_assembly_reproduces_builtin_score():
    schedule = reference_schedule()
    assert str(es.default_calculator().score(schedule)) == REFERENCE_SCORE


def test_default_assembly_grouped_search_matches_builtin_trajectory():
    built_in = es.make_random_roster(days=8, shifts_per_day=6, employees=9, seed=23)
    plugin = es.make_random_roster(days=8, shifts_per_day=6, employees=9, seed=23)
    trajectories = []

    def solve(schedule, calculator=None):
        trajectory = []
        cfg = es.SolverConfig()
        cfg.spent_limit_seconds = 10.0
        cfg.step_count_limit = 5000
        cfg.random_seed = 23
        cfg.guided_moves = False
        cfg.ruin_recreate = False
        cfg.on_new_best = lambda step, score: trajectory.append((step, str(score)))
        solver = es.Solver(cfg)
        if calculator is None:
            solver.solve(schedule)
        else:
            solver.solve(schedule, calculator)
        trajectories.append(trajectory)

    calculator = es.default_calculator()
    solve(built_in)
    solve(plugin, calculator)

    assert calculator.delta_group_count == 1
    assert trajectories[0] == trajectories[1]
    assert [s.employee_index for s in built_in.shifts] == [s.employee_index for s in plugin.shifts]
    assert str(built_in.score) == str(plugin.score)


def test_modified_default_assembly_keeps_grouping_and_custom_score():
    schedule = es.make_random_roster(days=6, shifts_per_day=5, employees=7, seed=29)
    calculator = es.DecomposedCalculator()
    for name in (
        "required_skill",
        "unavailable_employee",
        "overlapping_shift",
        "one_shift_per_day",
        "min_rest",
        "undesired_day",
        "desired_day",
    ):
        calculator.add(name)
    calculator.add("balance_assignments", weight=3.0)

    trajectory = []
    cfg = es.SolverConfig()
    cfg.spent_limit_seconds = 10.0
    cfg.step_count_limit = 5000
    cfg.random_seed = 29
    cfg.guided_moves = False
    cfg.ruin_recreate = False
    cfg.on_new_best = lambda step, score: trajectory.append((step, str(score)))
    es.Solver(cfg).solve(schedule, calculator)

    assert calculator.delta_group_count == 1
    assert trajectory
    assert trajectory[-1][1] == str(schedule.score)
    assert str(schedule.score) == str(calculator.score(schedule))


def test_structural_grouping_handles_pairwise_subsets_only_when_beneficial():
    schedule = es.make_random_roster(days=7, shifts_per_day=6, employees=8, seed=31)
    grouped = es.DecomposedCalculator([es.OverlappingShift(), es.MinimumRest(min_gap_minutes=12 * 60)])
    direct_only = es.DecomposedCalculator([es.RequiredSkill(), es.DesiredDay()])

    grouped.score(schedule)
    direct_only.score(schedule)

    assert grouped.delta_group_count == 1
    assert direct_only.delta_group_count == 0


def test_constraint_totals_explain_the_score():
    schedule = reference_schedule()
    calc = es.default_calculator()
    score = calc.score(schedule)
    totals = calc.constraint_totals()
    assert len(totals) == 8  # the eight default constraints
    hard = sum(t["cost"] for t in totals if t["is_hard"])
    soft = sum(t["cost"] for t in totals if not t["is_hard"])
    assert -hard == pytest.approx(score.hard)
    assert -soft == pytest.approx(score.soft)


def test_unknown_constraint_name_raises():
    with pytest.raises(ValueError):
        es.DecomposedCalculator().add("no_such_constraint")


def test_constraint_list_equals_add_by_name():
    """The declarative form (a list of Constraint specs) must build the same
    assembly as the equivalent add(name, ...) calls."""
    schedule = reference_schedule()

    declared = es.DecomposedCalculator(
        [
            es.RequiredSkill(2.0),
            es.MinimumRest(0.25, hard=False),
            es.UndesiredDay(hard=True),
            es.DesiredDay(exponent=2.0),
            es.BalanceShiftAssignments(3.0),
        ]
    )
    named = es.DecomposedCalculator()
    named.add("required_skill", weight=2.0)
    named.add("min_rest", weight=0.25, hard=False)
    named.add("undesired_day", hard=True)
    named.add("desired_day", exponent=2.0)
    named.add("balance_assignments", weight=3.0)

    assert str(declared.score(schedule)) == str(named.score(schedule))
    assert declared.constraint_totals() == named.constraint_totals()


def test_default_constraint_list_reproduces_builtin_score():
    schedule = reference_schedule()
    calc = es.DecomposedCalculator(
        [
            es.RequiredSkill(),
            es.UnavailableEmployee(),
            es.OverlappingShift(),
            es.MaxOneShiftPerDay(),
            es.MinimumRest(),
            es.UndesiredDay(),
            es.DesiredDay(),
            es.BalanceShiftAssignments(),
        ]
    )
    assert str(calc.score(schedule)) == REFERENCE_SCORE


@pytest.mark.parametrize(
    "spec",
    [
        [{"name": n} for n in es.CONSTRAINT_NAMES],
        [
            {"name": "required_skill", "weight": 2.0},
            {"name": "unavailable_employee"},
            {"name": "overlapping_shift", "weight": 0.5},
            {"name": "one_shift_per_day"},
            {"name": "min_rest", "hard": False, "weight": 0.25},
            {"name": "undesired_day", "hard": True},
            {"name": "desired_day", "weight": 0.5},
            {"name": "balance_assignments", "weight": 3.0},
        ],
    ],
    ids=["default", "weighted-with-overrides"],
)
def test_assembly_solve_matches_fresh_recompute(spec):
    """After a solve on the incremental (delta) plugin path, the reported
    score must equal a from-scratch recompute by an identical fresh assembly."""

    def build():
        calc = es.DecomposedCalculator()
        for kwargs in spec:
            calc.add(**kwargs)
        return calc

    schedule = es.make_random_roster(days=6, shifts_per_day=5, employees=8, seed=11)
    cfg = es.SolverConfig()
    cfg.spent_limit_seconds = 0.3
    cfg.random_seed = 11
    es.Solver(cfg).solve(schedule, build())
    assert str(schedule.score) == str(build().score(schedule))


def test_shaped_assembly_solves_on_the_incremental_path():
    """exponent != 1 keeps the fast path: shaping is applied on top of the
    incrementally maintained raw cost, and the result stays exact."""
    calc = es.DecomposedCalculator()
    calc.add("required_skill")
    calc.add("undesired_day", exponent=2.0)
    calc.add("balance_assignments", exponent=3.0)

    schedule = es.make_random_roster(days=6, shifts_per_day=5, employees=8, seed=7)
    cfg = es.SolverConfig()
    cfg.spent_limit_seconds = 0.3
    cfg.random_seed = 7
    es.Solver(cfg).solve(schedule, calc)

    fresh = es.DecomposedCalculator()
    fresh.add("required_skill")
    fresh.add("undesired_day", exponent=2.0)
    fresh.add("balance_assignments", exponent=3.0)
    assert str(schedule.score) == str(fresh.score(schedule))
    assert schedule.score.hard == 0


def test_employee_specific_moves_keep_incremental_score_exact():
    """The stronger employee-domain moves still go through ScoreDirector, so a
    solved schedule must match a fresh calculator rebuild."""

    schedule = es.make_random_roster(days=10, shifts_per_day=5, employees=9, seed=17)
    cfg = es.SolverConfig()
    cfg.random_seed = 17
    cfg.step_count_limit = 1500
    cfg.spent_limit_seconds = 5.0
    cfg.smart_change_moves = True
    cfg.smart_change_candidate_count = 4
    cfg.employee_day_repair_moves = True
    cfg.employee_run_repair_moves = True
    cfg.employee_run_repair_max_days = 3
    cfg.employee_window_moves = True
    cfg.employee_window_max_days = 3
    cfg.employee_ruin_moves = True
    es.Solver(cfg).solve(schedule, es.default_calculator())

    assert str(schedule.score) == str(es.default_calculator().score(schedule))


@pytest.mark.parametrize(
    "descent,as_move",
    [(0.05, 0.0), (0.0, 0.15), (0.05, 0.15), (0.3, 0.4)],
    ids=["descent", "destroy-repair-as-move", "both", "both-heavy"],
)
def test_search_strategies_keep_incremental_score_exact(descent, as_move):
    """Best-improvement descent and destroy-repair-as-a-move both mutate through
    multi-step sequences — a descent sweep restores every trial value before
    committing the best one, and a rejected destroy-repair is rolled back from a
    whole-solution snapshot. Rollback is where a bug would hide, so the solved
    score has to survive a fresh calculator rebuild."""

    schedule = es.make_random_roster(days=10, shifts_per_day=5, employees=9, seed=23)
    cfg = es.SolverConfig()
    cfg.random_seed = 23
    cfg.spent_limit_seconds = 2.0
    cfg.descent_weight = descent
    cfg.ruin_recreate_as_move_weight = as_move
    cfg.ruin_recreate_unimproved_step_trigger = 200
    cfg.ruin_recreate_polish_steps = 50
    es.Solver(cfg).solve(schedule, es.default_calculator())

    assert str(schedule.score) == str(es.default_calculator().score(schedule))


def test_squared_shaping_matches_hand_computation():
    schedule = es.make_random_roster(days=6, shifts_per_day=5, employees=8, seed=3)
    for emp in schedule.employees:  # the random roster has no undesired dates
        emp.undesired_dates = {1, 4}
    for i, shift in enumerate(schedule.shifts):
        shift.employee_index = i % len(schedule.employees)

    calc = es.DecomposedCalculator()
    calc.add("undesired_day", exponent=2.0)
    calc.score(schedule)
    (total,) = calc.constraint_totals()

    per_employee = {}
    for shift in schedule.shifts:
        emp = schedule.employees[shift.employee_index]
        if (shift.start // es.MINUTES_PER_DAY) in emp.undesired_dates:
            per_employee[shift.employee_index] = per_employee.get(shift.employee_index, 0) + shift.end - shift.start
    assert total["cost"] == pytest.approx(sum(v * v for v in per_employee.values()))
    assert total["cost"] > 0


def test_desired_shift_matches_day_and_location_and_shapes_per_employee():
    """The location preference is counted by exact (day, location).

    Two Location A matches for the first worker give raw cost -2.  The same day
    at Location B, the same location on another day, and a match assigned to the
    second worker contribute zero.  Weight 1.5 and exponent 2 therefore produce
    1.5 * -(2**2) = -6.
    """
    first = es.Employee("Worker 01")
    first.desired_shifts = {(2, "Location A")}
    second = es.Employee("Worker 02")
    shifts = [
        es.Shift("match-1", es.minutes(2, 8), es.minutes(2, 10), "Location A"),
        es.Shift("match-2", es.minutes(2, 11), es.minutes(2, 13), "Location A"),
        es.Shift("wrong-location", es.minutes(2, 14), es.minutes(2, 16), "Location B"),
        es.Shift("wrong-day", es.minutes(3, 8), es.minutes(3, 10), "Location A"),
        es.Shift("other-worker", es.minutes(2, 16), es.minutes(2, 18), "Location A"),
    ]
    for shift, employee in zip(shifts, [0, 0, 0, 0, 1], strict=True):
        shift.employee_index = employee
    schedule = es.EmployeeSchedule([first, second], shifts)

    calc = es.DecomposedCalculator([es.DesiredShift(weight=1.5, exponent=2.0)])
    result = calc.score(schedule)
    (total,) = calc.constraint_totals()

    assert total == {
        "name": "Desired shift for employee",
        "cost": pytest.approx(-6.0),
        "is_hard": False,
    }
    assert result.hard == 0
    assert result.soft == pytest.approx(6.0)


def _week_roster():
    """14 days x 1 shift/day (8h, day 0..13), 2 employees, all assigned to A.

    A fully deterministic playground for the extended constraints: employee A
    works 14 days in a row, B never works.
    """
    employees = [es.Employee("A"), es.Employee("B")]
    shifts = []
    for day in range(14):
        s = es.Shift(f"Shift {day}", es.minutes(day, 9), es.minutes(day, 17))
        s.employee_index = 0
        shifts.append(s)
    return es.EmployeeSchedule(employees, shifts)


def test_max_consecutive_days_counts_run_excess():
    schedule = _week_roster()
    calc = es.DecomposedCalculator([es.MaxConsecutiveDays(value=6)])
    score = calc.score(schedule)
    assert score.hard == -(14 - 6)  # one 14-day run, 8 days over the cap


def test_min_consecutive_days_counts_run_shortfall():
    schedule = _week_roster()
    schedule.shifts[3].employee_index = 1  # B: a single worked day (shortfall 2)
    schedule.shifts[4].employee_index = -1  # split A's run: 3 + 9 days
    calc = es.DecomposedCalculator([es.MinConsecutiveDays(value=3)])
    score = calc.score(schedule)
    assert score.soft == -2  # only B's 1-day run is below 3


def test_weekly_minutes_bounds():
    schedule = _week_roster()  # A: 7 shifts x 480min per week = 3360/week
    calc = es.DecomposedCalculator(
        [
            es.MaxWeeklyMinutes(minutes=3000),  # A: 360 excess x 2 weeks
            es.MinWeeklyMinutes(minutes=1000),  # B: 1000 shortfall x 2 weeks
        ]
    )
    score = calc.score(schedule)
    assert score.hard == -(360 * 2)
    assert score.soft == -(1000 * 2)


def _presence_roster(pattern):
    """One employee A; a shift on each 'W' day of `pattern` (index = day)."""
    shifts = []
    for day, ch in enumerate(pattern):
        if ch == "W":
            s = es.Shift(f"Shift {day}", es.minutes(day, 9), es.minutes(day, 17))
            s.employee_index = 0
            shifts.append(s)
    return es.EmployeeSchedule([es.Employee("A"), es.Employee("B")], shifts)


@pytest.mark.parametrize(
    "pattern, expected",
    [
        ("WWWWWW...WWWWWW...", 0),  # six days on, three off, twice
        ("WWWWWW..WWWWWW", -1),  # 6 on, only 2 rest, back to work -> shortfall 1
        ("WWWWWW..", 0),  # run at end of horizon -> nothing to rest before
        ("WWWWW.W", 0),  # run shorter than work_days
        ("WWWWWW.WWWWWW.W", -4),  # two 1-day gaps after 6-day runs, shortfall 2 each
        ("WWWWWW....WWWWWW", 0),  # 4 rest days is plenty
    ],
)
def test_rest_after_consecutive_work(pattern, expected):
    schedule = _presence_roster(pattern)
    calc = es.DecomposedCalculator([es.RestAfterConsecutiveWork(work_days=6, rest_days=3)])
    assert calc.score(schedule).hard == expected


def test_min_weekly_rest_days_counts_worked_day_excess():
    schedule = _week_roster()  # A works all 14 days: 7 worked days per 7-day week
    calc = es.DecomposedCalculator([es.MinWeeklyRestDays(value=1)])  # cap 6/week
    score = calc.score(schedule)
    assert score.hard == -(1 + 1)  # each week is 1 worked day over the cap


def test_min_weekly_rest_days_counts_a_day_once():
    # A works all 14 days, with a SECOND shift on day 0; it is still one worked
    # day, so the per-week worked-day counts (7, 7) are unchanged.
    shifts = []
    for day in range(14):
        s = es.Shift(f"Shift {day}", es.minutes(day, 9), es.minutes(day, 17))
        s.employee_index = 0
        shifts.append(s)
    extra = es.Shift("X0", es.minutes(0, 19), es.minutes(0, 22))
    extra.employee_index = 0
    shifts.append(extra)
    schedule = es.EmployeeSchedule([es.Employee("A"), es.Employee("B")], shifts)
    calc = es.DecomposedCalculator([es.MinWeeklyRestDays(value=1)])
    assert calc.score(schedule).hard == -(1 + 1)  # day 0 counts once


def test_assignment_cost_matrix_and_defaults():
    schedule = _week_roster()
    calc = es.DecomposedCalculator(
        [
            es.AssignmentCost(
                cost_per_hour=10.0,  # 8h shift = 80
                shift_costs={"A": {"Shift 0": 999.0}},  # override one cell
            )
        ]
    )
    score = calc.score(schedule)
    assert score.soft == -(999.0 + 13 * 80.0)


def test_unassigned_shift_and_balance_gap():
    schedule = _week_roster()
    schedule.shifts[0].employee_index = -1
    calc = es.DecomposedCalculator(
        [
            es.UnassignedShift(),
            es.BalanceMinMaxGap(tolerance=3),  # A=13, B=0 -> gap 13, excess 10
        ]
    )
    score = calc.score(schedule)
    assert score.hard == -1
    assert score.soft == -10


def test_balance_weekly_minutes_gap():
    # A works all 14 days (480 min/day -> 3360 min/week); B never works (0).
    # Two 7-day weeks, each gap 3360; tolerance 120 -> 3240 excess min per week.
    # Cost is in hours: 3240 / 60 = 54 h per week, over two weeks.
    schedule = _week_roster()
    calc = es.DecomposedCalculator([es.BalanceWeeklyMinutesGap(tolerance_minutes=120)])
    assert calc.score(schedule).soft == pytest.approx(-((3360 - 120) / 60 * 2))


def test_balance_weekly_minutes_gap_within_tolerance():
    # A and B each work all 14 days (same hours) -> equal weekly minutes, gap 0.
    shifts = []
    for day in range(14):
        for emp in (0, 1):
            s = es.Shift(f"Shift {day} {emp}", es.minutes(day, 9), es.minutes(day, 17))
            s.employee_index = emp
            shifts.append(s)
    schedule = es.EmployeeSchedule([es.Employee("A"), es.Employee("B")], shifts)
    calc = es.DecomposedCalculator([es.BalanceWeeklyMinutesGap(tolerance_minutes=120)])
    assert calc.score(schedule).soft == 0


def test_min_rest_gap_is_parameterizable():
    schedule = _week_roster()  # 16h gaps between consecutive shifts
    strict = es.DecomposedCalculator([es.MinimumRest(min_gap_minutes=18 * 60)])
    assert strict.score(schedule).hard == -(2 * 60) * 13  # 120min short x 13 gaps
    default = es.DecomposedCalculator([es.MinimumRest()])
    assert default.score(schedule).hard == 0


def test_extended_assembly_solves_and_matches_recompute():
    """Solve with a kitchen-sink assembly; the incremental search result must
    equal a from-scratch recompute by an identical fresh assembly."""

    def build():
        return es.DecomposedCalculator(
            [
                es.RequiredSkill(),
                es.UnavailableEmployee(),
                es.OverlappingShift(),
                es.MaxOneShiftPerDay(),
                es.MinimumRest(),
                es.UndesiredDay(),
                es.DesiredDay(),
                es.MaxConsecutiveDays(value=4),
                es.MinConsecutiveDays(0.5, value=2),
                es.RestAfterConsecutiveWork(work_days=4, rest_days=3),
                es.MaxWeeklyMinutes(minutes=2400),
                es.MinWeeklyMinutes(0.1, minutes=480),
                es.MinWeeklyRestDays(value=1),
                es.AssignmentCost(0.01, cost_per_hour=1.0),
                es.UnassignedShift(),
                es.BalanceMinMaxGap(2.0, tolerance=1),
                es.BalanceWeeklyMinutesGap(0.01, tolerance_minutes=60),
            ]
        )

    schedule = es.make_random_roster(days=10, shifts_per_day=5, employees=8, seed=13)
    cfg = es.SolverConfig()
    cfg.spent_limit_seconds = 0.5
    cfg.random_seed = 13
    es.Solver(cfg).solve(schedule, build())
    assert str(schedule.score) == str(build().score(schedule))


def test_hard_override_moves_the_level():
    schedule = es.make_random_roster(days=6, shifts_per_day=5, employees=8, seed=3)
    for emp in schedule.employees:
        emp.undesired_dates = {1, 4}
    for i, shift in enumerate(schedule.shifts):
        shift.employee_index = i % len(schedule.employees)

    calc = es.DecomposedCalculator()
    calc.add("undesired_day", hard=True)
    score = calc.score(schedule)
    assert score.hard < 0
    assert score.soft == 0


def test_day_pattern_projection_context_is_calculator_owned_and_rebuilt():
    employee = es.Employee("E0", {"Skill A"})
    shifts = [
        es.Shift("evening", es.minutes(0, 16), es.minutes(1, 0), "Evening", "Skill A"),
        es.Shift("gap", es.minutes(1, 8), es.minutes(1, 16), "Morning", "Skill A"),
        es.Shift("morning", es.minutes(2, 8), es.minutes(2, 16), "Morning", "Skill A"),
    ]
    shifts[0].employee_index = 0
    shifts[2].employee_index = 0
    schedule = es.EmployeeSchedule([employee], shifts)

    evening_first = es.DecomposedCalculator()
    evening_first.add("day_pattern", pattern="Evening,OFF,Morning")
    evening_first.add("day_pattern", pattern="Evening,*,Morning")
    morning_first = es.DecomposedCalculator()
    morning_first.add("day_pattern", pattern="Morning,OFF,Evening")

    assert evening_first.score(schedule).soft == -2
    assert morning_first.score(schedule).soft == 0
    # A second calculator must not overwrite the first calculator's context.
    assert evening_first.score(schedule).soft == -2

    schedule.shifts[0].location = "Morning"
    schedule.shifts[2].location = "Evening"
    # resetWorkingSolution advances the context generation and rebuilds the
    # projection from the mutated problem facts.
    assert evening_first.score(schedule).soft == 0
    assert morning_first.score(schedule).soft == -1


@pytest.mark.parametrize(
    "pattern, expected",
    [
        ("WORK,WORK", -2),  # days 0-1 and 1-2 are both worked
        ("!Evening,WORK", -1),  # day 1 is the only worked non-Evening day with work after it
        ("!Morning,WORK", -1),  # day 0, whatever the name of the excluded location
        ("!Elsewhere,WORK", -2),  # a location nobody works excludes nothing
        ("!Morning,OFF", 0),  # an off day never matches a "!x" token
    ],
)
def test_day_pattern_worked_day_tokens(pattern, expected):
    shifts = [
        es.Shift("evening", es.minutes(0, 16), es.minutes(1, 0), "Evening"),
        es.Shift("morning", es.minutes(1, 8), es.minutes(1, 16), "Morning"),
        es.Shift("late-evening", es.minutes(2, 16), es.minutes(3, 0), "Evening"),
    ]
    for shift in shifts:
        shift.employee_index = 0
    schedule = es.EmployeeSchedule([es.Employee("A")], shifts)

    calculator = es.DecomposedCalculator()
    calculator.add("day_pattern", pattern=pattern)
    assert calculator.score(schedule).soft == expected


def test_max_weekly_minutes_week_anchor_aligns_calendar_weeks():
    """week_anchor shifts the 7-day blocks so they align to a calendar week
    boundary. Two 10h shifts on days 6 and 7: with the default day-0 anchor they
    fall in different /7 blocks (no cap breach), but anchored at day 1 they share
    one week (1200 > 900 cap -> 300 over)."""
    employee = es.Employee("A", {"Skill A"})
    shifts = [
        es.Shift("s6", es.minutes(6, 0), es.minutes(6, 10), "Morning", "Skill A"),  # 600 min
        es.Shift("s7", es.minutes(7, 0), es.minutes(7, 10), "Morning", "Skill A"),  # 600 min
    ]
    for s in shifts:
        s.employee_index = 0
    schedule = es.EmployeeSchedule([employee], shifts)

    anchored = es.DecomposedCalculator()
    anchored.add("max_weekly_minutes", minutes=900, week_anchor=1)
    day0 = es.DecomposedCalculator()
    day0.add("max_weekly_minutes", minutes=900, week_anchor=0)

    assert anchored.score(schedule).hard == -300  # days 6,7 in one anchored week
    assert day0.score(schedule).hard == 0  # split across day-0 /7 blocks


def test_worker_pair_same_shift_reward_and_penalty():
    """WorkerPairSameShift over target buckets (day 0 Morning and Evening). REWARD
    (penalty=False): a bucket where exactly one of the pair works it costs 1 -- so
    both on Morning costs 0, A on Morning & B on Evening costs 2 (each is alone in
    one bucket), one working & one off costs 1. PENALTY is the mirror: both on the
    same bucket costs 1."""

    def calc(penalty):
        c = es.DecomposedCalculator()
        c.add(
            "worker_pair_same_shift",
            anchor=0,
            paired=1,
            bucket_days=[0, 0],
            bucket_locations=["Morning", "Evening"],
            penalty=penalty,
        )
        return c

    def score(a_idx, b_idx, penalty):  # build a fresh schedule (ctor copies shifts)
        shifts = [
            es.Shift("Morning 1", es.minutes(0, 8), es.minutes(0, 16), "Morning", ""),
            es.Shift("Morning 2", es.minutes(0, 8), es.minutes(0, 16), "Morning", ""),
            es.Shift("Evening 1", es.minutes(0, 16), es.minutes(1, 0), "Evening", ""),
            es.Shift("Evening 2", es.minutes(0, 16), es.minutes(1, 0), "Evening", ""),
        ]
        if a_idx >= 0:
            shifts[a_idx].employee_index = 0
        if b_idx >= 0:
            shifts[b_idx].employee_index = 1
        sch = es.EmployeeSchedule([es.Employee("A"), es.Employee("B")], shifts)
        return calc(penalty).score(sch).soft

    assert score(0, 1, False) == 0  # both on a Morning slot -> same shift, no reward miss
    assert score(0, 1, True) == -1  # ... but PENALTY: sharing a shift costs 1
    assert score(0, 2, False) == -2  # A on Morning, B on Evening -> each alone in a bucket
    assert score(0, 2, True) == 0  # ... not sharing -> no penalty
    assert score(0, -1, False) == -1  # A works, B off -> one-sided
    assert score(0, -1, True) == 0
