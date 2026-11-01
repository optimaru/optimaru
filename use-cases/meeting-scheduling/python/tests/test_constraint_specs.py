"""meeting_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the
catalog (`constraint_info()`), the knobs, the medium level and repeated picks.
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
    OvertimeCost,
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

    hard_level = solve([*HARD_PICKS, MeetingEndTimeCost(level="hard")])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "room_conflict_costs"  # trailing s
        weight = None
        level = None
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
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
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
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
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
        level = None
        exponent = 1.0

    class Flat:
        name = "meeting_overlap_cost"
        weight = None
        level = None
        exponent = 0.0

    class Unbounded:
        name = "meeting_overlap_cost"
        weight = None
        level = None
        exponent = 1.0
        offset = float("inf")

    for pick, message in (
        (Negative(), "weight must be finite and >= 0"),
        (Flat(), "exponent must be finite and > 0"),
        (Unbounded(), "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            ms.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        MeetingOverlapCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
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
    pick = SimpleNamespace(name="meeting_overlap_cost", weight=None, level=None, exponent=1.0)
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
    pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
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
        (PreferredAttendanceConflictCost(level="hard"), (-2, 0.0)),  # one hard unit per person-grain
        (PreferredAttendanceConflictCost(level="soft"), (0, -2.0)),  # one soft unit, outside the fold
        (PreferredAttendanceConflictCost(weight=3), (0, -6 * 1e6)),  # the weight stays in medium units
        (RequiredAndPreferredAttendanceConflictCost(level="soft", weight=5), (0, -10.0)),
    ],
)
def test_medium_knobs_score_the_same_in_python_and_in_the_engine(spec, expected):
    """level="hard" / level="soft" move a medium constraint to hard / soft at one unit per raw
    person-grain; an explicit weight stays in medium units. Both paths agree."""
    raw = ms.constraint_costs(_attendance_clash())
    assert score(raw, [spec]) == expected
    assert _engine_score([spec]) == expected


def test_demoting_both_mediums_scores_like_the_engine_on_a_solve():
    """level="soft" on the medium pair: the solve's own score equals the pure-Python rescoring."""
    selection = [spec if spec.name not in MEDIUM else type(spec)(level="soft") for spec in get_constraints()]
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
        [RoomConflictCost(level="soft"), *HARD_PICKS[:3], meeting_overlap_cost],
        [*HARD_PICKS, MeetingOverlapCost(exponent=2.0), MeetingEndTimeCost(exponent=2.0)],
        [*HARD_PICKS, PreferredAttendanceConflictCost(level="hard"), MeetingEndTimeCost(level="hard", weight=0.5)],
        [*HARD_PICKS, MeetingEndTimeCost(), MeetingEndTimeCost(weight=0.5, exponent=2.0), meeting_overlap_cost],
        [*HARD_PICKS, MeetingEndTimeCost(offset=3.0), MeetingEndTimeCost(offset=-1.0, clip=False, weight=0.25)],
        [
            *HARD_PICKS,
            MeetingOverlapCost(offset=4.0, clip=False),
            PreferredAttendanceConflictCost(),
            PreferredAttendanceConflictCost(offset=1.0, clip=False, weight=2),
            RoomConflictCost(offset=-2.0),
        ],
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


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [RoomConflictCost(), MeetingOverlapCost(weight=10), MeetingOverlapCost()]
    assert _engine_score(picks) == (0, -(10 + 10) * 2)  # overlap 2, default weight 10 twice
    assert get_constraints([RoomConflictCost(), RoomConflictCost()]) == [RoomConflictCost(), RoomConflictCost()]


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------
#
# The attendance clash (12 grains x 2 rooms = 24 cells): meeting_end_time_cost reads 3 in
# cell 0 and 5 in cell 5 (soft, weight 1, a unit constraint); meeting_overlap_cost 2 (soft,
# weight 10), room_change_cost 2 (soft, weight 1), the two mediums 2 each (global); every
# hard cost is 0.


REPEAT = [MeetingEndTimeCost(), MeetingEndTimeCost(weight=0.5, exponent=2)]


def test_the_attendance_clash_raw_costs():
    raw = ms.constraint_costs(_attendance_clash())
    nonzero = {key: {i: c for i, c in enumerate(entry["costs"]) if c} for key, entry in raw.items()}
    assert {key: cells for key, cells in nonzero.items() if cells} == {
        "meeting_end_time_cost": {0: 3.0, 5: 5.0},
        "required_and_preferred_attendance_conflict_cost": {0: 2.0},
        "preferred_attendance_conflict_cost": {0: 2.0},
        "meeting_overlap_cost": {0: 2.0},
        "room_change_cost": {0: 2.0},
    }
    assert len(raw["meeting_end_time_cost"]["costs"]) == 24


def test_a_repeated_pick_by_hand():
    """end time x 1 plus 0.5 x end time^2 per cell: (3 + 5) + 0.5 x (9 + 25) = 25."""
    raw = ms.constraint_costs(_attendance_clash(), REPEAT)
    assert list(raw) == ["meeting_end_time_cost", "meeting_end_time_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "meeting_end_time_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -((3 + 5) + 0.5 * (3**2 + 5**2)))
    assert expected == (0, -25.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(ms.constraint_costs(_attendance_clash()), REPEAT) == expected
    assert _engine_score(REPEAT) == expected


def test_identical_picks_count_twice():
    picks = [MeetingOverlapCost(), MeetingOverlapCost()]
    assert score(ms.constraint_costs(_attendance_clash()), picks) == (0, -40.0)
    assert _engine_score(picks) == (0, -40.0)
    assert list(ms.constraint_costs(_attendance_clash(), picks)) == ["meeting_overlap_cost", "meeting_overlap_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        MeetingOverlapCost(weight=1),
        RoomConflictCost(),
        MeetingOverlapCost(weight=2),
        MeetingEndTimeCost(),
        MeetingOverlapCost(weight=3),
    ]
    raw = ms.constraint_costs(_attendance_clash(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "meeting_end_time_cost": 1.0,
        "room_conflict_cost": 1.0,
        "meeting_overlap_cost": 1.0,
        "meeting_overlap_cost#2": 2.0,
        "meeting_overlap_cost#3": 3.0,
    }
    assert score(raw, picks) == (0, -(6 * 2 + 8))
    assert _engine_score(picks) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # A unit constraint, shaped per cell: 3 and 5 in two cells, 0 in the other 22.
        (MeetingEndTimeCost(offset=4), (0, -1.0)),
        (MeetingEndTimeCost(offset=4, clip=False), (0, -((3 - 4) + (5 - 4) + 22 * (0 - 4)))),  # every cell
        (MeetingEndTimeCost(offset=2, exponent=2), (0, -(1**2 + 3**2))),
        (MeetingEndTimeCost(offset=4, exponent=3, clip=False), (0, -(-1 + 1 + 22 * -(4**3)))),
        (MeetingEndTimeCost(offset=-1), (0, -(4 + 6))),  # a negative offset adds to every non-zero cell
        (MeetingEndTimeCost(offset=-1, clip=False), (0, -(4 + 6 + 22 * 1))),  # ...or to every cell
        # A hard unit constraint at 0 everywhere, literal: rounded per cell, 3 x -0.5 -> -2 x 24.
        (OvertimeCost(offset=0.5, clip=False, weight=3), (48, 0.0)),
        (OvertimeCost(offset=0.5), (0, 0.0)),
        # Global constraints, the one total shaped once.
        (MeetingOverlapCost(offset=1), (0, -10 * 1)),
        (MeetingOverlapCost(offset=5, clip=False), (0, -10 * (2 - 5))),
        (RoomConflictCost(offset=3), (0, 0.0)),
        (RoomConflictCost(offset=3, clip=False), (3, 0.0)),  # a hard reward below 3
        # Medium (folded into soft at 1e6).
        (PreferredAttendanceConflictCost(offset=1), (0, -1e6)),
        (PreferredAttendanceConflictCost(offset=3, clip=False), (0, 1e6)),
        (PreferredAttendanceConflictCost(offset=-1, level="hard"), (-3, 0.0)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(ms.constraint_costs(_attendance_clash()), [pick]) == expected
    raw = ms.constraint_costs(_attendance_clash(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # no scoped constraint here: every cell is evaluated
    assert score(raw, [pick]) == expected
    assert _engine_score([pick]) == expected


def test_a_repeated_and_a_shifted_medium_pick():
    """Both picks stay on the medium level (weight in medium units) and fold into soft."""
    repeat = [PreferredAttendanceConflictCost(), PreferredAttendanceConflictCost(weight=2)]
    shifted = [RequiredAndPreferredAttendanceConflictCost(offset=1.5, clip=False, weight=4)]
    raw = ms.constraint_costs(_attendance_clash(), repeat)
    assert [entry["level"] for entry in raw.values()] == ["medium", "medium"]
    assert score(raw, repeat) == _engine_score(repeat) == (0, -(2 + 4) * 1e6)
    assert score(ms.constraint_costs(_attendance_clash()), shifted) == _engine_score(shifted) == (0, -4 * 0.5 * 1e6)


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*HARD_PICKS, MeetingEndTimeCost(exponent=2), meeting_overlap_cost, preferred_attendance_conflict_cost]
    shifted = [
        *HARD_PICKS,
        MeetingEndTimeCost(exponent=2, offset=0.0, clip=False),
        MeetingOverlapCost(offset=0),
        PreferredAttendanceConflictCost(offset=0.0, clip=False),
    ]
    raw = ms.constraint_costs(_attendance_clash())
    assert score(raw, shifted) == score(raw, plain) == _engine_score(plain) == _engine_score(shifted)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*HARD_PICKS, *REPEAT, meeting_overlap_cost, preferred_attendance_conflict_cost],
        [
            *HARD_PICKS,
            OvertimeCost(offset=1, clip=False, weight=2),
            MeetingEndTimeCost(),
            MeetingEndTimeCost(offset=6, exponent=2, weight=0.25, clip=False),
            MeetingOverlapCost(),
            MeetingOverlapCost(offset=-3),
            PreferredAttendanceConflictCost(),
            PreferredAttendanceConflictCost(offset=2, clip=False, weight=0.5),
            room_change_cost,
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    schedule = build_schedule()
    config = ms.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    ms.Solver(config).solve_with_constraints(schedule, picks)
    raw = ms.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(ms.constraint_costs(schedule), picks) == python  # the default costs score the repeats too
