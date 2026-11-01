"""project_job.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py` and the C++
tests (one hand-calculated positive and zero case per constraint). This file covers the
selection path that picks "which constraints are on", the catalog (`constraint_info()`),
the five knobs, repeated picks and the medium level.
"""

import pickle
import re
from itertools import pairwise
from types import SimpleNamespace

import pytest

import project_job as pjs
from project_job import constraints as specs
from project_job._constraint import instance_keys
from project_job.constraints import (
    Constraint,
    Level,
    MakespanCost,
    NonRenewableResourceCapacityCost,
    ProjectDelayCost,
    RenewableResourceCapacityCost,
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

    hard_level = solve([MakespanCost(level="hard")])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "makespan_costs"  # trailing s
        weight = None
        level = None
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
    assert makespan_cost.level is None
    assert makespan_cost.exponent == 1.0
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0), spec.name
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
    assert score(raw, [*others, ProjectDelayCost(level="hard")]) == (-2, -3)
    assert score(raw, [*others, ProjectDelayCost(level="soft")]) == (0, -5)
    assert score(raw, [*others, ProjectDelayCost(weight=3)]) == (0, -3 - 6 * MEDIUM_UNIT)


def _solve_late(picks):
    schedule = _late_schedule()
    schedule.set_assignment([0, -1, 0])  # the solver places the 3-day job
    pjs.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    return schedule


def test_the_solver_reports_the_level_the_selection_gives_the_medium_term():
    """level="soft" moves project_delay_cost to soft in the solver (it was a no-op before
    0.2.0); level="hard" makes it one hard unit per day (not 10^9)."""
    others = [spec for spec in get_constraints() if spec.name != "project_delay_cost"]
    default = _solve_late(get_constraints())
    assert (default.score.hard, default.score.medium, default.score.soft) == (0, -2, -3)
    assert score(pjs.constraint_costs(default)) == _engine(default.score)
    for pick, expected in ((ProjectDelayCost(level="soft"), (0, 0, -5)), (ProjectDelayCost(level="hard"), (-2, 0, -3))):
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

    demoted = [*capacity, ProjectDelayCost(level="soft"), MakespanCost(weight=2)]
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
        ProjectDelayCost(level="soft", weight=MEDIUM_UNIT),
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
    negative = SimpleNamespace(name="makespan_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="makespan_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(name="makespan_cost", weight=None, level=None, exponent=1.0, offset=float("inf"))
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            pjs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            pjs.constraint_costs(build_schedule(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        MakespanCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="project_delay_cost", weight=None, level=None, exponent=1.0)
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
    pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        pjs.solve(build_schedule(), time_limit_seconds=0.05, constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(pjs.constraint_costs(build_schedule()), [pick])


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------
#
# The hand instance is _late_schedule(): one 3-day job, 2 days of project delay (medium),
# makespan 3 (soft), no resource use. All four constraints are global, so a pick's
# exponent / offset / clip shape each total once. The domain Score holds integer levels,
# so the engine comparisons use integral weights.


def _zero_budget(schedule, picks):
    """The engine's score of `schedule` under `picks`: a solve with no time keeps it as it is."""
    before = schedule.assignment()
    config = pjs.SolverConfig()
    config.spent_limit_seconds = 0
    pjs.Solver(config).solve_with_constraints(schedule, picks)
    assert schedule.assignment() == before
    return _engine(schedule.score)


REPEAT = [MakespanCost(), MakespanCost(weight=2, exponent=2)]


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [ProjectDelayCost(), MakespanCost(weight=10), MakespanCost()]
    assert _zero_budget(_late_schedule(), picks) == (0, -2 * MEDIUM_UNIT - 11 * 3)
    assert get_constraints([MakespanCost(), MakespanCost()]) == [MakespanCost(), MakespanCost()]
    schedule = build_schedule()
    pjs.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=[*get_constraints(), *REPEAT])
    assert score(pjs.constraint_costs(schedule), [*get_constraints(), *REPEAT]) == pytest.approx(
        _engine(schedule.score)
    )


def test_a_repeated_pick_by_hand():
    """The makespan plus twice its square: 3 + 2 x 3^2 = 21."""
    raw = pjs.constraint_costs(_late_schedule(), REPEAT)
    assert list(raw) == ["makespan_cost", "makespan_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [3.0]
    assert first["global"] and "units" not in first
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 2.0, 2.0)
    assert first["name"] == second["name"] == "makespan_cost"
    assert second["default_weight"] == 1.0
    assert second["medium_fold"] == {"kind": "into_soft", "unit": float(MEDIUM_UNIT)}
    expected = (0, -(3 + 2 * 3**2))
    assert expected == (0, -21)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(pjs.constraint_costs(_late_schedule()), REPEAT) == expected
    assert _zero_budget(_late_schedule(), REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [MakespanCost(), MakespanCost()]
    assert score(pjs.constraint_costs(_late_schedule()), picks) == (0, -6.0)
    assert _zero_budget(_late_schedule(), picks) == (0, -6.0)
    assert list(pjs.constraint_costs(_late_schedule(), picks)) == ["makespan_cost", "makespan_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        MakespanCost(weight=1),
        ProjectDelayCost(),
        MakespanCost(weight=2),
        NonRenewableResourceCapacityCost(),
        MakespanCost(weight=3),
    ]
    raw = pjs.constraint_costs(_late_schedule(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "non_renewable_resource_capacity_cost": 1.0,
        "project_delay_cost": 1.0,
        "makespan_cost": 1.0,
        "makespan_cost#2": 2.0,
        "makespan_cost#3": 3.0,
    }
    assert [key for key in raw if key.startswith("makespan")] == ["makespan_cost", "makespan_cost#2", "makespan_cost#3"]
    expected = (0, -2 * MEDIUM_UNIT - 18)
    assert score(raw, picks) == expected
    assert _zero_budget(_late_schedule(), picks) == expected


def test_a_repeated_medium_pick_adds_up():
    """Both instances stay on the medium level (folded at 10^9); a promoted repeat goes hard."""
    picks = [ProjectDelayCost(), ProjectDelayCost(weight=2)]
    assert score(pjs.constraint_costs(_late_schedule()), picks) == (0, -6 * MEDIUM_UNIT)
    schedule = _late_schedule()
    _zero_budget(schedule, picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -6, 0)
    mixed = [ProjectDelayCost(), ProjectDelayCost(level="hard"), ProjectDelayCost(level="soft", offset=1)]
    assert score(pjs.constraint_costs(_late_schedule(), mixed), mixed) == (-2, -2 * MEDIUM_UNIT - 1)
    schedule = _late_schedule()
    _zero_budget(schedule, mixed)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (-2, -2, -1)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Each global's single total is shaped once: makespan 3, delay 2, resources 0.
        (MakespanCost(offset=1), (0, -2.0)),
        (MakespanCost(offset=5), (0, 0.0)),
        (MakespanCost(offset=5, clip=False), (0, 2.0)),  # a makespan under day 5 is rewarded
        (MakespanCost(offset=-1), (0, -4.0)),  # a negative offset adds to a non-zero total
        (MakespanCost(offset=1, exponent=3), (0, -8.0)),
        (MakespanCost(offset=5, exponent=3, clip=False), (0, 8.0)),  # (3 - 5)^3
        (ProjectDelayCost(offset=1), (0, -1 * MEDIUM_UNIT)),
        (ProjectDelayCost(offset=4, clip=False), (0, 2 * MEDIUM_UNIT)),
        (ProjectDelayCost(offset=4, clip=False, level="hard"), (2, 0.0)),
        (RenewableResourceCapacityCost(offset=1, clip=False), (1, 0.0)),  # once, not per resource or day
        (RenewableResourceCapacityCost(offset=-2), (0, 0.0)),  # a raw 0 stays 0 under the threshold
        (RenewableResourceCapacityCost(offset=-2, clip=False), (-2, 0.0)),
        (NonRenewableResourceCapacityCost(weight=3, offset=0.5, clip=False), (2, 0.0)),  # -1.5 -> -2
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(pjs.constraint_costs(_late_schedule()), [pick]) == expected
    raw = pjs.constraint_costs(_late_schedule(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert score(raw, [pick]) == expected
    assert _zero_budget(_late_schedule(), [pick]) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*get_constraints()[:-1], MakespanCost(exponent=2)]
    shifted = [type(spec)(exponent=spec.exponent, offset=0.0, clip=False) for spec in plain]
    raw, solved = _raw()
    assert score(raw, shifted) == score(raw, plain)
    engines = [_zero_budget(solved, picks) for picks in (plain, shifted)]
    assert engines[0] == engines[1] == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)
        assert "units" not in entry


SELECTIONS = {
    "repeat": [*get_constraints(), *REPEAT, ProjectDelayCost(weight=2, exponent=2)],
    "repeat-and-offsets": [
        NonRenewableResourceCapacityCost(),
        NonRenewableResourceCapacityCost(offset=1, clip=False),
        RenewableResourceCapacityCost(),
        RenewableResourceCapacityCost(offset=-1),
        ProjectDelayCost(),
        ProjectDelayCost(offset=5, exponent=2),
        ProjectDelayCost(offset=-2, clip=False, level="soft", weight=3),
        MakespanCost(offset=20),
        MakespanCost(offset=40, clip=False, weight=2),
    ],
}


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_score_matches_the_engine_for_repeats_and_offsets(picks):
    raw, solved = _raw()
    engine = _zero_budget(solved, picks)
    python = score(pjs.constraint_costs(solved, picks), picks)
    assert python.hard == engine[0]
    assert python.soft == pytest.approx(engine[1])
    assert score(raw, picks) == python  # the default costs score the repeats too


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    schedule = build_schedule()
    config = pjs.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 2000
    config.spent_limit_seconds = 30
    pjs.Solver(config).solve_with_constraints(schedule, picks)
    raw = pjs.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    engine = _engine(schedule.score)
    assert python.hard == engine[0]
    assert python.soft == pytest.approx(engine[1])
    assert score(pjs.constraint_costs(schedule), picks) == python


# --- Levels: each pick, a repeat included, on the level it names --------------------


def test_a_soft_constraint_can_move_to_medium():
    """With the doubled makespan on medium next to the delay, 3 + 2 * 3 = 9 medium days beat
    1 + 2 * 5 = 11 (at the default levels the delay alone decides: P fast)."""
    capacity = [non_renewable_resource_capacity_cost, renewable_resource_capacity_cost]
    picks = [*capacity, project_delay_cost, MakespanCost(level=Level.MEDIUM, weight=2)]
    schedule = _fast_or_slow()
    pjs.solve(schedule, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -9, 0)
    assert score(pjs.constraint_costs(schedule), picks) == _engine(schedule.score)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        MakespanCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft'"):
        MakespanCost(level="firm")
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft'"):
        MakespanCost(level="HARD")


def test_core_refuses_an_unknown_level_like_the_python_base():
    pick = SimpleNamespace(name="makespan_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        pjs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        pjs.constraint_costs(_late_schedule(), [pick])


def test_the_knob_was_renamed():
    """hard= is gone: a spec refuses it, and an old duck-typed pick that carries `hard` but
    no `level` is refused on every path instead of running at the default level."""
    with pytest.raises(TypeError):
        MakespanCost(hard=True)
    old = SimpleNamespace(name="makespan_cost", weight=None, hard=True, exponent=1.0)
    with pytest.raises(AttributeError, match="level"):
        pjs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[old])
    with pytest.raises(AttributeError, match="level"):
        pjs.constraint_costs(_late_schedule(), [old])
    with pytest.raises(AttributeError, match="level"):
        score(pjs.constraint_costs(_late_schedule()), [old])


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        # _late_schedule(): makespan 3, delay 2, resources 0; every total shaped once.
        ([MakespanCost(level="medium")], (0, -3 * MEDIUM_UNIT)),
        ([MakespanCost(level=Level.HARD)], (-3, 0)),
        ([MakespanCost(level=Level.MEDIUM, weight=2)], (0, -6 * MEDIUM_UNIT)),  # weight in medium units
        ([RenewableResourceCapacityCost(level="medium", offset=1, clip=False)], (0, MEDIUM_UNIT)),
        ([ProjectDelayCost(level=Level.SOFT, weight=2)], (0, -4)),
        ([NonRenewableResourceCapacityCost(level="soft", offset=2, clip=False)], (0, 2)),
        # A repeat carries its own level and shaping: the soft makespan, medium beyond day 1,
        # and hard 2 x (3 - 5) = -4, a hard reward.
        (
            [
                MakespanCost(),
                MakespanCost(level="medium", offset=1),
                MakespanCost(level="hard", weight=2, offset=5, clip=False),
            ],
            (4, -2 * MEDIUM_UNIT - 3),
        ),
        # The medium constraint repeated onto all three levels.
        (
            [
                ProjectDelayCost(),
                ProjectDelayCost(level=Level.HARD, weight=3, offset=1),
                ProjectDelayCost(level=Level.SOFT, offset=5, clip=False),
            ],
            (-3, -2 * MEDIUM_UNIT + 3),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(pjs.constraint_costs(_late_schedule()), picks) == expected
    raw = pjs.constraint_costs(_late_schedule(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _zero_budget(_late_schedule(), picks) == expected


def test_a_repeat_on_medium_reports_its_level_unfolded():
    """schedule.score reports each level on its own: the repeat's medium term is medium."""
    picks = [ProjectDelayCost(level="soft"), MakespanCost(), MakespanCost(level="medium")]
    schedule = _late_schedule()
    _zero_budget(schedule, picks)
    assert (schedule.score.hard, schedule.score.medium, schedule.score.soft) == (0, -3, -5)


def test_a_repeat_is_checked_on_its_own_level():
    """The second pick's level is read and checked like the first's, on both paths."""
    pick = SimpleNamespace(name="makespan_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="constraint 'makespan_cost': level must be 'hard', 'medium' or 'soft'"):
        _zero_budget(_late_schedule(), [MakespanCost(), pick])
    with pytest.raises(ValueError, match="constraint 'makespan_cost': level must be 'hard', 'medium' or 'soft'"):
        score(pjs.constraint_costs(_late_schedule()), [MakespanCost(), pick])
