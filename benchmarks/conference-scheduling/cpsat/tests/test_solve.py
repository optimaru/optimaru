from itertools import product

import pytest

import conference_scheduling as domain
from conference_scheduling.constraints import (
    AudienceTypeDiversityCost,
    ContentConflictCost,
    DifferentDayTalksCost,
    LanguageDiversityCost,
    Level,
    RoomConflictCost,
    SpeakerConflictCost,
    SpeakerMakespanCost,
    SpeakerUnavailabilityCost,
    get_constraints,
    score,
)
from conference_scheduling.models import (
    ConferenceScheduleModel,
    RoomModel,
    SpeakerModel,
    TalkModel,
    TimeslotModel,
)
from conference_scheduling_cpsat import catalog, solve
from conference_scheduling import calculate_score


def test_smoke_and_plugin_catalog():
    problem = ConferenceScheduleModel(
        name="Small",
        talk_types=["Talk"],
        timeslots=[
            TimeslotModel(id="S", start_minute=0, end_minute=60, talk_types=[0])
        ],
        rooms=[RoomModel(id="R", capacity=10, talk_types=[0])],
        talks=[TalkModel(code="T", talk_type=0)],
    )
    result = solve(
        problem, time_limit_seconds=2, constraints=["talk_type_incompatibility_cost"]
    )
    assert result.talks[0].slot_room == 0
    assert result.score_hard == 0
    assert result.score_soft == 0
    assert len(catalog()) == 37
    shared = calculate_score(result.to_core())
    assert (result.score_hard, result.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> ConferenceScheduleModel:
    return ConferenceScheduleModel(
        name="Parity",
        talk_types=["Talk", "Workshop"],
        timeslots=[
            TimeslotModel(
                id="S0",
                start_minute=0,
                end_minute=60,
                talk_types=[0],
                tags=["morning", "projector"],
            ),
            TimeslotModel(
                id="S1",
                start_minute=30,
                end_minute=90,
                talk_types=[0, 1],
                tags=["afternoon"],
            ),
            TimeslotModel(
                id="S2",
                start_minute=1440,
                end_minute=1500,
                talk_types=[0, 1],
                tags=["morning"],
            ),
        ],
        rooms=[
            RoomModel(
                id="R0",
                capacity=10,
                talk_types=[0],
                unavailable_timeslots=[1],
                tags=["small", "projector"],
            ),
            RoomModel(id="R1", capacity=100, talk_types=[0, 1], tags=["large"]),
        ],
        speakers=[
            SpeakerModel(
                id="P0",
                unavailable_timeslots=[0],
                required_timeslot_tags=["morning"],
                preferred_timeslot_tags=["morning"],
                prohibited_timeslot_tags=["afternoon"],
                undesired_timeslot_tags=["afternoon"],
                required_room_tags=["large"],
                preferred_room_tags=["large"],
                prohibited_room_tags=["small"],
                undesired_room_tags=["small"],
            ),
            SpeakerModel(id="P1"),
        ],
        talks=[
            TalkModel(
                code="A",
                talk_type=1,
                speakers=[0],
                theme_track_tags=["T"],
                sector_tags=["S"],
                audience_types=["dev"],
                audience_level=1,
                content_tags=["C"],
                language="ko",
                required_timeslot_tags=["morning"],
                preferred_timeslot_tags=["morning"],
                prohibited_timeslot_tags=["afternoon"],
                undesired_timeslot_tags=["afternoon"],
                required_room_tags=["large"],
                preferred_room_tags=["large"],
                prohibited_room_tags=["small"],
                undesired_room_tags=["small"],
                mutually_exclusive_talks_tags=["X"],
                prerequisite_talks=[1],
                favorite_count=20,
                crowd_control_risk=2,
            ),
            TalkModel(
                code="B",
                talk_type=0,
                speakers=[0, 1],
                theme_track_tags=["T"],
                sector_tags=["S"],
                audience_types=["dev"],
                audience_level=2,
                content_tags=["C"],
                mutually_exclusive_talks_tags=["X"],
                favorite_count=15,
                crowd_control_risk=1,
            ),
            TalkModel(
                code="C",
                talk_type=0,
                speakers=[1],
                theme_track_tags=["T"],
                sector_tags=["S"],
                audience_types=["ops"],
                audience_level=3,
                content_tags=["C"],
                language="en",
            ),
            TalkModel(code="D", talk_type=0, favorite_count=30),
        ],
        minimum_consecutive_talks_pause_in_minutes=45,
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_a_fixed_schedule(constraint) -> None:
    problem = _parity_problem()
    fixed = [0, 0, 3, 5]
    candidate = problem.model_copy(deep=True)
    for talk, slot_room in zip(candidate.talks, fixed, strict=True):
        talk.slot_room = slot_room
    expected = score(
        domain.constraint_costs(candidate.to_core()),
        [type(constraint)(weight=1)],
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_slot_rooms=fixed,
        _proxy_score=proxy,
    )

    assert (proxy["hard"], proxy["soft"]) == pytest.approx(expected)


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """The plugin catalog reads every default off the engine's constraint_info()."""
    engine = {row.name: row for row in domain.constraint_info()}
    entries = catalog()
    assert [entry["name"] for entry in entries] == domain.list_constraints()
    for entry in entries:
        row = engine[entry["name"]]
        assert entry["default_level"] == row.default_level
        assert entry["default_weight"] == row.default_weight
        assert entry["level_flippable"] == row.level_flippable
        assert entry["reward"] == row.reward
        assert entry["default_enabled"] == row.default_enabled
        # The three *_diversity_cost rewards stay soft; everything else flips.
        assert entry["kind"] == ("soft" if row.reward else "either")
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    assert [entry["name"] for entry in entries if entry["reward"]] == [
        "audience_type_diversity_cost",
        "audience_level_diversity_cost",
        "language_diversity_cost",
    ]


def _fixed_candidate(problem, fixed):
    candidate = problem.model_copy(deep=True)
    for talk, slot_room in zip(candidate.talks, fixed, strict=True):
        talk.slot_room = slot_room
    return candidate


def test_level_flips_score_like_the_engine() -> None:
    """level="soft" demotes and level="hard" promotes on both solvers alike."""
    problem = _parity_problem()
    fixed = [0, 0, 3, 5]
    flipped = {"room_conflict_cost", "speaker_conflict_cost", "crowd_control_risk_cost"}
    selection = [
        type(spec)(level="soft") if spec.name in flipped else spec
        for spec in get_constraints()
    ]
    selection = [
        type(spec)(level="hard") if spec.name == "theme_track_conflict_cost" else spec
        for spec in selection
    ]
    expected = score(
        domain.constraint_costs(_fixed_candidate(problem, fixed).to_core()), selection
    )
    proxy = {}
    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=selection,
        _fixed_slot_rooms=fixed,
        _proxy_score=proxy,
    )
    assert (proxy["hard"], proxy["soft"]) == pytest.approx(expected)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(expected)


@pytest.mark.parametrize("spec", [AudienceTypeDiversityCost, LanguageDiversityCost])
def test_a_reward_is_never_hard_on_cpsat(spec) -> None:
    with pytest.raises(ValueError, match="is a reward"):
        solve(_parity_problem(), time_limit_seconds=1, constraints=[spec(level="hard")])


def test_old_names_are_unknown() -> None:
    for old in ("talk_type_compatibility", "room_conflict", "same_day_talks"):
        with pytest.raises(ValueError, match="unknown constraint"):
            solve(_parity_problem(), time_limit_seconds=1, constraints=[old])


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _two_talks() -> ConferenceScheduleModel:
    """Two talks, three slots (two overlapping, one on the next day), two rooms: 36 assignments.

    Both talks share speaker P0, content C and theme T; their languages differ, so a
    shared slot earns the language-diversity reward. P0 cannot speak in S0, R0 is
    closed in S1.
    """
    return ConferenceScheduleModel(
        name="Tiny",
        talk_types=["Talk"],
        timeslots=[
            TimeslotModel(id="S0", start_minute=0, end_minute=60, talk_types=[0]),
            TimeslotModel(id="S1", start_minute=30, end_minute=90, talk_types=[0]),
            TimeslotModel(id="S2", start_minute=1440, end_minute=1500, talk_types=[0]),
        ],
        rooms=[
            RoomModel(id="R0", capacity=10, talk_types=[0], unavailable_timeslots=[1]),
            RoomModel(id="R1", capacity=100, talk_types=[0]),
        ],
        speakers=[
            SpeakerModel(id="P0", unavailable_timeslots=[0]),
            SpeakerModel(id="P1"),
        ],
        talks=[
            TalkModel(
                code="A",
                talk_type=0,
                speakers=[0],
                content_tags=["C"],
                theme_track_tags=["T"],
                language="ko",
                audience_level=1,
                favorite_count=20,
            ),
            TalkModel(
                code="B",
                talk_type=0,
                speakers=[0, 1],
                content_tags=["C"],
                theme_track_tags=["T"],
                language="en",
                audience_level=2,
                favorite_count=10,
            ),
        ],
    )


SELECTIONS = {
    "repeat": [ContentConflictCost(), ContentConflictCost(weight=2, offset=20)],
    "repeat-literal": [
        RoomConflictCost(),
        SpeakerConflictCost(),
        ContentConflictCost(offset=50, clip=False),
        ContentConflictCost(weight=3, offset=-10),
        LanguageDiversityCost(offset=20, clip=False),
    ],
    # The literal formula with an offset is refused on hard: the hard picks are
    # thresholds, their literal copies sit on soft.
    "hard-offsets": [
        RoomConflictCost(offset=10),
        RoomConflictCost(level="soft", weight=2, offset=40, clip=False),
        SpeakerUnavailabilityCost(offset=-3),
        SpeakerUnavailabilityCost(level="soft", offset=-3, clip=False),
    ],
    "identical-twice": [
        DifferentDayTalksCost(),
        DifferentDayTalksCost(),
        SpeakerMakespanCost(),
    ],
    # Each pick on its own level: a soft literal repeat of a hard constraint beyond an
    # offset, a hard threshold repeat of a soft one, a demoted hard one with a negative
    # literal offset.
    "repeat-levels": [
        RoomConflictCost(),
        RoomConflictCost(level=Level.SOFT, weight=2, offset=10, clip=False),
        ContentConflictCost(level="hard", weight=3, offset=50),
        ContentConflictCost(),
        SpeakerUnavailabilityCost(level="soft", offset=-3, clip=False),
    ],
}


def _assignments(problem):
    cells = len(problem.timeslots) * len(problem.rooms)
    for assignment in product(range(cells), repeat=len(problem.talks)):
        yield list(assignment), _fixed_candidate(problem, assignment)


def _engine_score(candidate, selection):
    """The engine's own score of `candidate` under `selection`: a zero-budget solve with every
    talk pinned, so the assignment stays as it is."""
    pinned = candidate.model_copy(deep=True)
    for talk in pinned.talks:
        talk.pinned = True
    core = pinned.to_core()
    config = domain.SolverConfig()
    config.spent_limit_seconds = 0
    domain.Solver(config).solve_with_constraints(core, selection)
    return core.score.hard, core.score.soft


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_assignment(selection) -> None:
    """Pin each of the 36 assignments: the CP-SAT objective's per-level value is the
    engine's score of that assignment under the same repeated / shifted picks, exactly."""
    problem = _two_talks()
    for assignment, candidate in _assignments(problem):
        core = candidate.to_core()
        engine = score(domain.constraint_costs(core, selection), selection)
        proxy = {}
        solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_slot_rooms=assignment,
            _proxy_score=proxy,
        )
        assert proxy == {"hard": engine.hard, "soft": engine.soft}, assignment
        # Re-pricing the default costs (each name once) gives the same per instance key.
        assert score(domain.constraint_costs(core), selection) == engine
        # ...and the engine's own score of the pinned assignment is the mirror's.
        assert _engine_score(candidate, selection) == tuple(engine), assignment


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _two_talks()
    best = max(
        tuple(score(domain.constraint_costs(candidate.to_core(), selection), selection))
        for _, candidate in _assignments(problem)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(best)
    engine = score(domain.constraint_costs(solved.to_core(), selection), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))


def test_mapping_selections_repeat_and_shift_like_specs() -> None:
    """The server's {name, weight, offset, clip} dicts pick the same instances as specs."""
    problem = _two_talks()
    specs = [ContentConflictCost(), ContentConflictCost(weight=3, offset=4, clip=False)]
    dicts = [
        {"name": "content_conflict_cost"},
        {"name": "content_conflict_cost", "weight": 3, "offset": 4, "clip": False},
    ]
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


@pytest.mark.parametrize("level", [Level.MEDIUM, "medium"])
def test_medium_is_refused_by_cpsat(level) -> None:
    """No medium level in this use case: CP-SAT refuses it like the engine, per instance."""
    for picks in (
        [ContentConflictCost(level=level)],
        [RoomConflictCost(), RoomConflictCost(level=level)],
    ):
        with pytest.raises(ValueError, match="this use case has no medium level"):
            solve(_two_talks(), time_limit_seconds=1, constraints=picks)


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _two_talks(),
            time_limit_seconds=1,
            constraints=[ContentConflictCost(offset=0.5)],
        )


def test_every_plugin_adds_its_total_once() -> None:
    """Every conference constraint is global, so every plugin takes offsets (terms_per_unit)."""
    from conference_scheduling_cpsat.constraints import REGISTRY

    assert all(plugin.terms_per_unit for plugin in REGISTRY.plugins)
    # A literal offset on a constraint whose total is 0 everywhere still charges it.
    selection = [SpeakerMakespanCost(offset=5, clip=False)]
    solved = solve(_two_talks(), time_limit_seconds=5, constraints=selection)
    assert (solved.score_hard, solved.score_soft) == (0, 20 * 5)


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


@pytest.mark.parametrize("offset", [40, -3])
@pytest.mark.parametrize(
    "selection",
    [
        lambda offset: RoomConflictCost(
            offset=offset, clip=False
        ),  # the default level, hard
        lambda offset: {"name": "room_conflict_cost", "offset": offset, "clip": False},
        lambda offset: ContentConflictCost(level="hard", offset=offset, clip=False),
        lambda offset: {
            "name": "content_conflict_cost",
            "level": "hard",
            "offset": offset,
            "clip": False,
        },
    ],
    ids=["room-spec", "room-mapping", "content-moved-to-hard", "content-mapping"],
)
def test_the_literal_offset_is_refused_on_hard_like_the_engine(
    selection, offset
) -> None:
    """The CP-SAT reference refuses the pick with the engine's and score()'s message,
    alone and as a repeat next to the default picks."""
    pick = selection(offset)
    name = pick["name"] if isinstance(pick, dict) else pick.name
    message = HARD_REFUSAL.format(q=repr(name))
    for picks in ([pick], [RoomConflictCost(), ContentConflictCost(), pick]):
        with pytest.raises(ValueError) as refused:
            solve(_two_talks(), time_limit_seconds=1, constraints=picks)
        assert str(refused.value) == message
    if not isinstance(pick, dict):
        # The engine and the score() mirror say the same.
        core = _two_talks().to_core()
        with pytest.raises(ValueError) as refused:
            domain.constraint_costs(core, [pick])
        assert str(refused.value) == message
        with pytest.raises(ValueError) as refused:
            score(domain.constraint_costs(core), [pick])
        assert str(refused.value) == message


@pytest.mark.parametrize("offset", [40, -3])
def test_the_same_literal_pick_on_soft_is_accepted(offset) -> None:
    picks = [
        RoomConflictCost(),
        RoomConflictCost(level="soft", offset=offset, clip=False),
    ]
    problem = _two_talks()
    best = max(
        tuple(score(domain.constraint_costs(candidate.to_core(), picks), picks))
        for _, candidate in _assignments(problem)
    )
    solved = solve(problem, time_limit_seconds=5, constraints=picks)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(best)
