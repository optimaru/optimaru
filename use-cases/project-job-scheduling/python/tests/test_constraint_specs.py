"""project_job.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py` and the C++
tests (one hand-calculated positive and zero case per constraint). This file covers the
selection path that picks "which constraints are on", the catalog (`constraint_info()`),
the three knobs and the medium level.
"""

import pickle
import re
from itertools import pairwise
from types import SimpleNamespace

import pytest

import project_job as pjs
from project_job import constraints as specs
from project_job.constraints import (
    Constraint,
    MakespanCost,
    NonRenewableResourceCapacityCost,
    ProjectDelayCost,
    get_constraints,
    makespan_cost,
    non_renewable_resource_capacity_cost,
    project_delay_cost,
    renewable_resource_capacity_cost,
    score,
)
from project_job.demo import generate_demo_data
from project_job.models import (
    AllocationModel,
    ExecutionModeModel,
    JobModel,
    ProjectJobScheduleModel,
    ProjectModel,
    ResourceModel,
    ResourceRequirementModel,
)

MEDIUM_UNIT = 1_000_000_000  # the domain's medium fold (into soft): 10^9


def build_schedule():
    return generate_demo_data().to_core()


def solve(constraints=None):
    schedule = build_schedule()
    pjs.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = pjs.list_constraints()
    assert available == [
        "non_renewable_resource_capacity_cost",
        "renewable_resource_capacity_cost",
        "project_delay_cost",
        "makespan_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == pjs.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the medium term disappears entirely."""
    baseline = solve([renewable_resource_capacity_cost, project_delay_cost, makespan_cost])
    assert baseline.soft < 0

    without_delay = solve([renewable_resource_capacity_cost, makespan_cost])
    assert without_delay.medium == 0
    assert without_delay.soft < 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the makespan weight grows soft only; hard stays put."""
    base = solve([renewable_resource_capacity_cost, makespan_cost])
    heavier = solve([renewable_resource_capacity_cost, MakespanCost(weight=10)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    The makespan is always positive, so raising the level always lands on hard."""
    soft_level = solve([makespan_cost])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([MakespanCost(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "makespan_costs"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        pjs.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert makespan_cost.weight is None
    assert makespan_cost.hard is None
    assert makespan_cost.exponent == 1.0
    for spec in get_constraints():
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0), spec.name
        assert getattr(specs, spec.name) is spec  # the instance is named after the constraint


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        for attribute in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(type(spec), attribute).strip(), f"{spec.name}.{attribute}"


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    pjs.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return pjs.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == pjs.calculate_score(solution).hard


def test_score_reproduces_the_engine_score():
    """With the medium fold on every entry, score() equals the folded engine score."""
    raw, solution = _raw()
    assert score(raw) == _engine(pjs.calculate_score(solution))


def test_score_is_additive_over_the_picked_constraints():
    """The total score is the sum of per-constraint scores — unlisted terms simply drop out.

    Hard rounding happens per unit contribution, so splitting by constraint and
    summing gives the same value. "How much does this term add to the score"
    can be read without rescoring."""
    raw, _ = _raw()
    picked = get_constraints()
    full = score(raw, picked)
    parts = [score(raw, [spec]) for spec in picked]
    assert full.hard == sum(part.hard for part in parts)
    assert full.soft == pytest.approx(sum(part.soft for part in parts))


def test_score_reweighting_is_linear_without_rescoring():
    """Changing only the weight scales that term linearly — no rescoring."""
    raw, _ = _raw()
    base = score(raw, [makespan_cost])
    ten = score(raw, [MakespanCost(weight=10)])
    default_weight = raw[makespan_cost.name]["weight"]
    factor = 10 / default_weight
    if base.hard:
        assert ten.hard == pytest.approx(base.hard * factor, rel=1e-6)
    else:
        assert ten.soft == pytest.approx(base.soft * factor, rel=1e-9)


def test_score_rejects_an_unknown_constraint_name():
    """A spec with a wrong name (e.g. a renamed component) must not be silently ignored."""
    raw, _ = _raw()

    class Renamed(Constraint):
        name = "definitely_not_a_constraint"

    with pytest.raises(ValueError, match="unknown constraint"):
        score(raw, [Renamed()])


# --- The catalog: constraint_info() -------------------------------------------

# (name, default level, default weight), in list_constraints() order. The medium
# weight is in medium units (1, not the 10^9 fold).
EXPECTED_INFO = [
    ("non_renewable_resource_capacity_cost", "hard", 1.0),
    ("renewable_resource_capacity_cost", "hard", 1.0),
    ("project_delay_cost", "medium", 1.0),
    ("makespan_cost", "soft", 1.0),
]


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = pjs.constraint_info()
    assert [row.name for row in rows] == pjs.list_constraints()
    assert [(row.name, row.default_level, row.default_weight) for row in rows] == EXPECTED_INFO
    # Every constraint is flippable, none is a reward, and all four are on by default.
    assert all(row.level_flippable and not row.reward and row.default_enabled for row in rows)


def test_constraint_info_rows_are_plain_values():
    row = pjs.constraint_info()[2]
    assert isinstance(row, pjs.ConstraintInfo)
    assert row._asdict() == {
        "name": "project_delay_cost",
        "default_level": "medium",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='project_delay_cost'")
    assert pickle.loads(pickle.dumps(pjs.constraint_info())) == pjs.constraint_info()
    assert pjs.ConstraintInfo.__module__ == pjs._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults and carries the domain's medium fold on every entry."""
    raw = pjs.constraint_costs(build_schedule())
    assert list(raw) == pjs.list_constraints()
    for row in pjs.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] is True  # every project-job constraint is global
        assert len(entry["costs"]) == 1
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}


# --- The knobs and the medium level ----------------------------------------------


def _engine(score_):
    """The engine-level (hard, soft) of a HardMediumSoft score (medium folded into soft)."""
    return score_.hard, score_.medium * MEDIUM_UNIT + score_.soft


def _late_schedule():
    """SOURCE -> one 3-day job -> SINK, all at delay 0, critical path 1 day: the SINK
    ends on day 3, 2 days late; makespan 3; no resource use."""
    jobs = [
        JobModel(
            id="s",
            project_id="P",
            job_type="SOURCE",
            execution_modes=[ExecutionModeModel(id="ms", duration=0)],
            successor_job_ids=["a"],
        ),
        JobModel(
            id="a", project_id="P", execution_modes=[ExecutionModeModel(id="ma", duration=3)], successor_job_ids=["t"]
        ),
        JobModel(id="t", project_id="P", job_type="SINK", execution_modes=[ExecutionModeModel(id="mt", duration=0)]),
    ]
    allocations = [
        AllocationModel(id=job.id, job_id=job.id, execution_mode_id=job.execution_modes[0].id, delay=0) for job in jobs
    ]
    return ProjectJobScheduleModel(
        projects=[ProjectModel(id="P", release_date=0, critical_path_duration=1)],
        resources=[ResourceModel(id="R", capacity=1, renewable=True)],
        jobs=jobs,
        allocations=allocations,
        delay_range_size=5,
    ).to_core()


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit C++ default weights."""
    explicit = [type(spec)(weight=weight) for spec, (_, _, weight) in zip(get_constraints(), EXPECTED_INFO)]
    schedule = _late_schedule()
    raw = pjs.constraint_costs(schedule)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == _engine(pjs.calculate_score(schedule))


def test_the_medium_level_folds_into_soft():
    schedule = _late_schedule()
    raw = pjs.constraint_costs(schedule)
    assert {name: entry["costs"] for name, entry in raw.items()} == {
        "non_renewable_resource_capacity_cost": [0.0],
        "renewable_resource_capacity_cost": [0.0],
        "project_delay_cost": [2.0],
        "makespan_cost": [3.0],
    }
    monolithic = pjs.calculate_score(schedule)
    assert (monolithic.hard, monolithic.medium, monolithic.soft) == (0, -2, -3)
    assert score(raw) == (0, -3 - 2 * MEDIUM_UNIT)
    others = [spec for spec in get_constraints() if spec.name != "project_delay_cost"]
    assert score(raw, [*others, ProjectDelayCost(hard=True)]) == (-2, -3)
    assert score(raw, [*others, ProjectDelayCost(hard=False)]) == (0, -5)
    assert score(raw, [*others, ProjectDelayCost(weight=3)]) == (0, -3 - 6 * MEDIUM_UNIT)


def _solve_late(picks):
    schedule = _late_schedule()
    schedule.set_assignment([0, -1, 0])  # the solver places the 3-day job
    pjs.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    return schedule


def test_the_solver_reports_the_level_the_selection_gives_the_medium_term():
    """hard=False moves project_delay_cost to soft in the solver (it was a no-op before
    0.2.0); hard=True makes it one hard unit per day (not 10^9)."""
    others = [spec for spec in get_constraints() if spec.name != "project_delay_cost"]
    default = _solve_late(get_constraints())
    assert (default.score.hard, default.score.medium, default.score.soft) == (0, -2, -3)
    assert score(pjs.constraint_costs(default)) == _engine(default.score)
    for pick, expected in ((ProjectDelayCost(hard=False), (0, 0, -5)), (ProjectDelayCost(hard=True), (-2, 0, -3))):
        picks = [*others, pick]
        schedule = _solve_late(picks)
        assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == expected
        assert score(pjs.constraint_costs(schedule), picks) == (schedule.score.hard, schedule.score.soft)


def _fast_or_slow():
    """Two projects, each with one job that runs FAST (needing the single unit of a
    non-renewable stock) or SLOW (needing nothing), so at most one job can be fast.
    P (critical path 0, so its delay is its end day): fast 1 day, slow 3. Q (critical path
    100, never late): fast 2 days, slow 5. P fast + Q slow: 1 day of delay at makespan 5;
    P slow + Q fast: 3 days of delay at makespan 3. Both jobs list their modes in the same
    order, so one swap move turns either choice into the other."""
    projects, jobs, allocations = [], [], []
    for project, critical_path, fast, slow in (("P", 0, 1, 3), ("Q", 100, 2, 5)):
        projects.append(ProjectModel(id=project, release_date=0, critical_path_duration=critical_path))
        modes = [
            ExecutionModeModel(
                id=f"{project}-fast",
                duration=fast,
                resource_requirements=[ResourceRequirementModel(resource_id="S", requirement=1)],
            ),
            ExecutionModeModel(id=f"{project}-slow", duration=slow),
        ]
        jobs.append(
            JobModel(
                id=f"{project}-job", project_id=project, execution_modes=modes, successor_job_ids=[f"{project}-end"]
            )
        )
        jobs.append(
            JobModel(
                id=f"{project}-end",
                project_id=project,
                job_type="SINK",
                execution_modes=[ExecutionModeModel(id=f"{project}-end-mode", duration=0)],
            )
        )
        allocations.append(AllocationModel(id=f"{project}-job", job_id=f"{project}-job"))
        allocations.append(
            AllocationModel(
                id=f"{project}-end", job_id=f"{project}-end", execution_mode_id=f"{project}-end-mode", delay=0
            )
        )
    return ProjectJobScheduleModel(
        projects=projects,
        resources=[ResourceModel(id="S", capacity=1, renewable=False, project_id="P")],
        jobs=jobs,
        allocations=allocations,
        delay_range_size=3,
    ).to_core()


def test_demoting_the_medium_term_changes_the_trade_off():
    """At the default medium level the project delay dominates a doubled makespan (P fast:
    1 day of delay, makespan 5); demoted to soft, 3 + 2 * 3 = 9 beats 1 + 2 * 5 = 11."""
    capacity = [non_renewable_resource_capacity_cost, renewable_resource_capacity_cost]
    medium = [*capacity, project_delay_cost, MakespanCost(weight=2)]
    schedule = _fast_or_slow()
    pjs.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=medium)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -1, -10)
    assert score(pjs.constraint_costs(schedule), medium) == _engine(schedule.score)

    demoted = [*capacity, ProjectDelayCost(hard=False), MakespanCost(weight=2)]
    schedule = _fast_or_slow()
    pjs.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=demoted)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, 0, -9)
    assert score(pjs.constraint_costs(schedule), demoted) == (0, -9)


def _solve_reporting(run, schedule):
    """Solve `schedule` single-start with `run(solver, schedule)`, recording every on_new_best score."""
    reported = []
    cfg = pjs.SolverConfig()
    cfg.spent_limit_seconds = 0.4
    cfg.random_seed = 1
    cfg.on_new_best = lambda _step, best: reported.append((best.hard, best.medium, best.soft))
    run(pjs.Solver(cfg), schedule)
    return reported


def test_the_plugin_solve_reports_each_level_unfolded():
    """schedule.score and on_new_best on the plugin path read the calculator's levels, not a decoded fold.

    With project_delay_cost demoted to soft at weight 10^9, nothing is medium, so the
    soft total of 2 * 10^9 + 3 must stay soft in the final score and in every new best."""
    picks = [
        non_renewable_resource_capacity_cost,
        renewable_resource_capacity_cost,
        ProjectDelayCost(hard=False, weight=MEDIUM_UNIT),
        makespan_cost,
    ]
    schedule = _late_schedule()
    schedule.set_assignment([0, -1, 0])  # the solver places the 3-day job
    reported = _solve_reporting(lambda solver, s: solver.solve_with_constraints(s, picks), schedule)
    final = (schedule.score.hard, schedule.score.medium, schedule.score.soft)
    assert final == (0, 0, -(2 * MEDIUM_UNIT + 3))  # the case a decoded fold would misfile
    assert score(pjs.constraint_costs(schedule), picks) == (final[0], final[2])
    assert reported and all(medium == 0 for _, medium, _ in reported)
    assert reported[-1] == final


@pytest.mark.parametrize(
    "run",
    [
        pytest.param(lambda solver, schedule: solver.solve(schedule), id="monolithic"),
        pytest.param(
            lambda solver, schedule: solver.solve_with_constraints(schedule, get_constraints()),
            id="plugin",
        ),
    ],
)
def test_on_new_best_ends_on_the_final_score(run):
    """Each path reports strictly improving new bests in the levels of its final schedule.score."""
    schedule = build_schedule()
    reported = _solve_reporting(run, schedule)
    assert reported[-1] == (schedule.score.hard, schedule.score.medium, schedule.score.soft)
    assert all(earlier < later for earlier, later in pairwise(reported))


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        NonRenewableResourceCapacityCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="makespan_cost", weight=-2.0, hard=None, exponent=1.0)
    flat = SimpleNamespace(name="makespan_cost", weight=None, hard=None, exponent=0.0)
    for pick, message in ((negative, "weight must be finite and >= 0"), (flat, "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            pjs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="project_delay_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'project_delay_cost': {re.escape(message)}"):
        pjs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        ProjectDelayCost(**knobs)


# 0.2.0 migration: the pre-0.2.0 catalog names, which must be unknown now.
OLD_NAMES = [
    "non_renewable_resource_capacity",
    "renewable_resource_capacity",
    "total_project_delay",
    "total_makespan",
]


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        pjs.solve(build_schedule(), time_limit_seconds=0.05, constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(pjs.constraint_costs(build_schedule()), [pick])
