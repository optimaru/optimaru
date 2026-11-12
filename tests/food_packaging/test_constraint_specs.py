"""optimaru.food_packaging.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py` and the C++
tests (one hand-calculated positive and zero case per constraint). This file covers the
selection path that picks "which constraints are on", the catalog (`constraint_info()`),
the five knobs, repeated picks and the medium level.
"""

from types import SimpleNamespace

import pytest

import optimaru.food_packaging as fp
from optimaru._constraint import instance_keys
from optimaru.food_packaging import constraints as specs
from optimaru.food_packaging.constraints import (
    AfterIdealEndTimeCost,
    Level,
    MaxEndTimeCost,
    OperatorCleaningConflictCost,
    SquaredMakespanCost,
    UnassignedJobsCost,
    after_ideal_end_time_cost,
    get_constraints,
    max_end_time_cost,
    operator_cleaning_conflict_cost,
    score,
    squared_makespan_cost,
    unassigned_jobs_cost,
)

from .._picks import replace_default

MEDIUM_UNIT = 10_000_000_000  # the domain's medium fold (into soft): 10^10, never 10^9
BIG = 1_000_000_000
PA_S, PB = 0, 2  # products of the hand schedules (A small, B)


def build_schedule():
    return fp.generate_demo_data(line_count=2, job_count=10)


def solve(constraints=None):
    schedule = build_schedule()
    fp.solve(schedule, time_limit_seconds=0.5, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints (cleaning conflict + unassigned) after.
    available = fp.list_constraints()
    assert available == [
        "max_end_time_cost",
        "after_ideal_end_time_cost",
        "squared_makespan_cost",
        "operator_cleaning_conflict_cost",
        "unassigned_jobs_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the makespan term disappears entirely."""
    baseline = solve()
    assert baseline.soft < 0

    without_makespan = solve([max_end_time_cost, after_ideal_end_time_cost, unassigned_jobs_cost])
    assert without_makespan.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the makespan weight grows soft only; hard stays put."""
    picks = [max_end_time_cost, unassigned_jobs_cost]
    base = solve([*picks, squared_makespan_cost])
    heavier = solve([*picks, SquaredMakespanCost(weight=1000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Raising the level flips the solver's trade-off.

    With soft makespan, jobs go onto lines and the cost is paid in soft.
    Promoted to hard, running a line itself becomes a hard cost while unassigned stays
    medium — levels are lexicographic — so the solver leaves the lines empty."""
    picks = [unassigned_jobs_cost]

    soft_level = solve([*picks, squared_makespan_cost])
    assert soft_level.soft < 0  # loads the jobs and pays the makespan

    hard_level = solve([*picks, SquaredMakespanCost(level="hard")])
    assert hard_level.medium < 0  # leaves the jobs off instead
    assert hard_level.soft == 0
    assert hard_level.hard == 0  # no line runs, so no hard cost either


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert operator_cleaning_conflict_cost.weight is None
    assert operator_cleaning_conflict_cost.level is None
    assert operator_cleaning_conflict_cost.exponent == 1.0
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0), spec.name
        assert getattr(specs, spec.name) is spec  # the instance is named after the constraint


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    fp.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return fp.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == fp.calculate_score(solution).hard


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out.

    Hard rounding happens per unit contribution, so splitting by constraint and
    summing gives the same value. "How much does this term add to the score"
    can be read without rescoring."""
    raw, _ = _raw()
    specs = get_constraints()
    full = score(raw, specs)
    parts = [score(raw, [spec]) for spec in specs]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """Changing only the weight scales that term linearly — no rescoring."""
    raw, _ = _raw()
    base = score(raw, [squared_makespan_cost])
    ten = score(raw, [SquaredMakespanCost(weight=10)])
    default_weight = raw[squared_makespan_cost.name]["weight"]
    factor = 10 / default_weight
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


# --- The catalog: constraint_info() -------------------------------------------

# (name, default level, default weight), in list_constraints() order. The medium
# weights are in medium units (1, not the 10^10 fold).
EXPECTED_INFO = [
    ("max_end_time_cost", "hard", 1.0),
    ("after_ideal_end_time_cost", "medium", 1.0),
    ("squared_makespan_cost", "soft", 1.0),
    ("operator_cleaning_conflict_cost", "hard", 1.0),
    ("unassigned_jobs_cost", "medium", 1.0),
]
GLOBALS = {"operator_cleaning_conflict_cost", "unassigned_jobs_cost"}


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = fp.constraint_info()
    assert [row.name for row in rows] == fp.list_constraints()
    assert [(row.name, row.default_level, row.default_weight) for row in rows] == EXPECTED_INFO
    # Every constraint is flippable, none is a reward, and all five are on by default.
    assert all(row.level_flippable and not row.reward and row.default_enabled for row in rows)


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults and carries the domain's medium fold on every entry."""
    raw = fp.constraint_costs(build_schedule())
    assert list(raw) == fp.list_constraints()
    for row in fp.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"]) == (
            row.default_weight,
            row.default_level,
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name in GLOBALS)
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}


def test_constraint_costs_is_the_only_per_constraint_surface():
    """No second spelling of the per-constraint costs is bound."""
    assert not hasattr(fp, "analyze") and not hasattr(fp._core, "analyze")
    assert not hasattr(fp, "ConstraintBreakdown") and not hasattr(fp._core, "ConstraintBreakdown")


# --- The knobs and the medium level ----------------------------------------------


def _engine(score_):
    """The engine-level (hard, soft) of a hard/medium/soft score (medium folded into soft)."""
    return score_.hard, score_.medium * MEDIUM_UNIT + score_.soft


def _schedule(lines, loose_jobs=()):
    """One operator per line, the products of test_constraints.py (B -> A small cleans 60).

    lines: [[(product, duration, ideal_end), ...], ...]; loose_jobs are in no list."""
    products = [fp.Product("1", "Product A small"), fp.Product("2", "Product A large"), fp.Product("3", "Product B")]
    cleanup = [0, 0, 40, 5, 0, 40, 60, 60, 0]
    jobs = []

    def add_job(product, duration, ideal=BIG):
        jobs.append(fp.Job(str(len(jobs)), f"job{len(jobs)}", product, duration, 0, ideal, BIG))
        return len(jobs) - 1

    core_lines = [
        fp.Line(str(i), f"Line {i}", 0, i, [add_job(*spec) for spec in specs_]) for i, specs_ in enumerate(lines)
    ]
    for spec in loose_jobs:
        add_job(*spec)
    operators = [fp.Operator(f"Operator {i + 1}") for i in range(max(1, len(lines)))]
    return fp.PackagingSchedule(0, 14 * 24 * 60, products, cleanup, operators, core_lines, jobs)


def _late_and_unassigned():
    """Line 0 runs J0 0..200 (ideal 200) and J1 200..350 (ideal 250): 100 minutes past
    the ideal end; one loose 50-minute job: 50 unassigned minutes; makespan 350^2."""
    return _schedule([[(PA_S, 200, 200), (PA_S, 150, 250)]], loose_jobs=[(PB, 50)])


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit C++ default weights."""
    explicit = [type(spec)(weight=weight) for spec, (_, _, weight) in zip(get_constraints(), EXPECTED_INFO)]
    schedule = _late_and_unassigned()
    raw = fp.constraint_costs(schedule)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == _engine(fp.calculate_score(schedule))


def test_the_medium_level_folds_into_soft():
    schedule = _late_and_unassigned()
    raw = fp.constraint_costs(schedule)
    assert raw["after_ideal_end_time_cost"]["costs"] == [100.0, 0.0]  # the line, then the pool
    assert raw["unassigned_jobs_cost"]["costs"] == [50.0]  # global: one total
    monolithic = fp.calculate_score(schedule)
    assert (monolithic.hard, monolithic.medium, monolithic.soft) == (0, -150, -122500)
    soft = monolithic.soft
    assert score(raw) == (0, soft - 150 * MEDIUM_UNIT)
    others = [spec for spec in get_constraints() if spec.name != "after_ideal_end_time_cost"]
    assert score(raw, [*others, AfterIdealEndTimeCost(level="hard")]) == (-100, soft - 50 * MEDIUM_UNIT)
    assert score(raw, [*others, AfterIdealEndTimeCost(level="soft")]) == (0, soft - 100 - 50 * MEDIUM_UNIT)
    assert score(raw, [*others, AfterIdealEndTimeCost(weight=3)]) == (0, soft - 350 * MEDIUM_UNIT)
    others = [spec for spec in get_constraints() if spec.name != "unassigned_jobs_cost"]
    assert score(raw, [*others, UnassignedJobsCost(level="hard")]) == (-50, soft - 100 * MEDIUM_UNIT)
    assert score(raw, [*others, UnassignedJobsCost(level="soft")]) == (0, soft - 50 - 100 * MEDIUM_UNIT)
    assert score(raw, [*others, UnassignedJobsCost(weight=2)]) == (0, soft - 200 * MEDIUM_UNIT)


def _one_job_one_line():
    """One empty line and one 100-minute job in no list: on the line it costs 100^2 =
    10000 soft (squared_makespan_cost); left out it costs its 100 unassigned minutes."""
    return _schedule([[]], loose_jobs=[(PA_S, 100)])


def test_a_demoted_medium_constraint_is_soft_in_the_engine_too():
    """level="soft" moves unassigned_jobs_cost to soft in the solver.

    As soft, 100 unassigned minutes are cheaper than a 10000 squared makespan, so the solver
    leaves the job out, and its minutes cost soft, not medium."""
    picks = [UnassignedJobsCost(level="soft"), squared_makespan_cost]
    schedule = _one_job_one_line()
    fp.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, 0, -100)
    assert score(fp.constraint_costs(schedule), picks) == (schedule.score.hard, schedule.score.soft)


def test_the_default_medium_level_keeps_the_job():
    """At the default (medium) level the unassigned minutes dominate any soft makespan."""
    picks = [unassigned_jobs_cost, squared_makespan_cost]
    schedule = _one_job_one_line()
    fp.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, 0, -10000)
    assert score(fp.constraint_costs(schedule), picks) == _engine(schedule.score)


def test_a_promoted_medium_constraint_is_one_hard_unit_per_minute():
    """level="hard": an unassigned minute costs 1 hard (not 10^10). With the makespan also hard,
    leaving the job out (100 hard) is cheaper than running it (10000 hard)."""
    picks = [UnassignedJobsCost(level="hard"), SquaredMakespanCost(level="hard")]
    schedule = _one_job_one_line()
    fp.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (-100, 0, 0)
    assert score(fp.constraint_costs(schedule), picks) == (schedule.score.hard, schedule.score.soft)


def test_on_new_best_reports_the_levels_of_the_final_score():
    """on_new_best on the plugin path reports the same three levels as schedule.score.

    The last new best is the returned solution, so with a soft total of 10^10 or more its
    report still equals schedule.score instead of a decoded fold (which would move
    soft // 10^10 into medium)."""
    picks = [
        max_end_time_cost,
        operator_cleaning_conflict_cost,
        after_ideal_end_time_cost,
        unassigned_jobs_cost,
        SquaredMakespanCost(weight=10_000),
    ]
    reports = []
    schedule = build_schedule()
    cfg = fp.SolverConfig()
    cfg.spent_limit_seconds = 10.0
    cfg.step_count_limit = 2000
    cfg.random_seed = 7
    cfg.on_new_best = lambda _step, best: reports.append((best.hard, best.medium, best.soft))
    fp.Solver(cfg).solve_with_constraints(schedule, picks)
    final = (schedule.score.hard, schedule.score.medium, schedule.score.soft)
    assert final[2] <= -MEDIUM_UNIT
    assert reports and reports[-1] == final


# --- repeated picks and the offset / clip knobs ------------------------------
#
# The hand instance is _hand_schedule(): line 0 runs J0 0..200 (ideal 200) and J1
# 200..350 (ideal 250), line 1 is empty, a 50-minute job waits in the unassigned pool
# list (where a zero-budget solve leaves it). Raw per unit (line 0, line 1, the pool): after_ideal_end_time_cost [100, 0, 0],
# squared_makespan_cost [350^2, 0, 0], max_end_time_cost [0, 0, 0]; the globals
# unassigned_jobs_cost [50] and operator_cleaning_conflict_cost [0]. The per-line
# constraints evaluate the two lines (an empty one included), never the pool.


def _hand_schedule():
    schedule = _schedule([[(PA_S, 200, 200), (PA_S, 150, 250)], []], loose_jobs=[(PB, 50)])
    schedule.unassigned_jobs = [2]
    return schedule


def _layout(schedule):
    return [list(line.jobs) for line in schedule.lines], list(schedule.unassigned_jobs)


def _zero_budget(schedule, picks):
    """The engine's score of `schedule` under `picks`: a solve with no time keeps it as it is."""
    before = _layout(schedule)
    config = fp.SolverConfig()
    config.spent_limit_seconds = 0
    fp.Solver(config).solve_with_constraints(schedule, picks)
    assert _layout(schedule) == before
    return _engine(schedule.score)


REPEAT = [SquaredMakespanCost(), SquaredMakespanCost(weight=0.5, exponent=0.5)]


def test_naming_a_constraint_twice_scores_both_picks():
    """A constraint may be picked several times: each pick is scored on its own.

    Neither pick wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [UnassignedJobsCost(), SquaredMakespanCost(weight=10), SquaredMakespanCost()]
    assert _zero_budget(_hand_schedule(), picks) == (0, -50 * MEDIUM_UNIT - 11 * 350**2)
    assert get_constraints([MaxEndTimeCost(), MaxEndTimeCost()]) == [MaxEndTimeCost(), MaxEndTimeCost()]
    schedule = build_schedule()
    fp.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=[*get_constraints(), *REPEAT])
    assert score(fp.constraint_costs(schedule), [*get_constraints(), *REPEAT]) == pytest.approx(_engine(schedule.score))


def test_a_repeated_pick_by_hand():
    """The squared makespan plus half its square root per line: 350^2 + 0.5 x 350 = 122675."""
    raw = fp.constraint_costs(_hand_schedule(), REPEAT)
    assert list(raw) == ["squared_makespan_cost", "squared_makespan_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [122500.0, 0.0, 0.0]
    assert first["units"] == second["units"] == [0, 1]  # the lines, not the pool
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 0.5)
    assert first["name"] == second["name"] == "squared_makespan_cost"
    assert second["default_weight"] == 1.0
    assert second["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}
    expected = (0, -(350**2 + 0.5 * 350))
    assert expected == (0, -122675.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(fp.constraint_costs(_hand_schedule()), REPEAT) == expected
    assert _zero_budget(_hand_schedule(), REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [AfterIdealEndTimeCost(), AfterIdealEndTimeCost()]
    assert score(fp.constraint_costs(_hand_schedule()), picks) == (0, -200 * MEDIUM_UNIT)
    assert _zero_budget(_hand_schedule(), picks) == (0, -200 * MEDIUM_UNIT)
    assert list(fp.constraint_costs(_hand_schedule(), picks)) == [
        "after_ideal_end_time_cost",
        "after_ideal_end_time_cost#2",
    ]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        SquaredMakespanCost(weight=1),
        UnassignedJobsCost(),
        SquaredMakespanCost(weight=2),
        MaxEndTimeCost(),
        SquaredMakespanCost(weight=3),
    ]
    raw = fp.constraint_costs(_hand_schedule(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "max_end_time_cost": 1.0,
        "squared_makespan_cost": 1.0,
        "squared_makespan_cost#2": 2.0,
        "squared_makespan_cost#3": 3.0,
        "unassigned_jobs_cost": 1.0,
    }
    assert [key for key in raw if key.startswith("squared")] == [
        "squared_makespan_cost",
        "squared_makespan_cost#2",
        "squared_makespan_cost#3",
    ]
    expected = (0, -50 * MEDIUM_UNIT - 6 * 350**2)
    assert score(raw, picks) == expected
    assert _zero_budget(_hand_schedule(), picks) == expected


def test_a_repeated_medium_pick_adds_up():
    """Both instances stay on the medium level (folded at 10^10); a promoted repeat goes hard."""
    picks = [AfterIdealEndTimeCost(), AfterIdealEndTimeCost(weight=2)]
    assert score(fp.constraint_costs(_hand_schedule()), picks) == (0, -300 * MEDIUM_UNIT)
    schedule = _hand_schedule()
    _zero_budget(schedule, picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -300, 0)
    mixed = [
        AfterIdealEndTimeCost(),
        AfterIdealEndTimeCost(level="hard"),
        AfterIdealEndTimeCost(level="soft", offset=40),
    ]
    assert score(fp.constraint_costs(_hand_schedule(), mixed), mixed) == (-100, -100 * MEDIUM_UNIT - 60)
    schedule = _hand_schedule()
    _zero_budget(schedule, mixed)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (-100, -100, -60)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Per line: line 0 at 350^2, line 1 empty (raw 0, evaluated, so charged in literal mode).
        (SquaredMakespanCost(offset=22500), (0, -100000.0)),
        (SquaredMakespanCost(offset=22500, clip=False), (0, -(100000.0 - 22500))),  # the empty line is rewarded
        (SquaredMakespanCost(offset=200000), (0, 0.0)),
        (SquaredMakespanCost(offset=200000, clip=False), (0, -(-77500.0 - 200000))),
        (SquaredMakespanCost(offset=-1), (0, -122501.0)),  # a negative offset adds to every non-zero line
        (SquaredMakespanCost(offset=-1, clip=False), (0, -122502.0)),  # ...or to every line
        (SquaredMakespanCost(offset=122000, exponent=2), (0, -(500.0**2))),
        (AfterIdealEndTimeCost(offset=40), (0, -60 * MEDIUM_UNIT)),
        (AfterIdealEndTimeCost(offset=40, clip=False), (0, -20 * MEDIUM_UNIT)),  # 60 + (0 - 40)
        # The literal formula with an offset is refused on hard; on soft it charges both lines.
        (AfterIdealEndTimeCost(offset=150, clip=False, level="soft"), (0, 200.0)),  # (100 - 150) + (0 - 150)
        (AfterIdealEndTimeCost(offset=40, level="hard"), (-60, 0.0)),  # hard takes the threshold
        (MaxEndTimeCost(level="soft", offset=1, clip=False), (0, 2.0)),  # two lines, not the pool: a reward
        (MaxEndTimeCost(level="soft", weight=3, offset=0.5, clip=False), (0, 3.0)),  # -1.5 per line, not rounded
        # The globals' totals are shaped once.
        (UnassignedJobsCost(offset=20, exponent=2), (0, -900 * MEDIUM_UNIT)),  # (50 - 20)^2
        (UnassignedJobsCost(offset=80, clip=False), (0, 30 * MEDIUM_UNIT)),
        (OperatorCleaningConflictCost(level="soft", offset=5, clip=False), (0, 5.0)),
        (OperatorCleaningConflictCost(offset=5), (0, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(fp.constraint_costs(_hand_schedule()), [pick]) == expected
    raw = fp.constraint_costs(_hand_schedule(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget(_hand_schedule(), [pick]) == expected


def test_the_per_line_constraints_evaluate_the_lines_and_not_the_pool():
    """Every per-line entry lists the lines as its "units"; the literal offset charges exactly
    those (2 x 2 on an all-zero max_end_time_cost, on soft: its hard default refuses the
    literal formula with an offset), never the pool's cell."""
    raw = fp.constraint_costs(_hand_schedule())
    for name, entry in raw.items():
        if entry["global"]:
            assert "units" not in entry and len(entry["costs"]) == 1, name
        else:
            assert entry["units"] == [0, 1], name
            assert len(entry["costs"]) == 3 and entry["costs"][2] == 0.0, name
    pick = MaxEndTimeCost(level="soft", offset=2, clip=False)
    assert raw["max_end_time_cost"]["costs"] == [0.0, 0.0, 0.0]
    assert score(raw, [pick]) == (0, 4.0)
    assert _zero_budget(_hand_schedule(), [pick]) == (0, 4.0)


def test_offset_zero_reproduces_the_plain_scores():
    """offset 0 is the plain shaping in both modes, so a selection scores the same with offset 0."""
    plain = replace_default(get_constraints(), "unassigned_jobs_cost", UnassignedJobsCost(exponent=2))
    shifted = [type(spec)(exponent=spec.exponent, offset=0.0, clip=False) for spec in plain]
    raw, solved = _raw()
    assert score(raw, shifted) == score(raw, plain)
    engines = [_zero_budget(solved, picks) for picks in (plain, shifted)]
    assert engines[0] == engines[1] == pytest.approx(score(raw, plain))
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


SELECTIONS = {
    "repeat": [*get_constraints(), *REPEAT, AfterIdealEndTimeCost(weight=2, exponent=2)],
    "repeat-and-offsets": [
        MaxEndTimeCost(),
        MaxEndTimeCost(level="soft", weight=1, offset=1, clip=False),  # hard refuses the literal formula
        MaxEndTimeCost(weight=1, offset=1),
        AfterIdealEndTimeCost(),
        AfterIdealEndTimeCost(offset=30, exponent=2),
        AfterIdealEndTimeCost(offset=-5, clip=False, level="soft", weight=3),
        SquaredMakespanCost(offset=10000),
        SquaredMakespanCost(offset=40000, clip=False, weight=0.25),
        OperatorCleaningConflictCost(),
        OperatorCleaningConflictCost(level="soft", offset=3, clip=False, weight=2),
        OperatorCleaningConflictCost(offset=3, weight=2),
        UnassignedJobsCost(),
        UnassignedJobsCost(offset=-10, level="soft"),
    ],
}


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_score_matches_the_engine_for_repeats_and_offsets(picks):
    raw, solved = _raw()
    engine = _zero_budget(solved, picks)
    python = score(fp.constraint_costs(solved, picks), picks)
    assert python.hard == engine[0]
    assert python.soft == pytest.approx(engine[1])
    assert score(raw, picks) == python  # the default costs score the repeats too


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
    schedule = fp.generate_demo_data(line_count=3, job_count=12)
    config = fp.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 2000
    config.spent_limit_seconds = 30
    fp.Solver(config).solve_with_constraints(schedule, picks)
    raw = fp.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    engine = _engine(schedule.score)
    assert python.hard == engine[0]
    assert python.soft == pytest.approx(engine[1])
    assert score(fp.constraint_costs(schedule), picks) == python


# --- Levels: each pick, a repeat included, on the level it names --------------------


def test_a_soft_constraint_can_move_to_medium():
    """On medium the job's 100^2 squared makespan outranks its 100 unassigned minutes, so
    the solver now leaves it in the pool (at the defaults it keeps the job)."""
    picks = [unassigned_jobs_cost, SquaredMakespanCost(level=Level.MEDIUM)]
    schedule = _one_job_one_line()
    fp.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -100, 0)
    assert score(fp.constraint_costs(schedule), picks) == _engine(schedule.score)


def test_core_refuses_an_unknown_level_like_the_python_base():
    pick = SimpleNamespace(name="max_end_time_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        fp.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        fp.constraint_costs(build_schedule(), [pick])


def test_a_pick_without_level_is_refused():
    """A duck-typed pick without `level` is refused on every path instead of running at the
    default level."""
    pick = SimpleNamespace(name="max_end_time_cost", weight=None, exponent=1.0)
    with pytest.raises(AttributeError, match="level"):
        fp.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(AttributeError, match="level"):
        fp.constraint_costs(build_schedule(), [pick])
    with pytest.raises(AttributeError, match="level"):
        score(fp.constraint_costs(build_schedule()), [pick])


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        # _hand_schedule(): after_ideal [100, 0, 0], squared_makespan [350^2, 0, 0],
        # max_end_time [0, 0, 0]; the globals unassigned_jobs [50], operator_cleaning [0].
        ([SquaredMakespanCost(level="medium")], (0, -122500 * MEDIUM_UNIT)),
        ([SquaredMakespanCost(level=Level.HARD)], (-122500, 0.0)),
        ([SquaredMakespanCost(level=Level.MEDIUM, weight=2)], (0, -245000 * MEDIUM_UNIT)),  # medium units
        ([MaxEndTimeCost(level="medium", offset=1, clip=False)], (0, 2 * MEDIUM_UNIT)),  # two lines at -1
        ([AfterIdealEndTimeCost(level=Level.SOFT, weight=2)], (0, -200.0)),
        ([UnassignedJobsCost(level="hard", offset=10)], (-40, 0.0)),
        ([OperatorCleaningConflictCost(level="medium", offset=5, clip=False)], (0, 5 * MEDIUM_UNIT)),
        # A repeat carries its own level and shaping: the soft square, medium beyond 122000
        # per line, hard 2 x beyond 122400 per line (2 x 100), and soft 2 x (x - 122600) per
        # line: 2 x (-100 - 122600) = -245400, a reward (hard refuses the literal formula with
        # an offset).
        (
            [
                SquaredMakespanCost(),
                SquaredMakespanCost(level="medium", offset=122000),
                SquaredMakespanCost(level="hard", weight=2, offset=122400),
                SquaredMakespanCost(level="soft", weight=2, offset=122600, clip=False),
            ],
            (-200, -500 * MEDIUM_UNIT - 122500 + 245400),
        ),
        # A medium global repeated onto all three levels, each total shaped once.
        (
            [
                UnassignedJobsCost(),
                UnassignedJobsCost(level=Level.HARD, weight=3, offset=20),
                UnassignedJobsCost(level=Level.SOFT, offset=80, clip=False),
            ],
            (-90, -50 * MEDIUM_UNIT + 30),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(fp.constraint_costs(_hand_schedule()), picks) == expected
    raw = fp.constraint_costs(_hand_schedule(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(_hand_schedule(), picks) == expected


def test_a_repeat_on_medium_reports_its_level_unfolded():
    """schedule.score reports each level on its own: the repeat's medium term is medium."""
    picks = [AfterIdealEndTimeCost(level="soft"), SquaredMakespanCost(), SquaredMakespanCost(level="medium")]
    schedule = _hand_schedule()
    _zero_budget(schedule, picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -122500, -122600)


def test_a_repeat_is_checked_on_its_own_level():
    """The second pick's level is read and checked like the first's, on both paths."""
    pick = SimpleNamespace(name="max_end_time_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="constraint 'max_end_time_cost': level must be 'hard', 'medium' or 'soft'"):
        _zero_budget(_hand_schedule(), [MaxEndTimeCost(), pick])
    with pytest.raises(ValueError, match="constraint 'max_end_time_cost': level must be 'hard', 'medium' or 'soft'"):
        score(fp.constraint_costs(_hand_schedule()), [MaxEndTimeCost(), pick])


# --- clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [2.5, -2.5])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: MaxEndTimeCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: MaxEndTimeCost(level="hard", offset=offset, clip=False),
        lambda offset: OperatorCleaningConflictCost(offset=offset, clip=False),  # a global, hard by default
        lambda offset: SquaredMakespanCost(level="hard", offset=offset, clip=False),
        lambda offset: AfterIdealEndTimeCost(level=Level.HARD, offset=offset, clip=False),
        lambda offset: UnassignedJobsCost(level="hard", offset=offset, clip=False),
    ],
    ids=[
        "max-end-default-hard",
        "max-end-level-hard",
        "cleaning-conflict-default-hard",
        "makespan-moved-to-hard",
        "after-ideal-moved-to-hard",
        "unassigned-moved-to-hard",
    ],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward the lines below it, so one line's slack would cancel
    another line's overrun and an infeasible schedule could score hard 0; a negative one
    would charge every line, an empty one included. The engine (solve,
    solve_with_constraints, constraint_costs) and the score() mirror refuse it with the same
    message; the CP-SAT reference's test_solve.py checks the same words."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    repeated = [type(spec)(), spec]  # a refused repeat next to its accepted first instance
    assert _refusal(lambda: fp.solve(build_schedule(), time_limit_seconds=0.1, constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget(_hand_schedule(), repeated)) == message
    assert _refusal(lambda: fp.constraint_costs(_hand_schedule(), [spec])) == message
    assert _refusal(lambda: fp.constraint_costs(_hand_schedule(), repeated)) == message
    assert _refusal(lambda: score(fp.constraint_costs(_hand_schedule()), [spec])) == message
    assert _refusal(lambda: score(fp.constraint_costs(_hand_schedule()), repeated)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """(0 - offset) on each of the two lines, on soft, on every path; never the unassigned
    pool. Medium folds into soft here, so it takes the literal formula too."""
    pick = MaxEndTimeCost(level="soft", offset=offset, clip=False)
    expected = (0, 2 * offset)
    assert score(fp.constraint_costs(_hand_schedule()), [pick]) == expected
    raw = fp.constraint_costs(_hand_schedule(), [pick])
    assert (raw["max_end_time_cost"]["level"], raw["max_end_time_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _zero_budget(_hand_schedule(), [pick]) == expected
    schedule = build_schedule()
    fp.solve(schedule, time_limit_seconds=0.2, random_seed=1, constraints=[MaxEndTimeCost(), pick])
    assert score(fp.constraint_costs(schedule), [MaxEndTimeCost(), pick]) == pytest.approx(_engine(schedule.score))
    medium = MaxEndTimeCost(level="medium", offset=offset, clip=False)
    assert score(fp.constraint_costs(_hand_schedule()), [medium]) == (0, expected[1] * MEDIUM_UNIT)
    assert _zero_budget(_hand_schedule(), [medium]) == (0, expected[1] * MEDIUM_UNIT)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # _hand_schedule(): after_ideal [100, 0] minutes on the two lines, moved to hard.
        (AfterIdealEndTimeCost(level="hard", offset=40), (-60, 0.0)),  # clip=True on hard: a threshold
        (AfterIdealEndTimeCost(level="hard", offset=-10), (-110, 0.0)),  # adds 10 to the non-zero line only
        (AfterIdealEndTimeCost(level="hard", clip=False), (-100, 0.0)),  # clip=False with offset 0: plain
        (AfterIdealEndTimeCost(level="hard", offset=0.0, clip=False), (-100, 0.0)),
        (MaxEndTimeCost(offset=-1), (0, 0.0)),  # no overrun: a threshold adds nothing to a raw 0
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(fp.constraint_costs(_hand_schedule()), [pick]) == expected
    assert _zero_budget(_hand_schedule(), [pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(name="max_end_time_cost", weight=None, level="soft", exponent=1.0, offset=1.0, clip=False)
    assert _zero_budget(_hand_schedule(), [moved]) == (0, 2.0)
    kept = SimpleNamespace(name="max_end_time_cost", weight=None, level=None, exponent=1.0, offset=1.0, clip=False)
    message = HARD_REFUSAL.format(q="'max_end_time_cost'")
    assert _refusal(lambda: _zero_budget(_hand_schedule(), [kept])) == message
    assert _refusal(lambda: fp.constraint_costs(_hand_schedule(), [kept])) == message
    assert _refusal(lambda: score(fp.constraint_costs(_hand_schedule()), [kept])) == message
