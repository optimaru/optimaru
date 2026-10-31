import random
import typing

import pytest
from es_scheduling import models as domain
from es_scheduling.demo import generate_demo_data
from es_scheduling.models import (
    ScheduleRequest,
    SchedulingInput,
    ShiftModel,
    WorkerModel,
)

from employee_scheduling_cpsat import catalog, constraint_costs, solve
from employee_scheduling_cpsat.constraints import PLUGINS, SCALE

DAY = 24 * 60

# Every rule type, at values that make it bind on `_problem`. Several rule types
# appear more than once where a parameter switches to another code path.
RULES = [
    domain.RequiredSkillRule(),
    domain.RequiredSkillRule(weight=3, policy="soft"),
    domain.UnavailableWorkerRule(),
    domain.OverlappingShiftRule(),
    domain.MaxOneShiftPerDayRule(),
    domain.MinimumRestRule(),
    domain.MinimumRestRule(min_gap_minutes=120),
    domain.UndesiredDayRule(),
    domain.UndesiredDayRule(per_day=True),
    domain.DesiredDayRule(),
    domain.DesiredDayRule(per_day=True),
    domain.DesiredShiftRule(),
    domain.BalanceAssignmentsRule(),
    domain.MaxConsecutiveDaysRule(value=2),
    domain.MinConsecutiveDaysRule(value=3),
    domain.MaxConsecutiveOffRule(value=1),
    domain.RestAfterConsecutiveWorkRule(work_days=2, rest_days=3),
    domain.MaxWeeklyMinutesRule(minutes=600),
    domain.MaxWeeklyMinutesRule(minutes=600, week_anchor=3),
    domain.MinWeeklyMinutesRule(minutes=900),
    domain.MinWeeklyRestDaysRule(value=5),
    domain.AssignmentCostRule(
        cost_per_hour=1.5,
        cost_per_shift=2.0,
        shift_costs={"Worker 1": {"Shift 1": 9.25}},
    ),
    domain.AssignmentCostRule(cost_per_shift=2.0),
    domain.UnassignedShiftRule(),
    domain.BalanceMinMaxGapRule(tolerance=1),
    domain.BalanceWeeklyMinutesGapRule(tolerance_minutes=100),
    domain.ShiftCoverageRule(worker_skill="Skill A", min_count=1, max_count=1),
    domain.ShiftCoverageRule(min_count=2, worker_indices=[0, 1], shift_indices=[0, 2, 3]),
    domain.LocationRunRule(location="Location A", min_run=2, max_run=3),
    domain.LocationRunRule(min_run=3, max_run=0, waive_min_at_end=False),
    domain.RestAfterLocationRunRule(location="Location A", rest_days=2),
    domain.LocationRunGapRule(location="Location A", min_gap=3),
    domain.WorkerLocationCountRule(location="Location A", min_count=1, max_count=2),
    domain.WorkerLocationCountRule(worker_skill="Skill B", min_count=4),
    domain.DayPatternRule(pattern="Location A,OFF,Location B"),
    domain.DayPatternRule(pattern="!Location B,*,WORK"),
    domain.DayPatternRule(pattern="OFF,Location Z"),
    domain.TargetDaysOffRule(target_days=[0, 5, 6, 99], min_count=2),
    domain.WorkerDayCountInRuleRange(lo=1, hi=5, min_count=2, max_count=3),
    domain.WorkerDayCountInRuleRange(location="Location B", lo=0, hi=9, max_count=1),
    domain.BalanceLocationRule(location="Location A"),
    domain.BalanceLocationRule(worker_skill="Skill A", metric="gap", tolerance=1),
    domain.BalanceLocationRule(off_target_days=[0, 1, 2, 9]),
    domain.WorkerPairSameShiftRule(
        anchor=0, paired=1, bucket_days=[0, 1, 2], bucket_locations=["Location A"] * 3
    ),
    domain.WorkerPairSameShiftRule(
        anchor=0,
        paired=1,
        bucket_days=[1, 2],
        bucket_locations=["Location B", "Location A"],
        penalty=True,
    ),
    domain.MaxConsecutiveDaysRule(value=1, worker_indices=[0, 2]),
    domain.MinimumRestRule(shift_indices=list(range(0, 24, 2))),
    domain.DesiredDayRule(worker_indices=[]),
    domain.DayPatternRule(pattern="Location A,WORK", worker_indices=[1], shift_indices=list(range(12))),
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
        shift.worker = None if worker is None else candidate.model_input.workers[worker].id
    return candidate


def _engine_cost(problem: ScheduleRequest, fixed: list[int | None]) -> tuple[bool, float]:
    """The one rule's weighted cost under `fixed`, from the engine's calculator."""
    [total] = constraint_costs(_with_workers(problem, fixed))["totals"]
    return total["is_hard"], total["cost"]


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
        # Fractional terms round up to the 1/SCALE grid, one per week at most.
        assert -proxy["soft"] / SCALE == pytest.approx(expected, abs=2 / SCALE + 1e-9)


def test_every_rule_type_has_a_plugin() -> None:
    union = typing.get_args(typing.get_args(domain.Rule)[0])
    names = {rule.model_fields["name"].default for rule in union}

    assert set(PLUGINS) == names
    assert {entry["name"] for entry in catalog()} == names


@pytest.mark.parametrize("seed", range(4))
def test_every_rule_at_once_matches_the_engine_on_a_fixed_roster(seed) -> None:
    problem = _problem(RULES, seed)
    fixed = _stacked_roster(problem, seed)
    expected = constraint_costs(_with_workers(problem, fixed))
    proxy: dict[str, int] = {}

    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)

    assert proxy["hard"] == expected["hard"]
    # Each of the three root rules and two weeks of gap rounds up to the grid.
    assert proxy["soft"] / SCALE == pytest.approx(expected["soft"], abs=5 / SCALE + 1e-9)


def test_mixed_sign_costs_do_not_outweigh_the_hard_level() -> None:
    problem = ScheduleRequest(
        model_input=SchedulingInput(
            workers=[
                WorkerModel(id="Worker 1"),
                WorkerModel(id="Worker 2", skills=["Skill A"]),
            ],
            shifts=[ShiftModel(id="Shift 1", start=0, end=60, required_skill="Skill A")],
            rules=[
                domain.RequiredSkillRule(),
                domain.AssignmentCostRule(
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
        problem([domain.UnavailableWorkerRule(), domain.UnassignedShiftRule()]),
        time_limit_seconds=2,
    )
    free = solve(problem([domain.UnavailableWorkerRule()]), time_limit_seconds=2)

    assert priced.model_input.shifts[0].worker is None
    assert free.model_input.shifts[0].worker == "Worker 1"


def test_what_cannot_be_modelled_is_refused() -> None:
    with pytest.raises(ValueError, match="exponent"):
        solve(_problem([domain.RequiredSkillRule(exponent=2.0)]))
    # Refused even though no term of this rule would reach the objective.
    with pytest.raises(ValueError, match="integer"):
        solve(_problem([domain.TargetDaysOffRule(weight=0.5)]))
    with pytest.raises(ValueError, match="fractional"):
        solve(_problem([domain.BalanceAssignmentsRule(policy="hard")]))
    with pytest.raises(ValueError, match="fractional"):
        solve(_problem([domain.AssignmentCostRule(policy="hard", cost_per_shift=2.5)]))
    pinned = _problem([domain.RequiredSkillRule()])
    pinned.model_input.shifts[0].worker = "Worker 0"
    pinned.model_input.shifts[0].pinned = True
    with pytest.raises(ValueError, match="pinned"):
        solve(pinned)
