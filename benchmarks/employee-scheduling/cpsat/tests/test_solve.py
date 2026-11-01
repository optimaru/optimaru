import random
import typing
from collections import Counter
from itertools import product

import pytest
import es_scheduling
from ortools.sat.python import cp_model
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
from employee_scheduling_cpsat.solve import selection

DAY = 24 * 60

# Every rule type, at values that make it bind on `_problem`. Several rule types
# appear more than once where a parameter switches to another code path.
RULES = [
    domain.RequiredSkillCost(),
    # A second instance: a scoped, demoted copy.
    domain.RequiredSkillCost(weight=3, level="soft", worker_indices=(0, 1)),
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
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
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
        solve(_problem([domain.DesiredDayCost(level="hard")]))
    with pytest.raises(ValueError, match="is a reward"):
        solve(_problem([domain.DesiredShiftCost(level="hard", worker_indices=(0,))]))


def test_demoting_a_hard_rule_scores_like_the_engine() -> None:
    """level="soft" lands on soft in both solvers (cpsat-common's level_of)."""
    problem = _problem([domain.MaxConsecutiveDaysCost(value=1, level="soft")], seed=1)
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
                domain.UnassignedShiftsCost(level="soft"),
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
        solve(_problem([domain.ShiftCountImbalanceCost(level="hard")]))
    with pytest.raises(ValueError, match="fractional"):
        solve(_problem([domain.AssignmentCost(level="hard", cost_per_shift=2.0)]))
    pinned = _problem([domain.RequiredSkillCost()])
    pinned.model_input.shifts[0].worker = "Worker 0"
    pinned.model_input.shifts[0].pinned = True
    with pytest.raises(ValueError, match="pinned"):
        solve(pinned)


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------

# The plugins that add one term per engine unit (an employee, or the total of
# unassigned_shifts_cost), so they take offsets; at values that bind on `_problem`.
# The literal formula with an offset is refused on hard, so the literal picks of
# the hard rules sit on soft.
SHIFTED = [
    domain.RequiredSkillCost(level="soft", offset=1, clip=False),
    domain.RequiredSkillCost(offset=1),
    domain.RequiredSkillCost(weight=2, offset=-1),
    domain.EmployeeUnavailabilityCost(offset=60),
    domain.UndesiredDayCost(offset=100, clip=False),
    domain.UndesiredDayCost(offset=-30, per_day=True),
    domain.UndesiredDayCost(offset=30, worker_indices=(0, 2, 2, 9), clip=False),
    domain.DesiredDayCost(offset=200),
    domain.DesiredDayCost(offset=100, clip=False, shift_indices=tuple(range(0, 24, 3))),
    domain.UnassignedShiftsCost(offset=2),
    domain.UnassignedShiftsCost(level="soft", offset=2, clip=False),
    domain.UnassignedShiftsCost(weight=3, offset=1, level="soft"),
]


def _engine_scaled(problem: ScheduleRequest, fixed: list[int | None]) -> dict[str, int]:
    """The engine's score under `fixed`, soft on the model's SCALE grid (integral here)."""
    expected = constraint_costs(_with_workers(problem, fixed))
    soft = SCALE * expected["soft"]
    assert soft == round(soft)
    return {"hard": expected["hard"], "soft": round(soft)}


@pytest.mark.parametrize("seed", range(4))
@pytest.mark.parametrize("rule", SHIFTED, ids=lambda item: repr(item)[:80])
def test_a_shifted_rule_matches_the_engine_on_a_fixed_roster(rule, seed) -> None:
    """offset / clip shape each employee's term (or the total) exactly as the engine does."""
    problem = _problem([rule], seed)
    fixed = _stacked_roster(problem, seed)
    proxy: dict[str, int] = {}
    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)
    assert proxy == _engine_scaled(problem, fixed)


@pytest.mark.parametrize("seed", range(4))
def test_repeated_and_shifted_rules_at_once_match_the_engine(seed) -> None:
    rules = [
        domain.RequiredSkillCost(),
        domain.RequiredSkillCost(),  # identical: counts twice
        *SHIFTED,
        domain.UndesiredDayCost(),
        domain.UndesiredDayCost(weight=2),  # the same rule again, other knobs
        domain.MaxConsecutiveDaysCost(value=2),
        domain.MaxConsecutiveDaysCost(value=2),
    ]
    problem = _problem(rules, seed)
    fixed = _stacked_roster(problem, seed)
    proxy: dict[str, int] = {}
    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)
    assert proxy == _engine_scaled(problem, fixed)
    assert [plugin.name for plugin in registry_for(rules).plugins][:2] == [
        "required_skill_cost",
        "required_skill_cost#2",
    ]


# Each repeat on its own level, with offsets on the plugins that take them.
LEVELS = [
    domain.UndesiredDayCost(),
    domain.UndesiredDayCost(level="hard", offset=100),
    domain.EmployeeUnavailabilityCost(),
    domain.EmployeeUnavailabilityCost(level="soft", weight=2, offset=30, clip=False),
    domain.RequiredSkillCost(),
    domain.RequiredSkillCost(level=domain.Level.SOFT, offset=-1),
]


@pytest.mark.parametrize("seed", range(4))
def test_each_repeat_keeps_its_own_level_on_every_path(seed) -> None:
    """CP-SAT == the engine == the Python score() mirror, each instance on its own level."""
    problem = _problem(LEVELS, seed)
    fixed = _stacked_roster(problem, seed)
    candidate = _with_workers(problem, fixed)
    specs = [rule.to_spec() for rule in candidate.model_input.rules]
    assert [str(spec.level) if spec.level else None for spec in specs] == [
        None,
        "hard",
        None,
        "soft",
        None,
        "soft",
    ]
    schedule = models.to_schedule(candidate.model_input)
    raw = es_scheduling.constraint_costs(schedule, specs)
    assert [raw[key]["level"] for key in domain.instance_keys(specs)] == [
        "soft",
        "hard",
        "hard",
        "soft",
        "hard",
        "soft",
    ]
    mirror = domain.score(raw, specs)
    engine = constraint_costs(candidate)
    assert (engine["hard"], engine["soft"]) == (mirror.hard, pytest.approx(mirror.soft))
    proxy: dict[str, int] = {}
    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)
    assert proxy == _engine_scaled(problem, fixed)
    assert proxy == {"hard": mirror.hard, "soft": round(SCALE * mirror.soft)}


@pytest.mark.parametrize("level", [domain.Level.MEDIUM, "medium"])
def test_medium_is_refused_on_cpsat(level) -> None:
    """No medium level here: the rule schema refuses it, and so does the registry itself
    (when the objective is built, before any term is added)."""
    with pytest.raises(ValueError, match="'hard' or 'soft'"):
        _problem([domain.RequiredSkillCost(), domain.RequiredSkillCost(level=level)])
    picks = [domain.UndesiredDayCost(), domain.UndesiredDayCost(level=level)]
    registry = registry_for(picks)
    with pytest.raises(ValueError, match="this use case has no medium level"):
        registry.objective(
            cp_model.CpModel(), ("hard", "soft"), selection(registry.plugins)
        )


def _tiny(rules: list) -> ScheduleRequest:
    """Two workers and three shifts (one on each worker's undesired day): 3^3 rosters."""
    workers = [
        WorkerModel(
            id="Worker 0", skills=["Skill A"], undesired_dates=[0], desired_dates=[2]
        ),
        WorkerModel(id="Worker 1", skills=["Skill B"], undesired_dates=[1]),
    ]
    shifts = [
        ShiftModel(
            id=f"Shift {day}",
            start=day * DAY + 8 * 60,
            end=day * DAY + 16 * 60,
            required_skill=skill,
        )
        for day, skill in ((0, "Skill A"), (1, "Skill A"), (2, "Skill B"))
    ]
    return ScheduleRequest(
        model_input=SchedulingInput(workers=workers, shifts=shifts, rules=rules)
    )


TINY = {
    "repeat": [
        domain.RequiredSkillCost(),
        domain.RequiredSkillCost(),
        domain.UndesiredDayCost(),
    ],
    "shifted": [
        domain.RequiredSkillCost(offset=1),
        domain.RequiredSkillCost(level="soft", offset=1, clip=False),
        domain.UndesiredDayCost(offset=400),
        domain.UndesiredDayCost(weight=2, offset=-60, clip=False),
        domain.DesiredDayCost(offset=100, clip=False),
        domain.UnassignedShiftsCost(weight=1000, offset=1, level="soft"),
    ],
}


@pytest.mark.parametrize("rules", list(TINY.values()), ids=list(TINY))
def test_repeated_and_shifted_rules_reach_the_brute_force_optimum(rules) -> None:
    problem = _tiny(rules)
    workers = range(len(problem.model_input.workers))
    may_stay_empty = any(rule.name == "unassigned_shifts_cost" for rule in rules)
    choices = [*workers, None] if may_stay_empty else list(workers)
    scores = (
        constraint_costs(_with_workers(problem, list(fixed)))
        for fixed in product(choices, repeat=len(problem.model_input.shifts))
    )
    best = max((costs["hard"], costs["soft"]) for costs in scores)
    solved = solve(problem, time_limit_seconds=5)
    found = constraint_costs(solved)
    assert (found["hard"], found["soft"]) == pytest.approx(best)


def test_json_rules_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} rules pick the same instances as specs."""
    specs = [
        domain.UndesiredDayCost(),
        domain.UndesiredDayCost(weight=3, offset=40, clip=False),
    ]
    dicts = [
        {"name": "undesired_day_cost"},
        {"name": "undesired_day_cost", "weight": 3, "offset": 40, "clip": False},
    ]
    by_spec = solve(_tiny(specs), time_limit_seconds=5)
    by_dict = solve(_tiny(dicts), time_limit_seconds=5)
    assert [shift.worker for shift in by_dict.model_input.shifts] == [
        shift.worker for shift in by_spec.model_input.shifts
    ]
    assert constraint_costs(by_dict)["soft"] == constraint_costs(by_spec)["soft"]


def test_an_offset_cpsat_cannot_shape_is_refused() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(_problem([domain.UndesiredDayCost(offset=0.5)]), time_limit_seconds=1)
    # A rule whose terms are runs, not employees, takes no offset on this model.
    with pytest.raises(
        ValueError, match="offset is not supported by this CP-SAT model"
    ):
        solve(
            _problem([domain.MaxConsecutiveDaysCost(value=2, offset=1)]),
            time_limit_seconds=1,
        )
    assert {name for name, plugin in PLUGINS.items() if plugin.terms_per_unit} == {
        "required_skill_cost",
        "employee_unavailability_cost",
        "undesired_day_cost",
        "desired_day_cost",
        "unassigned_shifts_cost",
    }


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)

LITERAL_ON_HARD = [
    lambda offset: domain.RequiredSkillCost(
        offset=offset, clip=False
    ),  # the default level, hard
    lambda offset: domain.UndesiredDayCost(level="hard", offset=offset, clip=False),
    lambda offset: domain.UnassignedShiftsCost(offset=offset, clip=False),  # global
    lambda offset: domain.MaxConsecutiveDaysCost(
        value=2, offset=offset, clip=False
    ),  # no per-unit terms
]
LITERAL_IDS = [
    "skill-default-hard",
    "undesired-moved-to-hard",
    "unassigned-global",
    "max-days-runs",
]


@pytest.mark.parametrize("offset", [1, -1])
@pytest.mark.parametrize("pick", LITERAL_ON_HARD, ids=LITERAL_IDS)
def test_the_literal_offset_is_refused_on_hard_like_the_engine(pick, offset) -> None:
    """The rule schema refuses the pick in the engine's words; so does the CP-SAT
    objective itself (before it asks whether the plugin takes offsets at all), and so
    do the engine and the score() mirror."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    for rules in ([spec], [domain.RequiredSkillCost(), spec]):
        with pytest.raises(ValueError) as refused:
            _problem(rules)
        (error,) = refused.value.errors()
        assert str(error["ctx"]["error"]) == message
    registry = registry_for([spec])
    with pytest.raises(ValueError) as refused:
        registry.objective(
            cp_model.CpModel(), ("hard", "soft"), selection(registry.plugins)
        )
    assert str(refused.value) == message
    with pytest.raises(ValueError) as refused:
        es_scheduling.DecomposedCalculator([spec])
    assert str(refused.value) == message
    allowed = type(spec)(**spec.params())
    roster = models.to_schedule(_problem([allowed]).model_input)
    with pytest.raises(ValueError) as refused:
        domain.score(es_scheduling.constraint_costs(roster, [allowed]), [spec])
    assert str(refused.value) == message


@pytest.mark.parametrize("seed", range(2))
@pytest.mark.parametrize("offset", [1, -1])
def test_the_same_literal_pick_on_soft_matches_the_engine(offset, seed) -> None:
    rules = [
        domain.RequiredSkillCost(),
        domain.RequiredSkillCost(level="soft", offset=offset, clip=False),
        domain.UnassignedShiftsCost(level="soft", offset=offset, clip=False),
    ]
    problem = _problem(rules, seed)
    fixed = _stacked_roster(problem, seed)
    proxy: dict[str, int] = {}
    solve(problem, time_limit_seconds=5, _fixed_workers=fixed, _proxy_score=proxy)
    assert proxy == _engine_scaled(problem, fixed)
