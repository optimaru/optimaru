"""meeting_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the
catalog (`constraint_info()`), the knobs and the medium level.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import meeting_scheduling as ms
from meeting_scheduling.constraints import (
    Constraint,
    MeetingEndTimeCost,
    MeetingOverlapCost,
    PreferredAttendanceConflictCost,
    RequiredAndPreferredAttendanceConflictCost,
    RoomConflictCost,
    consecutive_meetings_cost,
    day_crossing_cost,
    get_constraints,
    larger_room_capacity_cost,
    meeting_end_time_cost,
    meeting_overlap_cost,
    overtime_cost,
    preferred_attendance_conflict_cost,
    required_and_preferred_attendance_conflict_cost,
    required_attendance_conflict_cost,
    room_capacity_cost,
    room_change_cost,
    room_conflict_cost,
    score,
)

HARD_PICKS = [
    overtime_cost,
    room_capacity_cost,
    day_crossing_cost,
    room_conflict_cost,
    required_attendance_conflict_cost,
]

MEDIUM = ("required_and_preferred_attendance_conflict_cost", "preferred_attendance_conflict_cost")


def build_schedule():
    return ms.demo_data()


def solve(constraints=None):
    schedule = build_schedule()
    ms.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints after — differs from emplace order.
    available = ms.list_constraints()
    assert available == [
        "overtime_cost",
        "room_capacity_cost",
        "day_crossing_cost",
        "meeting_end_time_cost",
        "larger_room_capacity_cost",
        "room_conflict_cost",
        "required_attendance_conflict_cost",
        "required_and_preferred_attendance_conflict_cost",
        "preferred_attendance_conflict_cost",
        "consecutive_meetings_cost",
        "meeting_overlap_cost",
        "room_change_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ms.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    schedule = build_schedule()
    ms.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=get_constraints())
    recomputed = ms.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve(HARD_PICKS)
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the overlap weight grows soft only; hard stays put."""
    base = solve([*HARD_PICKS, meeting_overlap_cost])
    heavier = solve([*HARD_PICKS, MeetingOverlapCost(weight=10_000)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Meetings must sit somewhere on the calendar, so the sum of last grain indices
    can never be 0 — raising the level always lands on hard."""
    soft_level = solve([*HARD_PICKS, meeting_end_time_cost])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([*HARD_PICKS, MeetingEndTimeCost(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "room_conflict_costs"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ms.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, hard=None, exponent=1)."""
    instances = (
        overtime_cost,
        room_capacity_cost,
        day_crossing_cost,
        meeting_end_time_cost,
        larger_room_capacity_cost,
        room_conflict_cost,
        required_attendance_conflict_cost,
        required_and_preferred_attendance_conflict_cost,
        preferred_attendance_conflict_cost,
        consecutive_meetings_cost,
        meeting_overlap_cost,
        room_change_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in instances] == ms.list_constraints()


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = ms.constraint_info()
    assert [row.name for row in rows] == ms.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),  # overtime_cost
        ("hard", 1.0, True, False, True),  # room_capacity_cost
        ("hard", 1.0, True, False, True),  # day_crossing_cost
        ("soft", 1.0, True, False, True),  # meeting_end_time_cost
        ("soft", 1.0, True, False, True),  # larger_room_capacity_cost
        ("hard", 1.0, True, False, True),  # room_conflict_cost
        ("hard", 1.0, True, False, True),  # required_attendance_conflict_cost
        ("medium", 1.0, True, False, True),  # required_and_preferred_attendance_conflict_cost
        ("medium", 1.0, True, False, True),  # preferred_attendance_conflict_cost
        ("soft", 100.0, True, False, True),  # consecutive_meetings_cost
        ("soft", 10.0, True, False, True),  # meeting_overlap_cost
        ("soft", 1.0, True, False, True),  # room_change_cost
    ]


def test_constraint_info_rows_are_plain_values():
    row = ms.constraint_info()[0]
    assert isinstance(row, ms.ConstraintInfo)
    assert row._asdict() == {
        "name": "overtime_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='overtime_cost'")
    assert pickle.loads(pickle.dumps(ms.constraint_info())) == ms.constraint_info()
    assert ms.ConstraintInfo.__module__ == ms._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint,
    and every entry carries the domain's medium fold (1e6 into soft)."""
    raw = ms.constraint_costs(build_schedule())
    unit_names = ms.list_constraints()[:5]
    for row in ms.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name not in unit_names)
        assert (entry["name"], entry["label"]) == (row.name, "")  # one instance per name, no label
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": ms.MEDIUM_SCALE}
    assert ms.MEDIUM_SCALE == 1e6
    assert sorted(row.name for row in ms.constraint_info() if row.default_level == "medium") == sorted(MEDIUM)


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights (medium at 1, in medium units)."""
    explicit = [type(spec)(weight=row.default_weight) for spec, row in zip(get_constraints(), ms.constraint_info())]
    assert [spec.weight for spec in explicit] == [1, 1, 1, 1, 1, 1, 1, 1, 1, 100, 10, 1]
    schedule = _solved()
    raw = ms.constraint_costs(schedule)
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (schedule.score.hard, pytest.approx(schedule.score.soft))


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
        MeetingOverlapCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "meeting_overlap_cost"
        weight = -2.0
        hard = None
        exponent = 1.0

    class Flat:
        name = "meeting_overlap_cost"
        weight = None
        hard = None
        exponent = 0.0

    for pick, message in ((Negative(), "weight must be finite and >= 0"), (Flat(), "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            ms.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    pick = SimpleNamespace(name="meeting_overlap_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'meeting_overlap_cost': {re.escape(message)}"):
        ms.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        MeetingOverlapCost(**knobs)


OLD_NAMES = (
    "room_conflict",
    "avoid_overtime",
    "required_attendance_conflict",
    "required_room_capacity",
    "start_and_end_on_same_day",
    "required_and_preferred_attendance_conflict",
    "preferred_attendance_conflict",
    "do_meetings_as_soon_as_possible",
    "one_break_between_consecutive_meetings",
    "overlapping_meetings",
    "assign_larger_rooms_first",
    "room_stability",
)


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        ms.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(ms.constraint_costs(build_schedule()), [pick])
    assert old not in ms.list_constraints()


# --- The medium level -------------------------------------------------------------


def _attendance_clash():
    """Person 0 required at [0,4) in room 0 and preferred at [2,6) in room 1; person 1
    preferred at both: 2 required-and-preferred and 2 preferred person-grains."""
    people = [ms.Person(str(i)) for i in range(2)]
    grains = [ms.TimeGrain(str(g), g, 100, 8 * 60 + 15 * g) for g in range(12)]
    rooms = [ms.Room("R0", capacity=5), ms.Room("R1", capacity=5)]
    meetings = [
        ms.Meeting("0", duration_in_grains=4, required_attendees=[0], preferred_attendees=[1]),
        ms.Meeting("1", duration_in_grains=4, preferred_attendees=[0, 1]),
    ]
    assignments = [
        ms.MeetingAssignment("0", 0, grain_room=0 * 2 + 0, pinned=True),
        ms.MeetingAssignment("1", 1, grain_room=2 * 2 + 1, pinned=True),
    ]
    return ms.MeetingSchedule(people, grains, rooms, meetings, assignments)


def _engine_score(constraints):
    """The engine's score of the (fully pinned) attendance clash under `constraints`."""
    schedule = _attendance_clash()
    ms.solve(schedule, time_limit_seconds=0.1, random_seed=1, constraints=constraints)
    return schedule.score.hard, schedule.score.soft


def test_the_medium_level_folds_into_soft_at_1e6():
    schedule = _attendance_clash()
    raw = ms.constraint_costs(schedule)
    assert [sum(raw[name]["costs"]) for name in MEDIUM] == [2.0, 2.0]
    soft = sum(entry["weight"] * sum(entry["costs"]) for entry in raw.values() if entry["level"] == "soft")
    engine = ms.calculate_score(schedule)
    assert (engine.hard, engine.soft) == (0, -(4 * 1e6 + soft))
    assert score(raw) == (engine.hard, engine.soft)
    assert _engine_score(None) == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    ("spec", "expected"),
    [
        (PreferredAttendanceConflictCost(), (0, -2 * 1e6)),  # medium, folded
        (PreferredAttendanceConflictCost(hard=True), (-2, 0.0)),  # one hard unit per person-grain
        (PreferredAttendanceConflictCost(hard=False), (0, -2.0)),  # one soft unit, outside the fold
        (PreferredAttendanceConflictCost(weight=3), (0, -6 * 1e6)),  # the weight stays in medium units
        (RequiredAndPreferredAttendanceConflictCost(hard=False, weight=5), (0, -10.0)),
    ],
)
def test_medium_knobs_score_the_same_in_python_and_in_the_engine(spec, expected):
    """hard=True / hard=False move a medium constraint to hard / soft at one unit per raw
    person-grain; an explicit weight stays in medium units. Both paths agree."""
    raw = ms.constraint_costs(_attendance_clash())
    assert score(raw, [spec]) == expected
    assert _engine_score([spec]) == expected


def test_demoting_both_mediums_scores_like_the_engine_on_a_solve():
    """hard=False on the medium pair: the solve's own score equals the pure-Python rescoring."""
    selection = [spec if spec.name not in MEDIUM else type(spec)(hard=False) for spec in get_constraints()]
    schedule = _solved(selection)
    python = score(ms.constraint_costs(schedule), selection)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _solved(constraints=None):
    schedule = build_schedule()
    ms.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return schedule


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = _solved()
    return ms.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    The engine reports HardSoftScore with the medium level folded into soft; `score()`
    folds the same way (the `medium_fold` of every entry). This assertion pins down, in
    this package, that `score()` reproduces the engine's applyRaw arithmetic (weights,
    shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = ms.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [room_conflict_cost],
        [*HARD_PICKS, MeetingOverlapCost(weight=60), meeting_end_time_cost],
        [RoomConflictCost(hard=False), *HARD_PICKS[:3], meeting_overlap_cost],
        [*HARD_PICKS, MeetingOverlapCost(exponent=2.0), MeetingEndTimeCost(exponent=2.0)],
        [*HARD_PICKS, PreferredAttendanceConflictCost(hard=True), MeetingEndTimeCost(hard=True, weight=0.5)],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per unit (a (grain, room) cell) for the per-meeting constraints
    and on the total for the pair constraints, so reproducing it needs the per-unit
    raw costs — totals alone are not enough."""
    schedule = _solved(specs)
    python = score(ms.constraint_costs(schedule), specs)
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
    base = score(raw, [meeting_overlap_cost])
    ten = score(raw, [MeetingOverlapCost(weight=10)])
    default_weight = raw[meeting_overlap_cost.name]["weight"]
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
        ms.solve(
            build_schedule(),
            time_limit_seconds=0.2,
            constraints=[room_conflict_cost, MeetingOverlapCost(weight=10), meeting_overlap_cost],
        )
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([room_conflict_cost, room_conflict_cost])
