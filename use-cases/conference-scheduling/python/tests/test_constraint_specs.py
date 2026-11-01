"""conference_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the catalog
(`constraint_info()`), the five knobs and repeated picks.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import conference_scheduling as cs
from conference_scheduling import constraints
from conference_scheduling.constraints import (
    AudienceTypeDiversityCost,
    Constraint,
    ContentConflictCost,
    LanguageDiversityCost,
    Level,
    RoomConflictCost,
    SpeakerMakespanCost,
    content_conflict_cost,
    get_constraints,
    room_conflict_cost,
    room_unavailability_cost,
    score,
    speaker_conflict_cost,
    talk_type_incompatibility_cost,
    theme_track_conflict_cost,
)

HARD_PICKS = [
    talk_type_incompatibility_cost,
    room_unavailability_cost,
    room_conflict_cost,
    speaker_conflict_cost,
]

#: The catalog, in list_constraints() order: the C++ component defaults.
EXPECTED_NAMES = [
    "talk_type_incompatibility_cost",
    "room_unavailability_cost",
    "room_conflict_cost",
    "speaker_unavailability_cost",
    "speaker_conflict_cost",
    "talk_prerequisite_talks_cost",
    "talk_mutually_exclusive_talks_tags_cost",
    "consecutive_talks_pause_cost",
    "crowd_control_risk_cost",
    "speaker_required_timeslot_tags_cost",
    "speaker_prohibited_timeslot_tags_cost",
    "talk_required_timeslot_tags_cost",
    "talk_prohibited_timeslot_tags_cost",
    "speaker_required_room_tags_cost",
    "speaker_prohibited_room_tags_cost",
    "talk_required_room_tags_cost",
    "talk_prohibited_room_tags_cost",
    "theme_track_conflict_cost",
    "theme_track_room_change_cost",
    "sector_conflict_cost",
    "audience_type_theme_track_conflict_cost",
    "content_audience_level_flow_violation_cost",
    "content_conflict_cost",
    "different_day_talks_cost",
    "popular_talks_room_size_cost",
    "speaker_preferred_timeslot_tags_cost",
    "speaker_undesired_timeslot_tags_cost",
    "talk_preferred_timeslot_tags_cost",
    "talk_undesired_timeslot_tags_cost",
    "speaker_preferred_room_tags_cost",
    "speaker_undesired_room_tags_cost",
    "talk_preferred_room_tags_cost",
    "talk_undesired_room_tags_cost",
    "speaker_makespan_cost",
    "audience_type_diversity_cost",
    "audience_level_diversity_cost",
    "language_diversity_cost",
]

#: (name, default_level, default_weight, level_flippable, reward, default_enabled)
EXPECTED_INFO = [
    ("talk_type_incompatibility_cost", "hard", 100000.0, True, False, True),
    ("room_unavailability_cost", "hard", 100000.0, True, False, True),
    ("room_conflict_cost", "hard", 1000.0, True, False, True),
    ("speaker_unavailability_cost", "hard", 100.0, True, False, True),
    ("speaker_conflict_cost", "hard", 10.0, True, False, True),
    ("talk_prerequisite_talks_cost", "hard", 10.0, True, False, True),
    ("talk_mutually_exclusive_talks_tags_cost", "hard", 1.0, True, False, True),
    ("consecutive_talks_pause_cost", "hard", 1.0, True, False, True),
    ("crowd_control_risk_cost", "hard", 1.0, True, False, True),
    ("speaker_required_timeslot_tags_cost", "hard", 1.0, True, False, True),
    ("speaker_prohibited_timeslot_tags_cost", "hard", 1.0, True, False, True),
    ("talk_required_timeslot_tags_cost", "hard", 1.0, True, False, True),
    ("talk_prohibited_timeslot_tags_cost", "hard", 1.0, True, False, True),
    ("speaker_required_room_tags_cost", "hard", 1.0, True, False, True),
    ("speaker_prohibited_room_tags_cost", "hard", 1.0, True, False, True),
    ("talk_required_room_tags_cost", "hard", 1.0, True, False, True),
    ("talk_prohibited_room_tags_cost", "hard", 1.0, True, False, True),
    ("theme_track_conflict_cost", "soft", 10.0, True, False, True),
    ("theme_track_room_change_cost", "soft", 10.0, True, False, True),
    ("sector_conflict_cost", "soft", 10.0, True, False, True),
    ("audience_type_theme_track_conflict_cost", "soft", 1.0, True, False, True),
    ("content_audience_level_flow_violation_cost", "soft", 10.0, True, False, True),
    ("content_conflict_cost", "soft", 100.0, True, False, True),
    ("different_day_talks_cost", "soft", 10.0, True, False, True),
    ("popular_talks_room_size_cost", "soft", 10.0, True, False, True),
    ("speaker_preferred_timeslot_tags_cost", "soft", 20.0, True, False, True),
    ("speaker_undesired_timeslot_tags_cost", "soft", 20.0, True, False, True),
    ("talk_preferred_timeslot_tags_cost", "soft", 20.0, True, False, True),
    ("talk_undesired_timeslot_tags_cost", "soft", 20.0, True, False, True),
    ("speaker_preferred_room_tags_cost", "soft", 20.0, True, False, True),
    ("speaker_undesired_room_tags_cost", "soft", 20.0, True, False, True),
    ("talk_preferred_room_tags_cost", "soft", 20.0, True, False, True),
    ("talk_undesired_room_tags_cost", "soft", 20.0, True, False, True),
    ("speaker_makespan_cost", "soft", 20.0, True, False, True),
    ("audience_type_diversity_cost", "soft", 1.0, False, True, True),
    ("audience_level_diversity_cost", "soft", 1.0, False, True, True),
    ("language_diversity_cost", "soft", 10.0, False, True, True),
]

REWARDS = ["audience_type_diversity_cost", "audience_level_diversity_cost", "language_diversity_cost"]


def build_schedule():
    return cs.demo_conference()


def solve(constraints=None):
    schedule = build_schedule()
    cs.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows, in the core's order."""
    available = cs.list_constraints()
    assert len(available) == 37
    assert available == EXPECTED_NAMES
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    """The default list must match the core's default assembly in names and order."""
    assert [spec.name for spec in get_constraints()] == cs.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    schedule = build_schedule()
    cs.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=get_constraints())
    recomputed = cs.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    baseline = solve()
    assert baseline.soft != 0

    hard_only = solve(HARD_PICKS)
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the content-conflict weight grows soft only; hard stays put."""
    base = solve([*HARD_PICKS, content_conflict_cost])
    heavier = solve([*HARD_PICKS, ContentConflictCost(weight=100_000)])
    assert heavier.hard == base.hard
    assert heavier.soft <= base.soft  # penalty, so equal or more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level."""
    picks = [talk_type_incompatibility_cost, room_unavailability_cost]

    soft_level = solve([*picks, theme_track_conflict_cost, content_conflict_cost])
    assert soft_level.hard == 0

    hard_level = solve(
        [
            *picks,
            type(theme_track_conflict_cost)(level="hard"),
            type(content_conflict_cost)(level="hard"),
        ]
    )
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
        cs.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_naming_a_constraint_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    picks = [RoomConflictCost(), RoomConflictCost(weight=10)]
    solved = _pinned_solve(_three_talks(), picks)
    assert solved.score.hard == -(1000 + 10) * 60  # one 60-minute room conflict, both picks
    assert get_constraints([RoomConflictCost(), RoomConflictCost()]) == [RoomConflictCost(), RoomConflictCost()]


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent=1)."""
    defaults = get_constraints()
    assert [spec.name for spec in defaults] == EXPECTED_NAMES
    for spec in defaults:
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)
        assert getattr(constraints, spec.name) is spec  # instance name == registry name
        assert type(spec).__name__ in constraints.__all__ and spec.name in constraints.__all__


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"
        assert not hasattr(cls, "default_weight") and not hasattr(cls, "default_level")


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = cs.constraint_info()
    assert [row.name for row in rows] == cs.list_constraints()
    assert [tuple(row) for row in rows] == EXPECTED_INFO
    assert [row.name for row in rows if row.reward] == REWARDS
    assert sum(row.default_level == "hard" for row in rows) == 17


def test_constraint_info_rows_are_plain_values():
    row = cs.constraint_info()[0]
    assert isinstance(row, cs.ConstraintInfo)
    assert row._asdict() == {
        "name": "talk_type_incompatibility_cost",
        "default_level": "hard",
        "default_weight": 100000.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='talk_type_incompatibility_cost'")
    assert pickle.loads(pickle.dumps(cs.constraint_info())) == cs.constraint_info()
    assert cs.ConstraintInfo.__module__ == cs._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults for every constraint."""
    raw = cs.constraint_costs(build_schedule())
    assert list(raw) == cs.list_constraints()
    for row in cs.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (row.level_flippable, row.reward)
        assert entry["global"] is True and len(entry["costs"]) == 1  # one total per constraint
        assert (entry["name"], entry["label"]) == (row.name, "")
        assert "medium_fold" not in entry  # no medium level in this domain


def test_the_breakdown_api_is_gone():
    """0.2.0: constraint_costs() is the only per-constraint surface."""
    assert not hasattr(cs, "compute_breakdown")
    assert not hasattr(cs, "Breakdown")
    assert not hasattr(cs._core, "compute_breakdown")


# --- The knobs ------------------------------------------------------------------


def _solved(constraints=None):
    schedule = build_schedule()
    cs.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return schedule


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights of the Java provider."""
    explicit = [
        constraints.TalkTypeIncompatibilityCost(weight=100000),
        constraints.RoomUnavailabilityCost(weight=100000),
        constraints.RoomConflictCost(weight=1000),
        constraints.SpeakerUnavailabilityCost(weight=100),
        constraints.SpeakerConflictCost(weight=10),
        constraints.TalkPrerequisiteTalksCost(weight=10),
        constraints.TalkMutuallyExclusiveTalksTagsCost(weight=1),
        constraints.ConsecutiveTalksPauseCost(weight=1),
        constraints.CrowdControlRiskCost(weight=1),
        constraints.SpeakerRequiredTimeslotTagsCost(weight=1),
        constraints.SpeakerProhibitedTimeslotTagsCost(weight=1),
        constraints.TalkRequiredTimeslotTagsCost(weight=1),
        constraints.TalkProhibitedTimeslotTagsCost(weight=1),
        constraints.SpeakerRequiredRoomTagsCost(weight=1),
        constraints.SpeakerProhibitedRoomTagsCost(weight=1),
        constraints.TalkRequiredRoomTagsCost(weight=1),
        constraints.TalkProhibitedRoomTagsCost(weight=1),
        constraints.ThemeTrackConflictCost(weight=10),
        constraints.ThemeTrackRoomChangeCost(weight=10),
        constraints.SectorConflictCost(weight=10),
        constraints.AudienceTypeThemeTrackConflictCost(weight=1),
        constraints.ContentAudienceLevelFlowViolationCost(weight=10),
        constraints.ContentConflictCost(weight=100),
        constraints.DifferentDayTalksCost(weight=10),
        constraints.PopularTalksRoomSizeCost(weight=10),
        constraints.SpeakerPreferredTimeslotTagsCost(weight=20),
        constraints.SpeakerUndesiredTimeslotTagsCost(weight=20),
        constraints.TalkPreferredTimeslotTagsCost(weight=20),
        constraints.TalkUndesiredTimeslotTagsCost(weight=20),
        constraints.SpeakerPreferredRoomTagsCost(weight=20),
        constraints.SpeakerUndesiredRoomTagsCost(weight=20),
        constraints.TalkPreferredRoomTagsCost(weight=20),
        constraints.TalkUndesiredRoomTagsCost(weight=20),
        constraints.SpeakerMakespanCost(weight=20),
        constraints.AudienceTypeDiversityCost(weight=1),
        constraints.AudienceLevelDiversityCost(weight=1),
        constraints.LanguageDiversityCost(weight=10),
    ]
    schedule = _solved()
    raw = cs.constraint_costs(schedule)
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
        ContentConflictCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""
    negative = SimpleNamespace(name="content_conflict_cost", weight=-2.0, level=None, exponent=1.0)
    flat = SimpleNamespace(name="content_conflict_cost", weight=None, level=None, exponent=0.0)
    unbounded = SimpleNamespace(
        name="content_conflict_cost", weight=None, level=None, exponent=1.0, offset=float("inf")
    )
    for pick, message in (
        (negative, "weight must be finite and >= 0"),
        (flat, "exponent must be finite and > 0"),
        (unbounded, "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            cs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        ContentConflictCost(offset=float("nan"))


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
    pick = SimpleNamespace(name="content_conflict_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'content_conflict_cost': {re.escape(message)}"):
        cs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        ContentConflictCost(**knobs)


@pytest.mark.parametrize("name", REWARDS)
def test_a_reward_is_never_hard(name):
    """The three *_diversity_cost rewards (raw <= 0) refuse level="hard" in the C++ path and in score()."""
    spec = type(getattr(constraints, name))(level="hard")
    with pytest.raises(ValueError, match="is a reward"):
        cs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[*HARD_PICKS, spec])
    duck = SimpleNamespace(name=name, weight=None, level="hard", exponent=1.0)
    with pytest.raises(ValueError, match="is a reward"):
        cs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[duck])
    with pytest.raises(ValueError, match="is a reward"):
        score(cs.constraint_costs(build_schedule()), [spec])


def test_a_reward_stays_soft_and_raises_the_score():
    """level="soft" re-states a reward's level (a no-op); re-weighting keeps the sign."""
    schedule = _solved()
    raw = cs.constraint_costs(schedule)
    reward = raw["audience_type_diversity_cost"]["costs"][0]
    assert reward < 0  # the demo's solution earns it
    assert score(raw, [AudienceTypeDiversityCost(level="soft")]) == score(raw, [AudienceTypeDiversityCost()])
    three = score(raw, [AudienceTypeDiversityCost(weight=3)])
    assert three.hard == 0 and three.soft == -3 * reward > 0
    assert score(raw, [LanguageDiversityCost(level="soft")]) == score(raw, [LanguageDiversityCost()])
    solved = _solved([*HARD_PICKS, AudienceTypeDiversityCost(level="soft", weight=2)])
    assert score(cs.constraint_costs(solved), [*HARD_PICKS, AudienceTypeDiversityCost(weight=2)]) == (
        solved.score.hard,
        pytest.approx(solved.score.soft),
    )


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    for old in [
        "talk_type_compatibility",
        "room_unavailable_timeslot",
        "room_conflict",
        "speaker_unavailable_timeslot",
        "speaker_conflict",
        "talk_prerequisite_talks",
        "talk_mutually_exclusive_talks_tags",
        "consecutive_talks_pause",
        "crowd_control",
        "speaker_required_timeslot_tags",
        "speaker_prohibited_timeslot_tags",
        "talk_required_timeslot_tags",
        "talk_prohibited_timeslot_tags",
        "speaker_required_room_tags",
        "speaker_prohibited_room_tags",
        "talk_required_room_tags",
        "talk_prohibited_room_tags",
        "theme_track_conflict",
        "theme_track_room_stability",
        "sector_conflict",
        "audience_type_theme_track_conflict",
        "content_audience_level_flow_violation",
        "content_conflict",
        "same_day_talks",
        "popular_talks",
        "speaker_preferred_timeslot_tags",
        "speaker_undesired_timeslot_tags",
        "talk_preferred_timeslot_tags",
        "talk_undesired_timeslot_tags",
        "speaker_preferred_room_tags",
        "speaker_undesired_room_tags",
        "talk_preferred_room_tags",
        "talk_undesired_room_tags",
        "speaker_makespan",
        "audience_type_diversity",
        "audience_level_diversity",
        "language_diversity",
    ]:
        assert old not in cs.list_constraints()
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            cs.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    cs.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return cs.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = cs.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        HARD_PICKS,
        [*HARD_PICKS, ContentConflictCost(weight=500), LanguageDiversityCost(weight=30)],
        [RoomConflictCost(level="soft"), talk_type_incompatibility_cost, content_conflict_cost],
        [*HARD_PICKS, ContentConflictCost(exponent=2.0), AudienceTypeDiversityCost(exponent=0.5)],
        [RoomConflictCost(exponent=1.5, weight=0.5), constraints.SpeakerMakespanCost(level="hard")],
        [*HARD_PICKS, ContentConflictCost(), ContentConflictCost(weight=0.5, exponent=2.0)],
        [*HARD_PICKS, ContentConflictCost(offset=30.0), ContentConflictCost(offset=-20.0, clip=False)],
        [
            *HARD_PICKS,
            RoomConflictCost(offset=10.0, clip=False, weight=3),
            LanguageDiversityCost(offset=50.0),
            LanguageDiversityCost(offset=-30.0, clip=False, weight=2),
        ],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    Every constraint is global, so exponent shapes the one total; the rewards keep
    their sign under shaping."""
    schedule = _solved(specs)
    python = score(cs.constraint_costs(schedule), specs)
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
    base = score(raw, [room_conflict_cost])
    ten = score(raw, [RoomConflictCost(weight=10)])
    default_weight = raw[room_conflict_cost.name]["weight"]
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


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _three_talks():
    """A hand-calculated schedule (the cpp/test makeBase rooms and slots), every talk pinned.

    A (speaker 0, content C) at s0 r0 and B (speaker 1, content C) at s1 r0: the
    same 60 minutes in the same room, so room_conflict_cost 60 and
    content_conflict_cost 60 (1 shared tag x 60). C (speaker 2, language "ko") at s0
    r1 shares s0 with A ("en"): language_diversity_cost -60. Every other raw cost is 0.
    """

    def slot(sid, start, end):
        return cs.Timeslot(id=sid, start_minute=start, end_minute=end, talk_types=[0])

    schedule = cs.ConferenceSchedule(
        name="hand",
        talk_types=["B"],
        timeslots=[
            slot("s0", 540, 600),
            slot("s1", 540, 600),
            slot("s2", 570, 630),
            slot("s3", 630, 690),
        ],
        rooms=[cs.Room("r0", capacity=100, talk_types=[0]), cs.Room("r1", capacity=200, talk_types=[0])],
        speakers=[cs.Speaker(f"k{k}") for k in range(3)],
        talks=[],
    )
    talks = []
    for code, speaker, slot_index, room, extra in (
        ("A", 0, 0, 0, {"content_tags": ["C"], "language": "en"}),
        ("B", 1, 1, 0, {"content_tags": ["C"], "language": "en"}),
        ("C", 2, 0, 1, {"language": "ko"}),
    ):
        talk = cs.Talk(code=code, speakers=[speaker], **extra)
        talk.slot_room = schedule.slot_room_of(slot_index, room)
        talk.pinned = True
        talks.append(talk)
    schedule.talks = talks
    return schedule


def _pinned_solve(schedule, picks):
    """A zero-budget solve of a fully pinned schedule: the engine's score of exactly that assignment."""
    config = cs.SolverConfig()
    config.spent_limit_seconds = 0
    before = schedule.assignment()
    cs.Solver(config).solve_with_constraints(schedule, picks)
    assert schedule.assignment() == before
    return schedule


def test_the_hand_schedule_raw_costs():
    raw = {key: entry["costs"] for key, entry in cs.constraint_costs(_three_talks()).items()}
    nonzero = {key: costs for key, costs in raw.items() if costs != [0.0]}
    assert nonzero == {
        "room_conflict_cost": [60.0],
        "content_conflict_cost": [60.0],
        "language_diversity_cost": [-60.0],
    }


REPEAT = [ContentConflictCost(), ContentConflictCost(weight=0.5, exponent=2)]


def test_a_repeated_pick_by_hand():
    """content conflict x 100 plus 0.5 x content conflict^2: 100 x 60 + 0.5 x 60^2 = 7800."""
    raw = cs.constraint_costs(_three_talks(), REPEAT)
    assert list(raw) == ["content_conflict_cost", "content_conflict_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [60.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (100.0, 1.0, 0.5, 2.0)
    assert first["name"] == second["name"] == "content_conflict_cost"
    assert second["default_weight"] == 100.0
    expected = (0, -(100 * 60 + 0.5 * 60**2))
    assert expected == (0, -7800.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw cost.
    assert score(cs.constraint_costs(_three_talks()), REPEAT) == expected
    engine = _pinned_solve(_three_talks(), REPEAT)
    assert (engine.score.hard, engine.score.soft) == expected


def test_identical_picks_count_twice():
    picks = [RoomConflictCost(), RoomConflictCost()]
    assert score(cs.constraint_costs(_three_talks()), picks) == (-120000, 0.0)
    assert _pinned_solve(_three_talks(), picks).score.hard == -120000
    assert list(cs.constraint_costs(_three_talks(), picks)) == ["room_conflict_cost", "room_conflict_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        ContentConflictCost(weight=1),
        RoomConflictCost(),
        ContentConflictCost(weight=2),
        LanguageDiversityCost(),
        ContentConflictCost(weight=3),
    ]
    raw = cs.constraint_costs(_three_talks(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "room_conflict_cost": 1000.0,
        "content_conflict_cost": 1.0,
        "content_conflict_cost#2": 2.0,
        "content_conflict_cost#3": 3.0,
        "language_diversity_cost": 10.0,
    }
    assert score(raw, picks) == (-60000, -(6 * 60 - 10 * 60))
    engine = _pinned_solve(_three_talks(), picks)
    assert (engine.score.hard, engine.score.soft) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Every constraint is global: the offset shapes the one total (content 60, weight 100).
        (ContentConflictCost(offset=20), (0, -100 * 40)),
        (ContentConflictCost(offset=100), (0, 0.0)),
        (ContentConflictCost(offset=100, clip=False), (0, -100 * (60 - 100))),  # a reward below 100
        (ContentConflictCost(offset=10, exponent=2), (0, -100 * 50**2)),
        (ContentConflictCost(offset=70, exponent=3, clip=False), (0, -100 * -(10**3))),
        (ContentConflictCost(offset=-10), (0, -100 * 70)),  # a negative offset adds to a non-zero total
        # A total of 0 (speaker_makespan_cost, weight 20): the literal formula charges it.
        (SpeakerMakespanCost(offset=5), (0, 0.0)),
        (SpeakerMakespanCost(offset=5, clip=False), (0, -20 * -5)),
        (SpeakerMakespanCost(offset=-10), (0, 0.0)),  # the threshold keeps a 0 at 0
        (SpeakerMakespanCost(offset=-10, clip=False), (0, -20 * 10)),
        # Hard (room conflict 60, weight 1000), rounded once (the one total).
        (RoomConflictCost(offset=10), (-1000 * 50, 0.0)),
        (RoomConflictCost(offset=100, clip=False), (40000, 0.0)),  # a hard reward below 100
        (RoomConflictCost(offset=59.5, clip=False, weight=3), (-2, 0.0)),  # 3 x 0.5 = 1.5 -> 2
        # The reward (language diversity -60, weight 10): a threshold rewards only beyond the
        # offset, a literal one shifts it further.
        (LanguageDiversityCost(offset=20), (0, 10 * 40)),
        (LanguageDiversityCost(offset=20, clip=False), (0, 10 * 80)),
        (LanguageDiversityCost(offset=-20, clip=False), (0, 10 * 40)),
        (LanguageDiversityCost(offset=-20), (0, 10 * 80)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(cs.constraint_costs(_three_talks()), [pick]) == expected
    raw = cs.constraint_costs(_three_talks(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # every constraint here is global
    assert score(raw, [pick]) == expected
    engine = _pinned_solve(_three_talks(), [pick])
    assert (engine.score.hard, engine.score.soft) == expected


# Each pick carries its own level: room conflict 60 (hard, weight 1000), content conflict
# 60 (soft, weight 100). The second room pick is soft at weight 2 beyond 10 minutes, the
# first content pick hard and literal beyond 50 at weight 3.
LEVELS = [
    RoomConflictCost(),
    RoomConflictCost(level=Level.SOFT, weight=2, offset=10),
    ContentConflictCost(level="hard", weight=3, offset=50, clip=False),
    ContentConflictCost(),
]


def test_each_repeat_carries_its_own_level():
    """hard: 1000 x 60 + 3 x (60 - 50) = 60030; soft: 2 x (60 - 10) + 100 x 60 = 6100."""
    raw = cs.constraint_costs(_three_talks(), LEVELS)
    assert {key: entry["level"] for key, entry in raw.items()} == {
        "room_conflict_cost": "hard",
        "room_conflict_cost#2": "soft",
        "content_conflict_cost": "hard",
        "content_conflict_cost#2": "soft",
    }
    expected = (-(1000 * 60 + 3 * (60 - 50)), -(2 * (60 - 10) + 100 * 60))
    assert expected == (-60030, -6100.0)
    assert score(raw, LEVELS) == expected
    assert score(cs.constraint_costs(_three_talks()), LEVELS) == expected
    engine = _pinned_solve(_three_talks(), LEVELS)
    assert (engine.score.hard, engine.score.soft) == expected


@pytest.mark.parametrize("level", [Level.MEDIUM, "medium"])
def test_medium_is_refused_on_every_path(level):
    """Conference scheduling has no medium level: a pick asking for it raises before scoring,
    on the engine path and in score(), also as the second pick of a name."""
    for picks in ([ContentConflictCost(level=level)], [RoomConflictCost(), RoomConflictCost(level=level)]):
        with pytest.raises(ValueError, match="this use case has no medium level"):
            _pinned_solve(_three_talks(), picks)
        with pytest.raises(ValueError, match="this use case has no medium level"):
            cs.constraint_costs(_three_talks(), picks)
        with pytest.raises(ValueError, match="this use case has no medium level"):
            score(cs.constraint_costs(_three_talks()), picks)


def test_the_hard_knob_is_gone():
    """0.3.0 replaced hard= by level=; the old keyword is a TypeError, not a silent default."""
    with pytest.raises(TypeError, match="hard"):
        ContentConflictCost(hard=True)
    assert ContentConflictCost(level="hard") == ContentConflictCost(level=Level.HARD)


def test_a_second_reward_pick_still_refuses_hard():
    with pytest.raises(ValueError, match="is a reward"):
        _pinned_solve(_three_talks(), [LanguageDiversityCost(), LanguageDiversityCost(level="hard")])
    with pytest.raises(ValueError, match="is a reward"):
        score(cs.constraint_costs(_three_talks()), [LanguageDiversityCost(), LanguageDiversityCost(level="hard")])


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*HARD_PICKS, ContentConflictCost(exponent=2), LanguageDiversityCost()]
    shifted = [
        *HARD_PICKS[:2],
        RoomConflictCost(offset=0.0, clip=False),
        HARD_PICKS[3],
        ContentConflictCost(exponent=2, offset=0),
        LanguageDiversityCost(offset=0.0, clip=False),
    ]
    schedule = _solved()
    raw = cs.constraint_costs(schedule)
    assert score(raw, shifted) == score(raw, plain)
    engines = []
    for picks in (plain, shifted):
        copy = build_schedule()
        copy.set_assignment(schedule.assignment())
        for talk in copy.talks:
            talk.pinned = True
        engines.append(_pinned_solve(copy, picks))
    assert (engines[0].score.hard, engines[0].score.soft) == (engines[1].score.hard, engines[1].score.soft)
    assert (engines[0].score.hard, engines[0].score.soft) == score(raw, plain)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "picks",
    [
        [*HARD_PICKS, *REPEAT, LanguageDiversityCost()],
        [
            *HARD_PICKS,
            RoomConflictCost(offset=30, clip=False, weight=2),
            ContentConflictCost(),
            ContentConflictCost(offset=120, exponent=2, weight=0.25, clip=False),
            constraints.SpeakerConflictCost(offset=-5),
            LanguageDiversityCost(),
            LanguageDiversityCost(offset=60, clip=False, weight=0.5),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    schedule = build_schedule()
    config = cs.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 3000
    config.spent_limit_seconds = 30
    cs.Solver(config).solve_with_constraints(schedule, picks)
    raw = cs.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(cs.constraint_costs(schedule), picks) == python  # the default costs score the repeats too
