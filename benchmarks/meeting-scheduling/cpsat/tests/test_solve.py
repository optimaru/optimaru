from itertools import product

import pytest

import meeting_scheduling as domain
from meeting_scheduling.constraints import (
    LargerRoomCapacityCost,
    Level,
    MeetingEndTimeCost,
    MeetingOverlapCost,
    PreferredAttendanceConflictCost,
    RequiredAndPreferredAttendanceConflictCost,
    RequiredAttendanceConflictCost,
    RoomCapacityCost,
    RoomConflictCost,
    get_constraints,
    score,
)
from meeting_scheduling.models import (
    MeetingAssignmentModel,
    MeetingModel,
    MeetingScheduleModel,
    PersonModel,
    RoomModel,
    TimeGrainModel,
)
from meeting_scheduling_cpsat import solve
from meeting_scheduling import calculate_score


def test_room_conflict_cost_is_avoided() -> None:
    problem = MeetingScheduleModel(
        people=[],
        time_grains=[
            TimeGrainModel(
                id="g0", grain_index=0, day_of_year=1, starting_minute_of_day=540
            ),
            TimeGrainModel(
                id="g1", grain_index=1, day_of_year=1, starting_minute_of_day=570
            ),
        ],
        rooms=[RoomModel(id="r", capacity=10)],
        meetings=[MeetingModel(id="m0"), MeetingModel(id="m1")],
        meeting_assignments=[
            MeetingAssignmentModel(id="a0", meeting=0),
            MeetingAssignmentModel(id="a1", meeting=1),
        ],
    )

    solved = solve(problem, time_limit_seconds=2)

    assert solved.score_hard == 0
    assert {assignment.grain_room for assignment in solved.meeting_assignments} == {
        0,
        1,
    }
    shared = calculate_score(solved.to_core())
    assert (solved.score_hard, solved.score_soft) == (shared.hard, shared.soft)


def _parity_problem() -> MeetingScheduleModel:
    return MeetingScheduleModel(
        people=[PersonModel(id=str(index)) for index in range(3)],
        time_grains=[
            TimeGrainModel(
                id=f"g{index}",
                grain_index=index,
                day_of_year=100 if index < 4 else 101,
                starting_minute_of_day=540 + 15 * (index % 4),
            )
            for index in range(8)
        ],
        rooms=[RoomModel(id="small", capacity=1), RoomModel(id="large", capacity=5)],
        meetings=[
            MeetingModel(
                id="m0",
                duration_in_grains=4,
                required_attendees=[0, 0],
                preferred_attendees=[0, 1],
            ),
            MeetingModel(
                id="m1",
                duration_in_grains=4,
                required_attendees=[0],
                preferred_attendees=[1, 1],
            ),
            MeetingModel(
                id="m2",
                duration_in_grains=2,
                required_attendees=[0],
                preferred_attendees=[1],
            ),
        ],
        meeting_assignments=[
            MeetingAssignmentModel(id="a0", meeting=0),
            MeetingAssignmentModel(id="a1", meeting=1),
            MeetingAssignmentModel(id="a2", meeting=2),
        ],
    )


@pytest.mark.parametrize("constraint", get_constraints(), ids=lambda item: item.name)
def test_each_constraint_matches_domain_on_fixed_meetings(constraint) -> None:
    problem = _parity_problem()
    fixed = [4, 4, 13]
    candidate = problem.model_copy(deep=True)
    for assignment, grain_room in zip(
        candidate.meeting_assignments, fixed, strict=True
    ):
        assignment.grain_room = grain_room
    expected = -sum(
        domain.constraint_costs(candidate.to_core())[constraint.name]["costs"]
    )
    proxy = {}

    solve(
        problem,
        time_limit_seconds=2,
        constraints=[{"name": constraint.name, "weight": 1}],
        _fixed_grain_rooms=fixed,
        _proxy_score=proxy,
    )

    assert sum(proxy.values()) == pytest.approx(expected)


def _fixed_candidate(
    problem: MeetingScheduleModel, fixed: list[int]
) -> MeetingScheduleModel:
    candidate = problem.model_copy(deep=True)
    for assignment, grain_room in zip(
        candidate.meeting_assignments, fixed, strict=True
    ):
        assignment.grain_room = grain_room
    return candidate


def test_cpsat_defaults_are_the_engine_defaults() -> None:
    """Every plugin reads its default level and weight from constraint_info()."""
    from meeting_scheduling_cpsat import catalog

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
        assert entry["kind"] == "either"  # every meeting constraint is flippable
        assert entry["params"] == {
            "weight": None,
            "level": None,
            "exponent": 1.0,
            "offset": 0.0,
            "clip": True,
        }
    levels = {
        entry["name"]: (entry["default_level"], entry["default_weight"])
        for entry in entries
    }
    # The values the plugins used to restate by hand now come from C++.
    assert levels["required_and_preferred_attendance_conflict_cost"] == ("medium", 1.0)
    assert levels["preferred_attendance_conflict_cost"] == ("medium", 1.0)
    assert levels["consecutive_meetings_cost"] == ("soft", 100.0)
    assert levels["meeting_overlap_cost"] == ("soft", 10.0)


def test_default_score_folds_medium_like_the_engine() -> None:
    """The returned score folds the medium level into soft with the engine's 1e6."""
    problem = _parity_problem()
    fixed = [4, 4, 13]
    expected = domain.calculate_score(_fixed_candidate(problem, fixed).to_core())
    raw = domain.constraint_costs(_fixed_candidate(problem, fixed).to_core())
    assert (
        sum(sum(raw[name]["costs"]) for name in raw if raw[name]["level"] == "medium")
        > 0
    )

    proxy = {}
    solved = solve(
        problem, time_limit_seconds=2, _fixed_grain_rooms=fixed, _proxy_score=proxy
    )

    assert (solved.score_hard, solved.score_soft) == (expected.hard, expected.soft)
    # The CP-SAT model itself puts the medium terms on its medium level.
    assert proxy["medium"] < 0
    assert proxy["hard"] == expected.hard
    assert proxy["medium"] * domain.MEDIUM_SCALE + proxy["soft"] == pytest.approx(
        expected.soft
    )


@pytest.mark.parametrize(
    ("selection", "expect_medium_empty"),
    [
        (
            [
                spec
                if spec.name
                not in (
                    "required_and_preferred_attendance_conflict_cost",
                    "preferred_attendance_conflict_cost",
                )
                else type(spec)(level="soft")
                for spec in get_constraints()
            ],
            True,
        ),
        (
            [
                PreferredAttendanceConflictCost(level="hard"),
                RequiredAndPreferredAttendanceConflictCost(weight=3),
            ],
            False,
        ),
    ],
    ids=["medium-demoted-to-soft", "medium-promoted-and-reweighted"],
)
def test_medium_knobs_score_like_the_engine(selection, expect_medium_empty) -> None:
    """level="soft" demotes a medium constraint to soft and level="hard" promotes it to
    hard on both solvers (one unit per raw person-grain); an explicit weight stays
    in medium units."""
    problem = _parity_problem()
    fixed = [4, 4, 13]
    proxy = {}

    solved = solve(
        problem,
        time_limit_seconds=2,
        constraints=selection,
        _fixed_grain_rooms=fixed,
        _proxy_score=proxy,
    )

    engine = score(domain.constraint_costs(solved.to_core()), selection)
    assert (solved.score_hard, solved.score_soft) == pytest.approx(tuple(engine))
    # The CP-SAT model itself puts each demoted/promoted medium term on the
    # level the engine charges it to, not only the re-derived returned score.
    assert proxy["hard"] == engine.hard
    assert proxy["medium"] * domain.MEDIUM_SCALE + proxy["soft"] == pytest.approx(
        engine.soft
    )
    if expect_medium_empty:
        assert proxy["medium"] == 0


# --- 0.3.0: repeated picks and offset / clip ---------------------------------------


def _two_meetings() -> MeetingScheduleModel:
    """Two 2-grain meetings, four grains on one day, two rooms (capacity 1 and 3): 64 assignments.

    Person 0 is required at m0 and preferred at m1, person 1 preferred at both, so an
    overlap costs on the medium level; a start in the last grain runs into overtime.
    """
    return MeetingScheduleModel(
        people=[PersonModel(id=str(index)) for index in range(2)],
        time_grains=[
            TimeGrainModel(
                id=f"g{index}",
                grain_index=index,
                day_of_year=100,
                starting_minute_of_day=540 + 15 * index,
            )
            for index in range(4)
        ],
        rooms=[RoomModel(id="small", capacity=1), RoomModel(id="large", capacity=3)],
        meetings=[
            MeetingModel(
                id="m0",
                duration_in_grains=2,
                required_attendees=[0],
                preferred_attendees=[1],
            ),
            MeetingModel(id="m1", duration_in_grains=2, preferred_attendees=[0, 1]),
        ],
        meeting_assignments=[
            MeetingAssignmentModel(id="a0", meeting=0),
            MeetingAssignmentModel(id="a1", meeting=1),
        ],
    )


SELECTIONS = {
    "repeat": [MeetingEndTimeCost(), MeetingEndTimeCost(weight=2, offset=2)],
    "repeat-literal": [
        RoomConflictCost(),
        MeetingEndTimeCost(offset=3, clip=False),
        MeetingEndTimeCost(weight=3, offset=-1),
        MeetingOverlapCost(offset=1, clip=False),
    ],
    "hard-offsets": [
        RoomCapacityCost(offset=1),
        RoomCapacityCost(weight=2, offset=2, clip=False),
        RoomConflictCost(offset=-3, clip=False),
    ],
    "medium": [
        PreferredAttendanceConflictCost(),
        PreferredAttendanceConflictCost(weight=2, offset=1, clip=False),
        RequiredAndPreferredAttendanceConflictCost(offset=1),
        RequiredAttendanceConflictCost(offset=1),
    ],
    "identical-twice": [
        MeetingOverlapCost(),
        MeetingOverlapCost(),
        LargerRoomCapacityCost(),
    ],
    # Each pick on its own level, medium included: soft and hard constraints moved onto
    # medium, medium ones moved off it, repeats of one name on three levels, with offsets.
    "levels": [
        MeetingEndTimeCost(),
        MeetingEndTimeCost(level=Level.MEDIUM, offset=2),
        MeetingEndTimeCost(level="hard", weight=2, offset=3),
        MeetingOverlapCost(level="medium", weight=2, offset=1),
        RoomConflictCost(level="medium"),
        PreferredAttendanceConflictCost(level="soft", offset=1, clip=False),
        PreferredAttendanceConflictCost(level=Level.HARD),
    ],
}


def _assignments(problem):
    cells = len(problem.time_grains) * len(problem.rooms)
    for assignment in product(range(cells), repeat=len(problem.meeting_assignments)):
        yield list(assignment), _fixed_candidate(problem, list(assignment))


def _engine_score(candidate, selection):
    """The engine's own score of `candidate` under `selection`: a zero-budget solve with every
    meeting pinned, so the assignment stays as it is."""
    pinned = candidate.model_copy(deep=True)
    for assignment in pinned.meeting_assignments:
        assignment.pinned = True
    core = pinned.to_core()
    config = domain.SolverConfig()
    config.spent_limit_seconds = 0
    domain.Solver(config).solve_with_constraints(core, selection)
    return core.score.hard, core.score.soft


def _folded(proxy: dict[str, int]) -> dict[str, float]:
    return {
        "hard": proxy["hard"],
        "soft": proxy["medium"] * domain.MEDIUM_SCALE + proxy["soft"],
    }


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_the_cpsat_terms_equal_the_engine_on_every_assignment(selection) -> None:
    """Pin each of the 64 assignments: the CP-SAT objective's per-level value is the
    engine's score of that assignment under the same repeated / shifted picks, exactly."""
    problem = _two_meetings()
    for assignment, candidate in _assignments(problem):
        core = candidate.to_core()
        engine = score(domain.constraint_costs(core, selection), selection)
        proxy = {}
        solve(
            problem,
            time_limit_seconds=5,
            constraints=selection,
            _fixed_grain_rooms=assignment,
            _proxy_score=proxy,
        )
        assert _folded(proxy) == {"hard": engine.hard, "soft": engine.soft}, assignment
        # Re-pricing the default costs (each name once) gives the same per instance key.
        assert score(domain.constraint_costs(core), selection) == engine
        # ...and the engine's own score of the pinned assignment is the mirror's.
        assert _engine_score(candidate, selection) == tuple(engine), assignment


@pytest.mark.parametrize("selection", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_repeated_and_shifted_picks_reach_the_brute_force_optimum(selection) -> None:
    problem = _two_meetings()
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
    problem = _two_meetings()
    specs = [MeetingEndTimeCost(), MeetingEndTimeCost(weight=3, offset=4, clip=False)]
    dicts = [
        {"name": "meeting_end_time_cost"},
        {"name": "meeting_end_time_cost", "weight": 3, "offset": 4, "clip": False},
    ]
    assert solve(problem, time_limit_seconds=5, constraints=dicts) == solve(
        problem, time_limit_seconds=5, constraints=specs
    )


def test_a_fractional_offset_is_refused_by_cpsat() -> None:
    with pytest.raises(ValueError, match="offset must be an integer for CP-SAT"):
        solve(
            _two_meetings(),
            time_limit_seconds=1,
            constraints=[MeetingEndTimeCost(offset=0.5)],
        )


def test_every_plugin_adds_engine_units() -> None:
    """Every plugin adds one term per engine unit (a cell, or a pair constraint's total)."""
    from meeting_scheduling_cpsat.constraints import REGISTRY

    assert all(plugin.terms_per_unit for plugin in REGISTRY.plugins)
    # A literal offset on a unit constraint charges every one of the 8 cells, empty ones included.
    selection = [LargerRoomCapacityCost(offset=1, clip=False)]
    solved = solve(_two_meetings(), time_limit_seconds=5, constraints=selection)
    assert solved.score_hard == 0
    assert (
        solved.score_soft
        == score(domain.constraint_costs(solved.to_core()), selection).soft
    )
    assert (
        solved.score_soft == 8 - 0
    )  # both meetings in the large room (penalty 0): 8 cells x +1
