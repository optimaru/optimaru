"""Tests for the pydantic request/response layer (es_scheduling.models)."""

import os
from types import SimpleNamespace

import pytest

pytest.importorskip("pydantic")

import es_scheduling as es  # noqa: E402
from es_scheduling import models  # noqa: E402
from es_scheduling.demo import generate_demo_data  # noqa: E402
from es_scheduling.models import (  # noqa: E402
    AssignmentCostRule,
    MaxConsecutiveDaysRule,
    RequiredSkillRule,
    RuleTotal,
    ScheduleRequest,
    ScheduleSolution,
    SchedulingInput,
    ShiftModel,
    SolverSettings,
    UnassignedShiftRule,
    WorkerModel,
    default_rules,
    solve_request,
    to_schedule,
)


def _small_input(**kwargs):
    workers = [
        WorkerModel(id="Worker 01", skills=["Skill A"]),
        WorkerModel(id="Worker 02", skills=["Skill A", "Skill B"], undesired_dates=[1]),
    ]
    shifts = [
        ShiftModel(id=f"Shift {d}", start=es.minutes(d, 9), end=es.minutes(d, 17), required_skill="Skill A")
        for d in range(4)
    ]
    return SchedulingInput(workers=workers, shifts=shifts, **kwargs)


def test_solve_request_end_to_end():
    request = ScheduleRequest(
        timelimit=0.3,
        model_input=_small_input(rules=default_rules() + [UnassignedShiftRule()]),
        settings=SolverSettings(random_seed=7),
    )
    solution = solve_request(request)
    assert solution.feasible
    assert not solution.unassigned
    assigned = [a.shift_id for lst in solution.schedule.values() for a in lst]
    assert sorted(assigned) == ["Shift 0", "Shift 1", "Shift 2", "Shift 3"]
    assert {t.name for t in solution.rule_totals} >= {"Missing required skill", "Unassigned shift"}


def test_json_round_trip():
    request = ScheduleRequest(
        timelimit=0.2,
        num_workers=3,
        model_input=_small_input(
            rules=[
                RequiredSkillRule(weight=2),
                MaxConsecutiveDaysRule(value=2, policy="soft", weight=3),
                AssignmentCostRule(cost_per_hour=10.0, shift_costs={"Worker 01": {"Shift 0": 5.0}}),
            ]
        ),
    )
    # The discriminated union must survive JSON serialization.
    clone = ScheduleRequest.model_validate_json(request.model_dump_json())
    assert clone.num_workers == 3
    assert isinstance(clone.model_input.rules[1], MaxConsecutiveDaysRule)
    assert clone.model_input.rules[1].value == 2
    assert clone.model_input.rules[1].hard is False  # policy="soft"

    solution = solve_request(clone)
    round_tripped = ScheduleSolution.model_validate_json(solution.model_dump_json())
    assert round_tripped == solution


def test_solver_settings_expose_engine_knobs():
    request = ScheduleRequest(
        timelimit=0.2,
        model_input=_small_input(rules=default_rules() + [UnassignedShiftRule()]),
        settings=SolverSettings(
            random_seed=9,
            guided_moves=False,
            employee_greedy_construction=False,
            late_acceptance_size=25,
            no_improvement_limit_seconds=0.05,
            step_count_limit=200,
            no_improvement_step_count_limit=100,
            ruin_recreate=False,
            ruin_recreate_rewards=[10.0, 5.0, 1.0, 0.0],
            ruin_recreate_as_move_weight=0.05,
            ruin_recreate_as_move_max_ruin=8,
            descent_weight=0.03,
            descent_max_sweeps=2,
            change_move_weight=1.0,
            swap_move_weight=0.0,
            smart_change_moves=False,
            smart_change_candidate_count=3,
            employee_day_repair_moves=False,
            employee_run_repair_moves=False,
            employee_run_repair_max_days=5,
            employee_window_moves=False,
            employee_window_max_days=2,
            custom_move_probability=0.15,
            employee_ruin_moves=False,
        ),
    )
    clone = ScheduleRequest.model_validate_json(request.model_dump_json())
    assert clone.settings.late_acceptance_size == 25
    assert clone.settings.employee_greedy_construction is False
    assert clone.settings.no_improvement_limit_seconds == pytest.approx(0.05)
    assert clone.settings.step_count_limit == 200
    assert clone.settings.no_improvement_step_count_limit == 100
    assert clone.settings.ruin_recreate is False
    assert clone.settings.ruin_recreate_rewards == [10.0, 5.0, 1.0, 0.0]
    assert clone.settings.ruin_recreate_as_move_weight == pytest.approx(0.05)
    assert clone.settings.ruin_recreate_as_move_max_ruin == 8
    assert clone.settings.descent_weight == pytest.approx(0.03)
    assert clone.settings.descent_max_sweeps == 2
    assert clone.settings.smart_change_moves is False
    assert clone.settings.smart_change_candidate_count == 3
    assert clone.settings.employee_day_repair_moves is False
    assert clone.settings.employee_run_repair_moves is False
    assert clone.settings.employee_run_repair_max_days == 5
    assert clone.settings.employee_window_moves is False
    assert clone.settings.employee_window_max_days == 2
    assert clone.settings.custom_move_probability == pytest.approx(0.15)
    assert clone.settings.employee_ruin_moves is False

    solution = solve_request(clone)
    assert isinstance(solution, ScheduleSolution)


def test_solver_settings_reject_invalid_ruin_options():
    with pytest.raises(ValueError):
        SolverSettings(ruin_recreate_rewards=[1.0, 2.0, 3.0])
    with pytest.raises(ValueError, match="must be <="):
        SolverSettings(ruin_recreate_min_ruin_fraction=0.5, ruin_recreate_max_ruin_fraction=0.1)
    with pytest.raises(ValueError, match="must match"):
        SolverSettings(no_improvement_limit_seconds=0.1, unimproved_spent_limit_seconds=0.2)
    with pytest.raises(ValueError, match="must match"):
        SolverSettings(no_improvement_step_count_limit=10, unimproved_step_count_limit=20)


def test_rules_flow_into_calculator_unchanged():
    """A pydantic rule list is a valid constraint-spec list for the core."""
    schedule = to_schedule(_small_input())
    for shift in schedule.shifts:
        shift.employee_index = 0
    calc = es.DecomposedCalculator([MaxConsecutiveDaysRule(value=2)])
    score = calc.score(schedule)
    assert score.hard == -2  # 4-day run, 2 over the cap


def test_preassigned_unknown_worker_raises():
    model_input = _small_input()
    model_input.shifts[0].worker = "Worker 99"
    with pytest.raises(ValueError, match="unknown worker"):
        to_schedule(model_input)


@pytest.mark.parametrize(
    ("start", "end"),
    [(-60, 60), (-3 * 1440, -3 * 1440 + 60), (60, 60), (120, 60)],
    ids=["before-day-0", "days-before-day-0", "empty", "reversed"],
)
def test_a_shift_needs_times_from_day_0_and_an_end_after_its_start(start, end):
    with pytest.raises(ValueError):
        ShiftModel(id="Shift 1", start=start, end=end)
    with pytest.raises(ValueError, match="need 0 <= start < end"):
        es.calculate_score(es.EmployeeSchedule([es.Employee("A")], [es.Shift("Shift 1", start, end)]))


def test_a_shift_cannot_run_past_the_last_day():
    start = 36526 * 1440
    with pytest.raises(ValueError, match="up to day 36525"):
        es.calculate_score(es.EmployeeSchedule([es.Employee("A")], [es.Shift("Shift 1", start, start + 60)]))


def test_a_pinned_shift_without_a_worker_fails_when_built():
    with pytest.raises(ValueError, match="pinned but has no worker"):
        ShiftModel(id="Shift 1", start=0, end=60, pinned=True)


def test_an_unknown_worker_fails_when_the_input_is_built():
    with pytest.raises(ValueError, match="unknown worker 'Worker 99'"):
        SchedulingInput(
            workers=[WorkerModel(id="Worker 01")],
            shifts=[ShiftModel(id="Shift 1", start=0, end=60, worker="Worker 99")],
        )


def test_duplicate_ids_rejected():
    with pytest.raises(ValueError, match="duplicate"):
        SchedulingInput(
            workers=[WorkerModel(id="A"), WorkerModel(id="A")],
            shifts=[ShiftModel(id="Only shift", start=0, end=60)],
        )


def _multi_start_request(**settings):
    return ScheduleRequest(timelimit=0.05, model_input=_small_input(), settings=SolverSettings(**settings))


def _start_count(request):
    """How many candidates `solve_request` hands to `select`."""
    counts = []

    def select(solutions):
        counts.append(len(solutions))
        return solutions[0]

    solve_request(request, select=select)
    return counts[0]


@pytest.mark.parametrize(
    ("num_workers", "multi_start", "starts"),
    [(3, 1, 3), (1, 3, 3), (2, 4, 4), (4, 2, 4)],
)
def test_starts_are_the_larger_of_num_workers_and_multi_start(num_workers, multi_start, starts):
    request = _multi_start_request(multi_start=multi_start)
    request.num_workers = num_workers
    assert _start_count(request) == starts


def test_a_single_start_ignores_select():
    solution = solve_request(_multi_start_request(), select=lambda solutions: pytest.fail("select was called"))
    assert isinstance(solution, ScheduleSolution)


@pytest.mark.parametrize("key", ["no_such_knob", "spent_limit_seconds", "random_seed", "on_new_best", "__class__"])
def test_a_bad_portfolio_key_fails_before_any_solve(key):
    body = {
        "timelimit": 30,
        "model_input": _small_input().model_dump(),
        "settings": {"multi_start": 4, "portfolio": [{"late_acceptance_size": 50}, {key: 1}]},
    }
    with pytest.raises(ValueError, match=rf"portfolio\[1\]: .*{key}"):
        ScheduleRequest.model_validate(body)


def test_every_portfolio_key_is_a_solver_config_attribute():
    config = es.SolverConfig()
    assert [key for key in models._PORTFOLIO_KEYS if not hasattr(config, key)] == []


def test_portfolio_overrides_reach_each_start():
    request = _multi_start_request(multi_start=2, portfolio=[{"late_acceptance_size": 50}, {"descent_weight": 0.0}])
    assert _start_count(request) == 2


def test_an_error_in_a_start_is_not_retried(monkeypatch, tmp_path):
    calls = tmp_path / "calls"

    def failing_start(request, seed, overrides=None):
        with calls.open("a") as log:
            log.write(".")
        raise RuntimeError("the start failed")

    monkeypatch.setattr(models, "_build_and_solve", failing_start)
    with pytest.raises(RuntimeError, match="the start failed"):
        solve_request(_multi_start_request(multi_start=3))
    assert calls.read_text() == "..."


def _refuse(*args, **kwargs):
    raise OSError(38, "Function not implemented")


def test_starts_run_in_turn_without_fork(monkeypatch):
    def no_fork(method):
        raise ValueError(f"cannot find context for {method!r}")

    monkeypatch.setattr("multiprocessing.get_context", no_fork)
    assert _start_count(_multi_start_request(multi_start=2)) == 2


def test_starts_run_in_turn_without_working_semaphores(monkeypatch):
    monkeypatch.setattr("concurrent.futures.ProcessPoolExecutor", _refuse)
    assert _start_count(_multi_start_request(multi_start=2)) == 2


def test_starts_run_in_turn_when_fork_fails(monkeypatch):
    monkeypatch.setattr("os.fork", _refuse)
    assert _start_count(_multi_start_request(multi_start=2)) == 2


def test_starts_run_in_turn_in_a_daemonic_process(monkeypatch):
    monkeypatch.setattr("multiprocessing.current_process", lambda: SimpleNamespace(daemon=True))
    assert _start_count(_multi_start_request(multi_start=2)) == 2


def test_starts_run_in_turn_after_a_worker_dies(monkeypatch):
    parent = os.getpid()
    solve = models._build_and_solve

    def die_in_a_worker(*args):
        if os.getpid() != parent:
            os._exit(1)
        return solve(*args)

    monkeypatch.setattr(models, "_build_and_solve", die_in_a_worker)
    assert _start_count(_multi_start_request(multi_start=2)) == 2


def _candidate(score_hard, score_soft, coverage_cost=0.0, is_hard=False):
    return ScheduleSolution(
        schedule={},
        score_hard=score_hard,
        score_soft=score_soft,
        feasible=score_hard == 0,
        rule_totals=[RuleTotal(name="Shift coverage[any 1..]", cost=coverage_cost, is_hard=is_hard)],
    )


def _winning_seed(monkeypatch, candidates, divisor):
    """The seed `solve_request` picks when start i returns candidates[i]."""
    monkeypatch.setattr(models, "_build_and_solve", lambda request, seed, overrides=None: candidates[seed])
    winner = solve_request(_multi_start_request(multi_start=len(candidates), select_coverage_divisor=divisor))
    return candidates.index(winner)


def test_selection_ranks_on_the_reported_score_without_a_divisor(monkeypatch):
    candidates = [_candidate(0, -100.0, coverage_cost=90.0), _candidate(0, -50.0)]
    assert _winning_seed(monkeypatch, candidates, divisor=1.0) == 1


def test_selection_divides_the_coverage_boost_out(monkeypatch):
    # Faithful soft: -100 + 90 * (1 - 1/10) = -19 against -50.
    candidates = [_candidate(0, -100.0, coverage_cost=90.0), _candidate(0, -50.0)]
    assert _winning_seed(monkeypatch, candidates, divisor=10.0) == 0


def test_selection_keeps_hard_ahead_of_faithful_soft(monkeypatch):
    candidates = [_candidate(-1, -100.0, coverage_cost=100.0), _candidate(0, -50.0)]
    assert _winning_seed(monkeypatch, candidates, divisor=10.0) == 1


def test_selection_leaves_a_hard_coverage_rule_alone(monkeypatch):
    candidates = [_candidate(0, -100.0, coverage_cost=90.0, is_hard=True), _candidate(0, -50.0)]
    assert _winning_seed(monkeypatch, candidates, divisor=10.0) == 1


def test_selection_finds_the_coverage_rule_by_its_engine_name():
    request = generate_demo_data(seed=3, workers=8, days=7)
    request.timelimit = 0.05
    names = [total.name for total in solve_request(request).rule_totals]
    assert any(name.startswith("Shift coverage") for name in names), names
