"""maintenance_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the catalog
(`constraint_info()`) and the three knobs.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import maintenance_scheduling as mnt
from maintenance_scheduling.constraints import (
    AfterIdealEndDateCost,
    BeforeIdealEndDateCost,
    Constraint,
    CrewConflictCost,
    MaxEndDateCost,
    MinStartDateCost,
    TagConflictCost,
    after_ideal_end_date_cost,
    before_ideal_end_date_cost,
    crew_conflict_cost,
    get_constraints,
    max_end_date_cost,
    min_start_date_cost,
    score,
    tag_conflict_cost,
)

MONDAY = 18659  # fixed Monday epoch day — a reproducible demo instance

# 2021-02-01 (Monday) == epoch day 18659, DAY_1 of the Java test.
DAY_1 = MONDAY
DAY_2 = DAY_1 + 1
DAY_3 = DAY_1 + 2


def build_schedule():
    return mnt.generate_demo_data("SMALL", MONDAY)


def solve(constraints=None):
    schedule = build_schedule()
    mnt.solve(schedule, time_limit_seconds=0.5, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints (crew/tag conflicts) after.
    available = mnt.list_constraints()
    assert available == [
        "min_start_date_cost",
        "max_end_date_cost",
        "before_ideal_end_date_cost",
        "after_ideal_end_date_cost",
        "crew_conflict_cost",
        "tag_conflict_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == mnt.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring.

    Whether both paths reach the same **solution** cannot be checked: three knobs do
    not apply on the plugin path — parallelSolverCount and two guided selection hooks
    (windowValueProbability / hotJobProbability) read the monolithic calculator's
    live state (cpp/include/mnt/solver.hpp)."""
    schedule = build_schedule()
    mnt.solve(
        schedule,
        time_limit_seconds=0.5,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = mnt.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve([min_start_date_cost, max_end_date_cost, crew_conflict_cost])
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the lateness weight grows soft only; hard stays put."""
    picks = [min_start_date_cost, max_end_date_cost, crew_conflict_cost]
    base = solve([*picks, after_ideal_end_date_cost])
    heavier = solve([*picks, AfterIdealEndDateCost(weight=10**9)])
    assert heavier.hard == base.hard
    assert heavier.soft <= base.soft  # penalty, so equal or more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level."""
    picks = [min_start_date_cost, max_end_date_cost]

    soft_level = solve([*picks, before_ideal_end_date_cost])
    assert soft_level.hard == 0

    hard_level = solve([*picks, type(before_ideal_end_date_cost)(level="hard")])
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "tag_conflict_costs"  # trailing s
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        mnt.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    for spec in (
        min_start_date_cost,
        max_end_date_cost,
        before_ideal_end_date_cost,
        after_ideal_end_date_cost,
        crew_conflict_cost,
        tag_conflict_cost,
    ):
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in get_constraints()] == [
        "min_start_date_cost",
        "max_end_date_cost",
        "before_ideal_end_date_cost",
        "after_ideal_end_date_cost",
        "crew_conflict_cost",
        "tag_conflict_cost",
    ]


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = mnt.constraint_info()
    assert [row.name for row in rows] == mnt.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 1.0, True, False, True),
        ("soft", 1_000_000.0, True, False, True),
        ("hard", 1.0, True, False, True),
        ("soft", 1_000.0, True, False, True),
    ]


def test_constraint_info_rows_are_plain_values():
    row = mnt.constraint_info()[0]
    assert isinstance(row, mnt.ConstraintInfo)
    assert row._asdict() == {
        "name": "min_start_date_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='min_start_date_cost'")
    assert pickle.loads(pickle.dumps(mnt.constraint_info())) == mnt.constraint_info()
    assert mnt.ConstraintInfo.__module__ == mnt._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = mnt.constraint_costs(build_schedule())
    global_names = {"crew_conflict_cost", "tag_conflict_cost"}
    for row in mnt.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name in global_names)
        assert (entry["name"], entry["label"]) == (row.name, "")  # one instance per name, no label
        assert "medium_fold" not in entry  # no medium level in this domain


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights 1 / 1 / 1 / 10^6 / 1 / 1000."""
    explicit = [
        MinStartDateCost(weight=1),
        MaxEndDateCost(weight=1),
        BeforeIdealEndDateCost(weight=1),
        AfterIdealEndDateCost(weight=1_000_000),
        CrewConflictCost(weight=1),
        TagConflictCost(weight=1_000),
    ]
    schedule = _solved()
    raw = mnt.constraint_costs(schedule)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (schedule.score.hard, schedule.score.soft)


@pytest.mark.parametrize(
    ("kwargs", "error"),
    [
        ({"weight": -1}, ValueError),
        ({"weight": float("nan")}, ValueError),
        ({"weight": float("inf")}, ValueError),
        ({"weight": "2"}, TypeError),
        ({"exponent": 0}, ValueError),
        ({"exponent": -1.0}, ValueError),
        ({"hard": 1}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        TagConflictCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "tag_conflict_cost"
        weight = -2.0
        level = None
        exponent = 1.0

    class Flat:
        name = "tag_conflict_cost"
        weight = None
        level = None
        exponent = 0.0

    for pick, message in ((Negative(), "weight must be finite and >= 0"), (Flat(), "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            mnt.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"level": 1}, "level must be a Level, its name or None, not 1"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="tag_conflict_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'tag_conflict_cost': {re.escape(message)}"):
        mnt.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        TagConflictCost(**knobs)


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in (
        "min_start_date",
        "max_end_date",
        "before_ideal_end_date",
        "after_ideal_end_date",
        "crew_conflict",
        "tag_conflict",
    ):

        class Old:
            name = old
            weight = None
            level = None
            exponent = 1.0

        with pytest.raises(ValueError, match="unknown constraint"):
            mnt.solve(build_schedule(), time_limit_seconds=0.1, constraints=[Old()])


def _schedule_of(jobs_and_assignments):
    """[(Job, crew_index, start_epoch|None)] -> a MaintenanceSchedule with that assignment."""
    jobs = [job for job, _, _ in jobs_and_assignments]
    schedule = mnt.MaintenanceSchedule(
        DAY_1,
        DAY_1 + 28,
        [mnt.Crew("1", "Alpha crew"), mnt.Crew("2", "Beta crew")],
        jobs,
    )
    date_index = {epoch: i for i, epoch in enumerate(schedule.start_dates())}
    schedule.set_assignment(
        [
            -1 if crew is None else schedule.crew_date_of(date_index[start], crew)
            for _, crew, start in jobs_and_assignments
        ]
    )
    return schedule


def test_constraint_costs_match_every_constraint_by_hand():
    """Each raw component is checked without running a solver.

    Job a (3 business days from Mon on crew 0) occupies [Mon, Thu); job b (3 days from
    Tue on crew 0) occupies [Tue, Fri): they overlap on Tue and Wed (2 days) and share
    the tags Downtown and Crane, so crew_conflict_cost = 2 and tag_conflict_cost =
    2 days x 2 tags = 4. Job a starts 2 days before its min start (Wed) and ends 1 day
    after its ideal end (Wed); job b ends 1 day past its max end (Thu) and 1 day before
    its ideal end (Sat). The unassigned job contributes nothing.
    """
    schedule = _schedule_of(
        [
            (mnt.Job("a", "a", 3, min_start_date=DAY_3, ideal_end_date=DAY_3, tags=["Downtown", "Crane"]), 0, DAY_1),
            (
                mnt.Job("b", "b", 3, max_end_date=DAY_1 + 3, ideal_end_date=DAY_1 + 5, tags=["Crane", "Downtown"]),
                0,
                DAY_2,
            ),
            (mnt.Job("c", "c", 9, min_start_date=DAY_3, max_end_date=DAY_1, ideal_end_date=DAY_1), None, None),
        ]
    )
    raw = mnt.constraint_costs(schedule)
    cells = schedule.crew_date_count()
    a_cell = schedule.crew_date_of(0, 0)
    b_cell = schedule.crew_date_of(1, 0)

    def one_cell(cell, value):
        costs = [0.0] * cells
        costs[cell] = value
        return costs

    assert raw["min_start_date_cost"]["costs"] == one_cell(a_cell, 2.0)
    assert raw["max_end_date_cost"]["costs"] == one_cell(b_cell, 1.0)
    assert raw["before_ideal_end_date_cost"]["costs"] == one_cell(b_cell, 1.0)
    assert raw["after_ideal_end_date_cost"]["costs"] == one_cell(a_cell, 1.0)
    assert raw["crew_conflict_cost"]["costs"] == [2.0]
    assert raw["tag_conflict_cost"]["costs"] == [4.0]
    # Default weights: min start / max end / crew 1 hard; before 1, after 10^6, tag 1000 soft.
    assert score(raw) == (-(2 + 1 + 2), -(1 + 1_000_000 + 1_000 * 4))
    engine = mnt.calculate_score(schedule)
    assert score(raw) == (engine.hard, engine.soft)


def test_constraint_costs_are_zero_without_violations():
    """Every job inside its window, on its ideal date, alone on its crew, no shared tag: all 0."""
    schedule = _schedule_of(
        [
            (
                mnt.Job("a", "a", 1, min_start_date=DAY_1, max_end_date=DAY_2, ideal_end_date=DAY_2, tags=["Downtown"]),
                0,
                DAY_1,
            ),
            (
                mnt.Job("b", "b", 1, min_start_date=DAY_1, max_end_date=DAY_2, ideal_end_date=DAY_2, tags=["Uptown"]),
                1,
                DAY_1,
            ),
        ]
    )
    raw = mnt.constraint_costs(schedule)
    for name in mnt.list_constraints():
        assert not any(raw[name]["costs"]), name
    assert score(raw) == (0, 0.0)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _solved(constraints=None):
    schedule = build_schedule()
    mnt.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return schedule


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = _solved()
    return mnt.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = mnt.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [crew_conflict_cost, TagConflictCost(weight=10_000)],
        [MaxEndDateCost(level="soft"), min_start_date_cost, crew_conflict_cost, after_ideal_end_date_cost],
        [min_start_date_cost, max_end_date_cost, crew_conflict_cost, TagConflictCost(exponent=2.0)],
        [CrewConflictCost(exponent=2.0), BeforeIdealEndDateCost(level="hard", weight=0.5), tag_conflict_cost],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per unit (per cell for the date rules, on the total for the
    two global conflict rules), so reproducing it needs the per-unit raw costs."""
    schedule = _solved(specs)
    python = score(mnt.constraint_costs(schedule), specs)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)


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
    base = score(raw, [tag_conflict_cost])
    ten = score(raw, [TagConflictCost(weight=10)])
    default_weight = raw[tag_conflict_cost.name]["weight"]
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


def test_naming_a_constraint_twice_is_rejected():
    """Passing the same constraint twice must not let the later one silently win."""
    with pytest.raises(ValueError, match="picked more than once"):
        mnt.solve(
            build_schedule(),
            time_limit_seconds=0.2,
            constraints=[crew_conflict_cost, TagConflictCost(weight=10), tag_conflict_cost],
        )
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([crew_conflict_cost, crew_conflict_cost])
