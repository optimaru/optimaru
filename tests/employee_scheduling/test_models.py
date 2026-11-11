"""Tests for the pydantic request/response layer (optimaru.employee_scheduling.models)."""

import os
from types import SimpleNamespace

import pytest
from pydantic import ValidationError

import optimaru.employee_scheduling as es
from optimaru.employee_scheduling import models
from optimaru.employee_scheduling.constraints import (
    SPEC_CLASSES,
    AssignmentCost,
    DayPatternCost,
    MaxConsecutiveDaysCost,
    RequiredSkillCost,
    UnassignedShiftsCost,
    UndesiredDayCost,
    WorkerDayCountInRangeCost,
)
from optimaru.employee_scheduling.demo import generate_demo_data
from optimaru.employee_scheduling.models import (
    CONSTRAINT_MODELS,
    ConstraintTotal,
    ScheduleRequest,
    ScheduleSolution,
    SchedulingInput,
    ShiftModel,
    SolverSettings,
    WorkerModel,
    constraint_model,
    default_constraints,
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
        model_input=_small_input(constraints=default_constraints() + [UnassignedShiftsCost()]),
        settings=SolverSettings(random_seed=7),
    )
    solution = solve_request(request)
    assert solution.feasible
    assert not solution.unassigned
    assigned = [a.shift_id for lst in solution.schedule.values() for a in lst]
    assert sorted(assigned) == ["Shift 0", "Shift 1", "Shift 2", "Shift 3"]
    assert {t.name for t in solution.constraint_totals} >= {"required_skill_cost", "unassigned_shifts_cost"}


def test_json_round_trip():
    request = ScheduleRequest(
        timelimit=0.2,
        num_workers=3,
        model_input=_small_input(
            constraints=[
                RequiredSkillCost(weight=2),
                MaxConsecutiveDaysCost(value=2, level="soft", weight=3),
                AssignmentCost(cost_per_hour=10.0, shift_costs={"Worker 01": {"Shift 0": 5.0}}),
            ]
        ),
    )
    # The discriminated union must survive JSON serialization.
    clone = ScheduleRequest.model_validate_json(request.model_dump_json())
    assert clone.num_workers == 3
    assert isinstance(clone.model_input.constraints[1], constraint_model(MaxConsecutiveDaysCost))
    assert clone.model_input.constraints[1].value == 2
    assert clone.model_input.constraints[1].level == "soft"
    assert clone.model_input.constraints[1].to_spec() == MaxConsecutiveDaysCost(value=2, level="soft", weight=3)

    solution = solve_request(clone)
    round_tripped = ScheduleSolution.model_validate_json(solution.model_dump_json())
    assert round_tripped == solution


def test_solver_settings_expose_engine_knobs():
    request = ScheduleRequest(
        timelimit=0.2,
        model_input=_small_input(constraints=default_constraints() + [UnassignedShiftsCost()]),
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
    """A pydantic constraint list is a valid constraint-spec list for the core, and so are its specs."""
    schedule = to_schedule(_small_input())
    for shift in schedule.shifts:
        shift.employee_index = 0
    constraint = constraint_model("max_consecutive_days_cost")(value=2)
    assert es.DecomposedCalculator([constraint]).score(schedule).hard == -2  # 4-day run, 2 over the cap
    assert es.DecomposedCalculator([constraint.to_spec()]).score(schedule).hard == -2


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


@pytest.mark.parametrize("bounds", [{"lo": -1}, {"hi": 36526}, {"hi": -2}], ids=["lo", "hi", "below-default"])
def test_a_day_range_outside_the_limit_fails_when_built(bounds):
    with pytest.raises(ValueError):
        WorkerDayCountInRangeCost(**bounds)
    with pytest.raises(ValueError):
        constraint_model(WorkerDayCountInRangeCost)(**bounds)
    assert WorkerDayCountInRangeCost(lo=0, hi=36525).hi == 36525
    assert constraint_model(WorkerDayCountInRangeCost)(lo=0, hi=36525).hi == 36525


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
        constraint_totals=[
            ConstraintTotal(
                name="shift_coverage_cost",
                label="Shift coverage[any 1..inf]",
                cost=coverage_cost,
                level="hard" if is_hard else "soft",
                is_hard=is_hard,
            )
        ],
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
    totals = solve_request(request).constraint_totals
    coverage = [total for total in totals if total.name == "shift_coverage_cost"]
    assert [total.label for total in coverage] == ["Shift coverage[Skill A 1..inf]"], totals


# ---------------------------------------------------------------------------
# The constraint models are generated from the constraint specs
# ---------------------------------------------------------------------------


def test_there_is_one_rule_model_per_spec_in_factory_order():
    assert list(CONSTRAINT_MODELS) == [spec.name for spec in SPEC_CLASSES] == list(es.CONSTRAINT_NAMES)
    for spec in SPEC_CLASSES:
        model = constraint_model(spec)
        assert model.spec_class is spec
        assert model.__name__ == f"{spec.__name__}Model"  # no "Rule" suffix
        assert getattr(models, model.__name__) is model  # picklable by reference
        assert set(model.model_fields) == {"name", "weight", "level", "exponent", "offset", "clip", *spec().params()}
        assert model().to_spec() == spec()


def test_default_rules_are_the_default_specs():
    assert [constraint.to_spec() for constraint in default_constraints()] == es.constraints.get_constraints()


def test_spec_instances_and_json_dicts_are_both_rules():
    (first,) = _small_input(constraints=[MaxConsecutiveDaysCost(value=2)]).constraints
    (second,) = _small_input(constraints=[{"name": "max_consecutive_days_cost", "value": 2}]).constraints
    assert first == second
    assert first.to_spec() == MaxConsecutiveDaysCost(value=2)


def test_a_rule_may_repeat_with_other_parameters():
    request = ScheduleRequest(
        timelimit=0.05,
        model_input=_small_input(constraints=[DayPatternCost(pattern="WORK,WORK"), DayPatternCost(pattern="OFF,WORK")]),
    )
    solution = solve_request(request)
    assert [(t.name, t.label, t.level) for t in solution.constraint_totals] == [
        ("day_pattern_cost", "Day pattern[WORK,WORK]", "soft"),
        ("day_pattern_cost", "Day pattern[OFF,WORK]", "soft"),
    ]


@pytest.mark.parametrize(
    "constraint",
    [
        {"name": "required_skill"},  # an unknown name
        {"name": "required_skill_cost", "policy": "hard"},  # an unknown field
        {"name": "required_skill_cost", "hard": True},  # the level is `level`
        {"name": "max_consecutive_days_cost", "value": 0},  # below the spec's bound
        {"name": "desired_day_cost", "level": "hard"},  # a reward is never hard
        {"name": "shift_count_imbalance_cost", "worker_indices": [0]},  # no scope on this global
        {"name": "required_skill_cost", "weight": -1},
        {"name": "required_skill_cost", "exponent": 0},
        {"name": "required_skill_cost", "level": "yes"},
        {"name": "required_skill_cost", "level": "medium"},  # no medium level in this use case
        {"name": "required_skill_cost", "weight": True},  # the spec refuses a bool weight
        {"name": "required_skill_cost", "weight": "2"},  # ... and a numeric string
        {"name": "required_skill_cost", "exponent": True},
        {"name": "required_skill_cost", "exponent": "2"},
        {"name": "required_skill_cost", "offset": "5"},
        {"name": "required_skill_cost", "offset": False},
        {"name": "required_skill_cost", "offset": float("inf")},
        {"name": "required_skill_cost", "clip": 1},
        {"name": "required_skill_cost", "clip": "no"},
    ],
    ids=[
        "unknown-name",
        "unknown-field",
        "hard-key",
        "bound",
        "reward-hard",
        "global-scope",
        "weight",
        "exponent",
        "hard-type",
        "level-medium",
        "weight-bool",
        "weight-str",
        "exponent-bool",
        "exponent-str",
        "offset-str",
        "offset-bool",
        "offset-inf",
        "clip-int",
        "clip-str",
    ],
)
def test_a_bad_rule_fails_validation(constraint):
    with pytest.raises(ValidationError):
        SchedulingInput.model_validate({**_small_input().model_dump(), "constraints": [constraint]})


def test_the_json_schema_carries_the_spec_bounds():
    schema = constraint_model(MaxConsecutiveDaysCost).model_json_schema()
    assert schema["properties"]["name"]["const"] == "max_consecutive_days_cost"
    assert schema["properties"]["value"]["minimum"] == 1
    assert schema["properties"]["weight"]["anyOf"][0]["minimum"] == 0
    day_range = constraint_model(WorkerDayCountInRangeCost).model_json_schema()["properties"]
    assert day_range["hi"]["minimum"] == -1 and day_range["hi"]["maximum"] == es.MAX_DAY


def test_integral_json_knobs_stay_numbers():
    """weight 2 (a JSON integer) is a number, not a type error."""
    constraint = SchedulingInput.model_validate(
        {**_small_input().model_dump(), "constraints": [{"name": "required_skill_cost", "weight": 2, "exponent": 1}]}
    ).constraints[0]
    assert constraint.to_spec() == RequiredSkillCost(weight=2.0)


def test_no_generated_model_is_named_rule():
    """The per-constraint JSON models end in "Model", never in "Rule"."""
    assert not [name for name in dir(models) if name.endswith("CostRule")]
    with pytest.raises(ImportError):
        exec("from optimaru.employee_scheduling.models import AssignmentCostRule", {})
    assert "MaxConsecutiveDaysCostModel" in SchedulingInput.model_json_schema()["$defs"]


def test_assigning_specs_to_rules_is_validated():
    """input.constraints = [specs] after construction goes through the validators (it skipped them)."""
    model_input = _small_input()
    model_input.constraints = [RequiredSkillCost(), MaxConsecutiveDaysCost(value=2)]
    assert [constraint.to_spec() for constraint in model_input.constraints] == [
        RequiredSkillCost(),
        MaxConsecutiveDaysCost(value=2),
    ]
    solution = solve_request(ScheduleRequest(timelimit=0.05, model_input=model_input))
    assert {total.name for total in solution.constraint_totals} == {"required_skill_cost", "max_consecutive_days_cost"}
    with pytest.raises(ValidationError):
        model_input.constraints = [{"name": "max_consecutive_days_cost", "value": 0}]


def test_an_unvalidated_spec_in_rules_still_solves():
    """An in-place edit bypasses pydantic; the solve still takes a spec found there."""
    model_input = _small_input()
    model_input.constraints.append(MaxConsecutiveDaysCost(value=2))
    solution = solve_request(ScheduleRequest(timelimit=0.05, model_input=model_input))
    assert "max_consecutive_days_cost" in {total.name for total in solution.constraint_totals}


def _all_on_the_first_worker():
    model_input = _small_input()
    for shift in model_input.shifts:
        shift.worker = "Worker 01"
    return to_schedule(model_input)


def test_an_empty_rule_list_is_refused():
    """The get_constraints() constraint holds for SchedulingInput.constraints too."""
    with pytest.raises(ValidationError, match="must not be empty"):
        _small_input(constraints=[])


@pytest.mark.parametrize(
    "constraints",
    [
        [RequiredSkillCost(), RequiredSkillCost()],
        [MaxConsecutiveDaysCost(value=2), {"name": "max_consecutive_days_cost", "value": 2}],
        [MaxConsecutiveDaysCost(value=2), MaxConsecutiveDaysCost(value=2, weight=3, offset=1)],
    ],
    ids=["same-twice", "spec-and-json-twice", "other-knobs"],
)
def test_a_repeated_rule_is_one_instance_per_entry(constraints):
    """The same constraint may repeat with the same parameters: each entry is scored."""
    model_input = _small_input(constraints=constraints)
    assert len(model_input.constraints) == 2
    solution = solve_request(ScheduleRequest(timelimit=0.05, model_input=model_input))
    assert [total.name for total in solution.constraint_totals] == [constraints[0].name] * 2


def test_each_repeated_rule_keeps_its_own_level():
    """A JSON constraint repeats a spec constraint on another level, with an offset: two instances, two levels."""
    constraints = [
        MaxConsecutiveDaysCost(value=2),
        {"name": "max_consecutive_days_cost", "value": 2, "level": "soft", "weight": 3, "offset": 1},
    ]
    model_input = _small_input(constraints=constraints)
    assert [constraint.level for constraint in model_input.constraints] == [None, "soft"]
    solution = solve_request(ScheduleRequest(timelimit=0.05, model_input=model_input))
    assert [(total.name, total.level) for total in solution.constraint_totals] == [
        ("max_consecutive_days_cost", "hard"),
        ("max_consecutive_days_cost", "soft"),
    ]


def test_the_cpp_calculator_accepts_a_repeated_selection():
    """DecomposedCalculator(list) follows get_constraints(): empty is refused, a repeat is an instance."""
    with pytest.raises(ValueError, match="must not be empty"):
        es.DecomposedCalculator([])
    schedule = _all_on_the_first_worker()
    twice = es.DecomposedCalculator([MaxConsecutiveDaysCost(value=1), MaxConsecutiveDaysCost(value=1)])
    once = es.DecomposedCalculator([MaxConsecutiveDaysCost(value=1)])
    assert once.score(schedule).hard == -3  # four days in a row against a limit of 1
    assert twice.score(schedule).hard == 2 * once.score(schedule).hard
    raw = es.constraint_costs(schedule, [DayPatternCost(pattern="WORK,WORK")] * 2)
    assert list(raw) == ["day_pattern_cost", "day_pattern_cost#2"]
    # Other knobs alone make another instance too, as do other parameters (or scope).
    es.DecomposedCalculator([MaxConsecutiveDaysCost(value=1), MaxConsecutiveDaysCost(value=1, weight=2)])
    es.DecomposedCalculator([MaxConsecutiveDaysCost(value=1), MaxConsecutiveDaysCost(value=2)])
    es.DecomposedCalculator([MaxConsecutiveDaysCost(value=1), MaxConsecutiveDaysCost(value=1, worker_indices=(0,))])


def test_a_list_and_a_tuple_scope_are_two_instances_of_the_same_rule():
    """A list and a tuple scope are the same parameters; picked twice, they count twice on every path."""
    a = MaxConsecutiveDaysCost(value=5, worker_indices=(0, 1))
    b = MaxConsecutiveDaysCost(value=5, worker_indices=[0, 1])
    assert len(es.constraints.get_constraints([a, b])) == 2
    schedule = _all_on_the_first_worker()
    raw = es.constraint_costs(schedule, [a, b])
    assert list(raw) == ["max_consecutive_days_cost", "max_consecutive_days_cost#2"]
    assert raw["max_consecutive_days_cost"]["costs"] == raw["max_consecutive_days_cost#2"]["costs"]
    assert raw["max_consecutive_days_cost"]["units"] == raw["max_consecutive_days_cost#2"]["units"] == [0, 1]
    single = es.DecomposedCalculator([a]).score(schedule)
    double = es.DecomposedCalculator([a, b]).score(schedule)
    assert (double.hard, double.soft) == (2 * single.hard, 2 * single.soft)


def test_offset_and_clip_round_trip_through_json():
    """The knobs are constraint fields (not domain parameters): spec -> JSON -> spec keeps them."""
    spec = UndesiredDayCost(weight=0.5, exponent=2.0, offset=120, clip=False)
    (constraint,) = _small_input(constraints=[spec]).constraints
    assert (constraint.offset, constraint.clip) == (120.0, False)
    assert constraint.to_spec() == spec
    assert constraint.params() == spec.params()
    assert "offset" not in constraint.params() and "clip" not in constraint.params()
    dumped = constraint.model_dump(mode="json")
    assert dumped["offset"] == 120.0 and dumped["clip"] is False
    again = SchedulingInput.model_validate({**_small_input().model_dump(), "constraints": [dumped]})
    assert again.constraints[0].to_spec() == spec
    defaults = constraint_model(UndesiredDayCost)()
    assert (defaults.offset, defaults.clip) == (0.0, True)


def test_the_json_schema_carries_the_offset_and_clip_knobs():
    properties = constraint_model(MaxConsecutiveDaysCost).model_json_schema()["properties"]
    assert properties["offset"]["default"] == 0.0 and properties["offset"]["type"] == "number"
    assert properties["clip"]["default"] is True and properties["clip"]["type"] == "boolean"


# --- clip=False with an offset stays off the hard score -------------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _validation_message(call):
    """The one error of a ValidationError, as the model validator raised it."""
    with pytest.raises(ValidationError) as refused:
        call()
    (error,) = refused.value.errors()
    return str(error["ctx"]["error"])


@pytest.mark.parametrize("offset", [120, -120])
@pytest.mark.parametrize(
    "constraint",
    [
        lambda offset: {"name": "required_skill_cost", "offset": offset, "clip": False},  # default hard
        lambda offset: {"name": "undesired_day_cost", "level": "hard", "offset": offset, "clip": False},
        lambda offset: {"name": "max_consecutive_days_cost", "value": 3, "offset": offset, "clip": False},
    ],
    ids=["skill-default-hard", "undesired-moved-to-hard", "max-days-default-hard"],
)
def test_a_json_rule_refuses_the_literal_offset_on_hard(constraint, offset):
    """The JSON constraint models refuse it when they are validated, in the engine's words: as a
    constraint model, inside a request, and from a spec instance."""
    data = constraint(offset)
    message = HARD_REFUSAL.format(q=repr(data["name"]))
    model = constraint_model(data["name"])
    assert _validation_message(lambda: model.model_validate(data)) == message
    payload = {**_small_input().model_dump(), "constraints": [{"name": "required_skill_cost"}, data]}
    assert _validation_message(lambda: SchedulingInput.model_validate(payload)) == message
    spec = model.spec_class(**{key: value for key, value in data.items() if key != "name"})
    # The spec itself is accepted until it is scored; a constraint list refuses it.
    assert _validation_message(lambda: _small_input(constraints=[spec])) == message


@pytest.mark.parametrize("offset", [120, -120])
def test_solve_request_refuses_a_literal_hard_rule_added_in_place(offset):
    """An in-place edit of the constraint list bypasses pydantic; the engine refuses the pick."""
    request = ScheduleRequest(timelimit=0.05, model_input=_small_input(constraints=default_constraints()))
    request.model_input.constraints.append(RequiredSkillCost(offset=offset, clip=False))
    with pytest.raises(ValueError) as refused:
        solve_request(request)
    assert str(refused.value) == HARD_REFUSAL.format(q="'required_skill_cost'")


@pytest.mark.parametrize(
    "constraint",
    [
        {"name": "required_skill_cost", "level": "soft", "offset": 120, "clip": False},
        {"name": "required_skill_cost", "offset": 120},  # a threshold on hard
        {"name": "required_skill_cost", "offset": 0, "clip": False},  # clip=False with offset 0
        {"name": "undesired_day_cost", "offset": -120, "clip": False},  # soft by default
    ],
    ids=["soft-literal", "hard-threshold", "hard-offset-0", "default-soft-literal"],
)
def test_the_allowed_literal_and_threshold_rules_solve(constraint):
    request = ScheduleRequest(
        timelimit=0.05,
        model_input=SchedulingInput.model_validate({**_small_input().model_dump(), "constraints": [constraint]}),
    )
    solution = solve_request(request)
    (total,) = solution.constraint_totals
    assert total.name == constraint["name"]
    assert total.level == constraint.get("level", "soft" if constraint["name"] == "undesired_day_cost" else "hard")


def test_the_earlier_rule_names_are_aliases():
    """rules / rule_totals in JSON and as attributes, and the old functions and classes, are
    the constraint names under a second name; a dump uses the new ones."""
    from optimaru.employee_scheduling import models

    request = generate_demo_data(workers=6, days=4)
    data = request.model_input.model_dump()
    data["rules"] = data.pop("constraints")[:3]
    model_input = models.SchedulingInput.model_validate(data)
    assert len(model_input.constraints) == 3 and model_input.rules is model_input.constraints
    model_input.rules = models.default_rules()[:2]
    assert len(model_input.constraints) == 2
    with pytest.raises(ValueError):
        model_input.rules = []  # the setter validates, as assignment to constraints does
    assert "constraints" in model_input.model_dump() and "rules" not in model_input.model_dump()
    copied = model_input.model_copy(update={"rules": models.default_rules()[:1]})
    assert len(copied.constraints) == 1 and len(model_input.constraints) == 2

    solved = solve_request(request.model_copy(update={"time_limit_seconds": 0.0}))
    as_before = solved.model_dump()
    as_before["rule_totals"] = as_before.pop("constraint_totals")
    read = models.ScheduleSolution.model_validate(as_before)
    assert read.constraint_totals == solved.constraint_totals and read.rule_totals is read.constraint_totals

    assert models.Rule is models.ConstraintModel
    assert models.RuleBase is models.ConstraintModelBase
    assert models.RuleTotal is models.ConstraintTotal
    assert models.RULE_MODELS is models.CONSTRAINT_MODELS
    assert models.rule_model is models.constraint_model
    assert models.default_rules is models.default_constraints


def test_solve_request_takes_constraints_in_place_of_the_request_s_own():
    """constraints= replaces model_input.constraints for the solve and leaves the request as it
    was; a calculator or a name is refused."""
    from optimaru.employee_scheduling import DecomposedCalculator
    from optimaru.employee_scheduling.constraints import RequiredSkillCost, get_constraints

    request = generate_demo_data(workers=6, days=4).model_copy(update={"time_limit_seconds": 0.0})
    before = request.model_dump()
    solved = solve_request(request, constraints=[RequiredSkillCost()])
    assert [total.name for total in solved.constraint_totals] == ["required_skill_cost"]
    assert request.model_dump() == before
    assert len(solve_request(request).constraint_totals) == len(request.model_input.constraints)
    with pytest.raises(TypeError, match="DecomposedCalculator"):
        solve_request(request, constraints=DecomposedCalculator(get_constraints()))
    with pytest.raises(TypeError, match="names"):
        solve_request(request, constraints=["required_skill_cost"])
