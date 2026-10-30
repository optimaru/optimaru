"""Tests for the pydantic request/response layer (es_scheduling.models)."""

import pytest

pytest.importorskip("pydantic")

import es_scheduling as es  # noqa: E402
from es_scheduling.models import (  # noqa: E402
    AssignmentCostRule,
    MaxConsecutiveDaysRule,
    RequiredSkillRule,
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


def test_duplicate_ids_rejected():
    with pytest.raises(ValueError, match="duplicate"):
        SchedulingInput(
            workers=[WorkerModel(id="A"), WorkerModel(id="A")],
            shifts=[ShiftModel(id="Only shift", start=0, end=60)],
        )
