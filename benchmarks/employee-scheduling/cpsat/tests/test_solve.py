import random
import typing
from collections import Counter

import pytest
import es_scheduling
from es_scheduling import constraints as domain
from es_scheduling import models
from es_scheduling.demo import generate_demo_data
from es_scheduling.models import (
    ScheduleRequest,
    SchedulingInput,
    ShiftModel,
    WorkerModel,
)

from employee_scheduling_cpsat import catalog, constraint_costs, solve
from employee_scheduling_cpsat.constraints import PLUGINS, SCALE, registry_for

DAY = 24 * 60

# Every rule type, at values that make it bind on `_problem`. Several rule types
# appear more than once where a parameter switches to another code path.
RULES = [
    domain.RequiredSkillCost(),
    # A second instance needs other parameters (the same rule with the same
    # parameters twice is refused): a scoped, demoted copy.
    domain.RequiredSkillCost(weight=3, hard=False, worker_indices=(0, 1)),
    domain.EmployeeUnavailabilityCost(),
    domain.ShiftOverlapCost(),
    domain.MaxOneShiftPerDayCost(),
    domain.MinRestCost(),
    domain.MinRestCost(min_gap_minutes=120),
    domain.UndesiredDayCost(),
    domain.UndesiredDayCost(per_day=True),
    domain.DesiredDayCost(),
    domain.DesiredDayCost(per_day=True),
    domain.DesiredShiftCost(),
    domain.ShiftCountImbalanceCost(),
    domain.MaxConsecutiveDaysCost(value=2),
    domain.MinConsecutiveDaysCost(value=3),
    domain.MaxConsecutiveDaysOffCost(value=1),
    domain.RestAfterConsecutiveWorkCost(work_days=2, rest_days=3),
    domain.MaxWeeklyMinutesCost(minutes=600),
    domain.MaxWeeklyMinutesCost(minutes=600, week_anchor=3),
    domain.MinWeeklyMinutesCost(minutes=900),
    domain.MinWeeklyMinutesCost(minutes=900, week_anchor=3),
    domain.MinWeeklyRestDaysCost(value=5),
    domain.AssignmentCost(
        cost_per_hour=1.5,
        cost_per_shift=2.0,
        shift_costs={"Worker 1": {"Shift 1": 9.25}},
    ),
    domain.AssignmentCost(cost_per_shift=2.0),
    domain.UnassignedShiftsCost(),
    domain.ShiftCountGapCost(tolerance=1),
    domain.WeeklyMinutesGapCost(tolerance_minutes=100),
    domain.ShiftCoverageCost(worker_skill="Skill A", min_count=1, max_count=1),
    domain.ShiftCoverageCost(
        min_count=2, worker_indices=(0, 1), shift_indices=(0, 2, 3)
    ),
    domain.ShiftCoverageCost(
        min_count=1, worker_indices=(1, 1, 99), shift_indices=(0, 0, 2, -1)
    ),
    domain.LocationRunLengthCost(location="Location A", min_run=2, max_run=3),
    domain.LocationRunLengthCost(min_run=3, max_run=0, waive_min_at_end=False),
    domain.RestAfterLocationRunCost(location="Location A", rest_days=2),
    domain.LocationRunGapCost(location="Location A", min_gap=3),
    domain.WorkerLocationCountCost(location="Location A", min_count=1, max_count=2),
    domain.WorkerLocationCountCost(worker_skill="Skill B", min_count=4),
    domain.DayPatternCost(pattern="Location A,OFF,Location B"),
    domain.DayPatternCost(pattern="!Location B,*,WORK"),
    domain.DayPatternCost(pattern="OFF,Location Z"),
    domain.TargetDaysOffCost(target_days=(0, 5, 6, 99), min_count=2),
    domain.WorkerDayCountInRangeCost(lo=1, hi=5, min_count=2, max_count=3),
    domain.WorkerDayCountInRangeCost(location="Location B", lo=0, hi=9, max_count=1),
    domain.LocationImbalanceCost(location="Location A"),
    domain.LocationImbalanceCost(worker_skill="Skill A", metric="gap", tolerance=1),
    domain.LocationImbalanceCost(off_target_days=(0, 1, 2, 9)),
    domain.WorkerPairingCost(
        anchor=0, paired=1, bucket_days=(0, 1, 2), bucket_locations=("Location A",) * 3
    ),
    domain.WorkerPairingCost(
        anchor=0,
        paired=1,
        bucket_days=(1, 2),
        bucket_locations=("Location B", "Location A"),
        penalty=True,
    ),
    domain.MaxConsecutiveDaysCost(value=1, worker_indices=(0, 2, 2, 99, -1)),
    domain.MinRestCost(shift_indices=tuple(range(0, 24, 2))),
    domain.DesiredDayCost(worker_indices=()),
    domain.DayPatternCost(
        pattern="Location A,WORK", worker_indices=(1,), shift_indices=tuple(range(12))
    ),
]


def _problem(rules: list, seed: int = 3) -> ScheduleRequest:
    """Ten days of mixed shifts, among them overlaps and midnight crossings."""
    rng = random.Random(seed)
    workers = [
        WorkerModel(
            id=f"Worker {index}",
            skills=rng.sample(["Skill A", "Skill B", "Skill C"], 2),
            unavailable_dates=[rng.randrange(10)],
            undesired_dates=rng.sample(range(10), 2),
            desired_dates=rng.sample(range(10), 2),
            desired_shifts=[(rng.randrange(10), "Location B")],
        )
        for index in range(4)
    ]
    shifts = []
    for index in range(24):
        day = index % 10 if index % 7 else (index + 1) % 10
        start = day * DAY + rng.choice([0, 8 * 60, 8 * 60, 16 * 60, 22 * 60])
        shifts.append(
            ShiftModel(
                id=f"Shift {index}",
                start=start,
                end=start + rng.choice([4 * 60, 8 * 60, 8 * 60]),
                location=rng.choice(["Location A", "Location B"]),
                required_skill=rng.choice(["", "Skill A", "Skill B"]),
            )
        )
    return ScheduleRequest(
        model_input=SchedulingInput(workers=workers, shifts=shifts, rules=rules)
    )


def _stacked_roster(problem: ScheduleRequest, seed: int) -> list[int | None]:
    """Few workers and some gaps, so rules about doubled or long runs of work bind."""
    rng = random.Random(seed)
    return [
        None if rng.random() < 0.1 else rng.choice([0, 0, 1, 2])
        for _ in problem.model_input.shifts
    ]


def _with_workers(problem: ScheduleRequest, fixed: list[int | None]) -> ScheduleRequest:
    candidate = problem.model_copy(deep=True)
    for shift, worker in zip(candidate.model_input.shifts, fixed, strict=True):
        shift.worker = (
            None if worker is None else candidate.model_input.workers[worker].id
        )
    return candidate


def _model_cost(total: dict, fixed: list[int | None]) -> float:
    """What the model's objective holds for an engine total: no root for sqrt(ssd)."""
    if total["name"] == "shift_count_imbalance_cost":
        loads = Counter(worker for worker in fixed if worker is not None)
        return sum(load**2 for load in loads.values()) / 2
    if total["name"] == "location_imbalance_cost" and total["label"].endswith(" ssd]"):
        return total["cost"] ** 2 / 2
    return total["cost"]


def _engine_cost(
    problem: ScheduleRequest, fixed: list[int | None]
) -> tuple[bool, float]:
    """The one rule's weighted cost under `fixed`, in the model's terms."""
    [total] = constraint_costs(_with_workers(problem, fixed))["totals"]
    return total["is_hard"], _model_cost(total, fixed)


@pytest.mark.parametrize("seed", range(4))
@pytest.mark.parametrize("rule", RULES, ids=lambda item: repr(item)[:60])
def test_each_rule_matches_the_engine_on_a_fixed_roster(rule, seed) -> None:
    problem = _problem([rule], seed)
    fixed = _stacked_roster(problem, seed)
    is_hard, expected = _engine_cost(problem, fixed)
    proxy: dict[str, int] = {}

    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)

    if is_hard:
        assert proxy == {"hard": -expected, "soft": 0}
    else:
        assert proxy["hard"] == 0
        # Fractional terms round up to the 1/SCALE grid, and there are two weeks.
        assert -proxy["soft"] / SCALE == pytest.approx(expected, abs=2 / SCALE + 1e-9)


def test_every_rule_type_has_a_plugin() -> None:
    union = typing.get_args(typing.get_args(models.Rule)[0])
    names = [rule.model_fields["name"].default for rule in union]

    assert list(PLUGINS) == names == list(es_scheduling.CONSTRAINT_NAMES)
    for name, plugin in PLUGINS.items():
        assert issubclass(plugin, models.rule_model(name).spec_class)
    assert {entry["name"] for entry in catalog()} == set(names)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """The catalog equals constraint_info() row for row: the eight defaults first,
    then the rule-only constraints (default_enabled False)."""
    engine = es_scheduling.constraint_info()
    entries = catalog()
    assert [entry["name"] for entry in entries] == [row.name for row in engine]
    for entry, row in zip(entries, engine, strict=True):
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        assert entry["kind"] == ("either" if row.level_flippable else row.default_level)
        assert entry["params"] == {"weight": None, "hard": None, "exponent": 1.0}
    assert [row.name for row in engine if row.default_enabled] == [
        spec.name for spec in domain.get_constraints()
    ]


def test_plugins_restate_no_engine_default() -> None:
    for plugin in PLUGINS.values():
        for attr in ("default_level", "default_weight", "default_hard", "rule_type"):
            assert not hasattr(plugin, attr), (plugin, attr)


def test_repeated_rules_get_numbered_plugins() -> None:
    registry = registry_for(
        [
            domain.DayPatternCost(pattern="Location A,OFF,Location B"),
            domain.RequiredSkillCost(),
            domain.DayPatternCost(pattern="OFF,WORK"),
        ]
    )
    assert [plugin.name for plugin in registry.plugins] == [
        "day_pattern_cost",
        "required_skill_cost",
        "day_pattern_cost#2",
    ]
    assert [spec.name for spec in registry.specs] == [
        plugin.name for plugin in registry.plugins
    ]
    assert registry.plugins[2].pattern == "OFF,WORK"
    assert registry.specs[2].level == "soft"  # the plain name's catalog row


def test_a_reward_refuses_hard_on_the_cpsat_path_too() -> None:
    with pytest.raises(ValueError, match="is a reward"):
        solve(_problem([domain.DesiredDayCost(hard=True)]))
    with pytest.raises(ValueError, match="is a reward"):
        solve(_problem([domain.DesiredShiftCost(hard=True, worker_indices=(0,))]))


def test_demoting_a_hard_rule_scores_like_the_engine() -> None:
    """hard=False lands on soft in both solvers (cpsat-common's level_of)."""
    problem = _problem([domain.MaxConsecutiveDaysCost(value=1, hard=False)], seed=1)
    fixed = _stacked_roster(problem, 1)
    expected = constraint_costs(_with_workers(problem, fixed))
    proxy: dict[str, int] = {}
    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)
    assert expected["hard"] == 0 and expected["soft"] < 0
    assert proxy == {"hard": 0, "soft": round(SCALE * expected["soft"])}


@pytest.mark.parametrize("seed", range(4))
def test_every_rule_at_once_matches_the_engine_on_a_fixed_roster(seed) -> None:
    problem = _problem(RULES, seed)
    fixed = _stacked_roster(problem, seed)
    expected = constraint_costs(_with_workers(problem, fixed))
    proxy: dict[str, int] = {}

    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)

    assert proxy["hard"] == expected["hard"]
    soft = sum(
        _model_cost(total, fixed)
        for total in expected["totals"]
        if not total["is_hard"]
    )
    # Two ssd rules and two weeks of gap round up to the grid.
    assert -proxy["soft"] / SCALE == pytest.approx(soft, abs=5 / SCALE + 1e-9)


def test_mixed_sign_costs_do_not_outweigh_the_hard_level() -> None:
    problem = ScheduleRequest(
        model_input=SchedulingInput(
            workers=[
                WorkerModel(id="Worker 1"),
                WorkerModel(id="Worker 2", skills=["Skill A"]),
            ],
            shifts=[
                ShiftModel(id="Shift 1", start=0, end=60, required_skill="Skill A")
            ],
            rules=[
                domain.RequiredSkillCost(),
                domain.AssignmentCost(
                    shift_costs={
                        "Worker 1": {"Shift 1": -5.0},
                        "Worker 2": {"Shift 1": 5.0},
                    }
                ),
            ],
        )
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.model_input.shifts[0].worker == "Worker 2"


def test_the_demo_solves_to_a_feasible_roster() -> None:
    request = generate_demo_data(workers=8, days=7)

    solved = solve(request, time_limit_seconds=10, num_workers=4)

    assert all(shift.worker for shift in solved.model_input.shifts)
    assert constraint_costs(solved)["hard"] == 0


def test_a_shift_stays_empty_only_when_a_rule_prices_it() -> None:
    worker = WorkerModel(id="Worker 1", unavailable_dates=[0])
    shift = ShiftModel(id="Shift 1", start=0, end=60)

    def problem(rules: list) -> ScheduleRequest:
        return ScheduleRequest(
            model_input=SchedulingInput(workers=[worker], shifts=[shift], rules=rules)
        )

    priced = solve(
        problem([domain.EmployeeUnavailabilityCost(), domain.UnassignedShiftsCost()]),
        time_limit_seconds=2,
    )
    free = solve(problem([domain.EmployeeUnavailabilityCost()]), time_limit_seconds=2)

    assert priced.model_input.shifts[0].worker is None
    assert free.model_input.shifts[0].worker == "Worker 1"


def test_imbalance_does_not_buy_an_empty_shift_at_a_soft_price() -> None:
    problem = ScheduleRequest(
        model_input=SchedulingInput(
            workers=[WorkerModel(id="Worker 1"), WorkerModel(id="Worker 2")],
            shifts=[
                ShiftModel(id=f"Shift {day}", start=day * DAY, end=day * DAY + 60)
                for day in range(3)
            ],
            rules=[
                domain.ShiftCountImbalanceCost(),
                domain.UnassignedShiftsCost(hard=False),
            ],
        )
    )

    solved = solve(problem, time_limit_seconds=2)

    assert all(shift.worker for shift in solved.model_input.shifts)
    assert constraint_costs(solved)["soft"] == pytest.approx(-(0.5**0.5), abs=1e-5)


def test_what_cannot_be_modelled_is_refused() -> None:
    with pytest.raises(ValueError, match="exponent"):
        solve(_problem([domain.RequiredSkillCost(exponent=2.0)]))
    # Refused even though no term of this rule would reach the objective.
    with pytest.raises(ValueError, match="integer"):
        solve(_problem([domain.TargetDaysOffCost(weight=0.5)]))
    with pytest.raises(ValueError, match="fractional"):
        solve(_problem([domain.ShiftCountImbalanceCost(hard=True)]))
    with pytest.raises(ValueError, match="fractional"):
        solve(_problem([domain.AssignmentCost(hard=True, cost_per_shift=2.0)]))
    pinned = _problem([domain.RequiredSkillCost()])
    pinned.model_input.shifts[0].worker = "Worker 0"
    pinned.model_input.shifts[0].pinned = True
    with pytest.raises(ValueError, match="pinned"):
        solve(pinned)
