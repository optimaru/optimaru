"""sports_league.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the five
knobs (the level included) and repeated picks.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import sports_league as sl
from sports_league._constraint import instance_keys
from sports_league.constraints import (
    AwayToAwayHopCost,
    AwayToEndHopCost,
    AwayToHomeHopCost,
    ConsecutiveAwayMatchesCost,
    ConsecutiveHomeMatchesCost,
    Constraint,
    HomeToAwayHopCost,
    Level,
    RepeatMatchOnNextDayCost,
    StartToAwayHopCost,
    TeamConflictCost,
    WorkdayClassicMatchesCost,
    away_to_away_hop_cost,
    away_to_end_hop_cost,
    away_to_home_hop_cost,
    consecutive_away_matches_cost,
    consecutive_home_matches_cost,
    get_constraints,
    home_to_away_hop_cost,
    repeat_match_on_next_day_cost,
    score,
    start_to_away_hop_cost,
    team_conflict_cost,
    workday_classic_matches_cost,
)

HARD_PICKS = [
    team_conflict_cost,
    consecutive_home_matches_cost,
    consecutive_away_matches_cost,
    repeat_match_on_next_day_cost,
]
HOPS = [
    start_to_away_hop_cost,
    home_to_away_hop_cost,
    away_to_away_hop_cost,
    away_to_home_hop_cost,
    away_to_end_hop_cost,
]


def build_schedule():
    return sl.demo_schedule()


def forced_conflict_schedule():
    """Three teams, one round, two matches sharing team 0: one unavoidable team conflict."""
    return sl.LeagueSchedule(
        [sl.Team(str(t)) for t in range(3)],
        [sl.Round(0)],
        [sl.Match("a", 0, 1), sl.Match("b", 0, 2)],
        [[0] * 3 for _ in range(3)],
    )


def solve(constraints=None):
    schedule = build_schedule()
    sl.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints after — differs from emplace order.
    available = sl.list_constraints()
    assert available == [
        "start_to_away_hop_cost",
        "away_to_end_hop_cost",
        "workday_classic_matches_cost",
        "team_conflict_cost",
        "consecutive_home_matches_cost",
        "consecutive_away_matches_cost",
        "repeat_match_on_next_day_cost",
        "home_to_away_hop_cost",
        "away_to_away_hop_cost",
        "away_to_home_hop_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == sl.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring."""
    schedule = build_schedule()
    sl.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=get_constraints())
    recomputed = sl.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the soft term disappears entirely."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve(HARD_PICKS)
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the derby weight grows soft only; hard stays put."""
    base = solve([*HARD_PICKS, workday_classic_matches_cost])
    heavier = solve([*HARD_PICKS, WorkdayClassicMatchesCost(weight=10**6)])
    assert heavier.hard == base.hard
    assert heavier.soft <= base.soft  # penalty, so equal or more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Only with all five travel terms on can they never reach 0. With just one on, the
    solver escapes to placements that avoid it (e.g. emptying the first round to make
    `start_to_away_hop_cost` 0), and the level shift cannot be observed."""
    soft_level = solve([*HARD_PICKS, *HOPS])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([*HARD_PICKS, *(type(hop)(level="hard") for hop in HOPS)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "workday_classic_match_cost"  # missing the es of matches
        weight = None
        level = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        sl.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instances_carry_no_overrides():
    """Module-level instances must keep the defaults (weight=None, level=None, exponent 1)."""
    for spec in get_constraints():
        assert (spec.weight, spec.level, spec.exponent) == (None, None, 1.0)


def test_every_spec_has_the_four_labels():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = build_schedule()
    sl.solve(schedule, time_limit_seconds=0.4, random_seed=1)
    return sl.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    This domain has only two levels, so the comparison needs no folding. This
    assertion pins down, in this package, that `score()` reproduces the engine's
    applyRaw arithmetic (weights, shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = sl.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


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
    base = score(raw, [workday_classic_matches_cost])
    ten = score(raw, [WorkdayClassicMatchesCost(weight=10)])
    default_weight = raw[workday_classic_matches_cost.name]["weight"]
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


# --- The 0.2.0 catalog: constraint_info() and the knobs -------------------------

#: (spec class, default level, default weight, global) in list_constraints() order.
CATALOG = [
    (StartToAwayHopCost, "soft", 1.0, False),
    (AwayToEndHopCost, "soft", 1.0, False),
    (WorkdayClassicMatchesCost, "soft", 1000.0, False),
    (TeamConflictCost, "hard", 1.0, True),
    (ConsecutiveHomeMatchesCost, "hard", 1.0, True),
    (ConsecutiveAwayMatchesCost, "hard", 1.0, True),
    (RepeatMatchOnNextDayCost, "hard", 1.0, True),
    (HomeToAwayHopCost, "soft", 1.0, True),
    (AwayToAwayHopCost, "soft", 1.0, True),
    (AwayToHomeHopCost, "soft", 1.0, True),
]


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = sl.constraint_info()
    assert [row.name for row in rows] == sl.list_constraints() == [cls.name for cls, *_ in CATALOG]
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [(level, weight, True, False, True) for _, level, weight, _ in CATALOG]


def test_constraint_info_rows_are_plain_values():
    row = sl.constraint_info()[2]
    assert isinstance(row, sl.ConstraintInfo)
    assert row._asdict() == {
        "name": "workday_classic_matches_cost",
        "default_level": "soft",
        "default_weight": 1000.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='workday_classic_matches_cost'")
    assert pickle.loads(pickle.dumps(sl.constraint_info())) == sl.constraint_info()
    assert sl.ConstraintInfo.__module__ == sl._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint."""
    raw = sl.constraint_costs(build_schedule())
    assert list(raw) == sl.list_constraints()
    for row, (_, _, _, is_global) in zip(sl.constraint_info(), CATALOG, strict=True):
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"], entry["global"]) == (True, False, is_global)
        assert (entry["name"], entry["label"], entry["exponent"]) == (row.name, "", 1.0)
        assert "medium_fold" not in entry  # no medium level in this domain


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights (1, and 1000 for derbies)."""
    explicit = [cls(weight=weight) for cls, _, weight, _ in CATALOG]
    raw, schedule = _raw()
    assert score(raw) == score(raw, explicit)
    assert score(raw) == (schedule.score.hard, pytest.approx(schedule.score.soft))


def test_demoting_a_hard_constraint_scores_it_as_soft():
    """level="soft" moves the raw cost to the soft level, in the engine and in score()."""
    schedule = build_schedule()
    schedule.set_assignment([0] * schedule.match_count())  # every match in round 0
    demoted = [TeamConflictCost(level="soft")]
    raw = sl.constraint_costs(schedule)
    pairs = sum(raw["team_conflict_cost"]["costs"])
    assert pairs > 0
    assert score(raw, demoted) == (0, -pairs)
    assert score(raw, [team_conflict_cost]) == (-pairs, 0.0)
    # The engine path agrees. One round and two matches sharing team 0 make the
    # conflict unavoidable, so the solve reports a non-zero cost on whichever level
    # the selection puts it (a binding that ignored level="soft" would report -1hard).
    demoted_solve = forced_conflict_schedule()
    sl.solve(demoted_solve, time_limit_seconds=0.3, random_seed=1, constraints=demoted)
    assert (demoted_solve.score.hard, demoted_solve.score.soft) == (0, -1.0)
    assert score(sl.constraint_costs(demoted_solve), demoted) == (0, -1.0)
    hard_solve = forced_conflict_schedule()
    sl.solve(hard_solve, time_limit_seconds=0.3, random_seed=1, constraints=[team_conflict_cost])
    assert (hard_solve.score.hard, hard_solve.score.soft) == (-1, 0.0)


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
        ({"offset": float("nan")}, ValueError),
        ({"offset": "1"}, TypeError),
        ({"clip": 0}, TypeError),
    ],
)
def test_specs_validate_their_knobs(kwargs, error):
    with pytest.raises(error):
        WorkdayClassicMatchesCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "workday_classic_matches_cost"
        weight = -2.0
        level = None
        exponent = 1.0

    class Flat:
        name = "workday_classic_matches_cost"
        weight = None
        level = None
        exponent = 0.0

    class Unbounded:
        name = "workday_classic_matches_cost"
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
            sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])


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
    pick = SimpleNamespace(name="workday_classic_matches_cost", weight=None, level=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'workday_classic_matches_cost': {re.escape(message)}"):
        sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        WorkdayClassicMatchesCost(**knobs)


def test_a_constraint_named_twice_scores_both_picks():
    """Since 0.3.0 a constraint may be picked several times: each pick is scored on its own.

    The later pick no longer wins in silence, nor is it an error: both are
    instances with their own knobs, and they add up."""
    assert get_constraints([TeamConflictCost(), TeamConflictCost(weight=2)]) == [
        TeamConflictCost(),
        TeamConflictCost(weight=2),
    ]
    picks = [TeamConflictCost(), TeamConflictCost(weight=2)]
    solved = forced_conflict_schedule()
    sl.solve(solved, time_limit_seconds=0.3, random_seed=1, constraints=picks)
    assert (solved.score.hard, solved.score.soft) == (-(1 + 2), 0.0)  # the one unavoidable conflict, twice


def test_old_names_are_gone():
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    raw = sl.constraint_costs(build_schedule())
    for old in [
        "matches_on_same_day",
        "consecutive_home_matches",
        "consecutive_away_matches",
        "repeat_match_on_next_day",
        "start_to_away_hop",
        "home_to_away_hop",
        "away_to_away_hop",
        "away_to_home_hop",
        "away_to_end_hop",
        "classic_matches",
    ]:
        pick = SimpleNamespace(name=old, weight=None, level=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])
    for gone in ("Breakdown", "compute_breakdown"):
        assert not hasattr(sl, gone)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------


def _two_rounds():
    """Two round positions (indices 0, 1), four teams at synthetic distances, both matches placed.

    (0, 1) in round 0 and (2, 0) in round 1. Raw: start_to_away_hop_cost [21, 0] (team 1
    travels d(1, 0) = 21 to the season's first round), away_to_end_hop_cost [0, 31] (team 0
    travels home from the last round, d(2, 0) = 31), workday_classic_matches_cost [0, 0];
    the globals: home_to_away_hop_cost 13 (team 0 from its home to 2's, d(0, 2)), every
    other total 0.
    """
    distance = [[0 if a == b else 10 * (a + 1) + (b + 1) for b in range(4)] for a in range(4)]
    return sl.LeagueSchedule(
        [sl.Team(str(t)) for t in range(4)],
        [sl.Round(0), sl.Round(1)],
        [sl.Match("a", 0, 1, round=0), sl.Match("b", 2, 0, round=1)],
        distance,
    )


def _zero_budget_solve(schedule, picks):
    """Score a complete assignment under ``picks`` on the engine: no step is taken."""
    config = sl.SolverConfig()
    config.spent_limit_seconds = 0
    assignment = schedule.assignment()
    sl.Solver(config).solve_with_constraints(schedule, picks)
    assert schedule.assignment() == assignment
    return schedule


def _engine(picks):
    solved = _zero_budget_solve(_two_rounds(), picks)
    return (solved.score.hard, solved.score.soft)


REPEAT = [HomeToAwayHopCost(), HomeToAwayHopCost(weight=2, exponent=2)]


def test_the_hand_instance():
    raw = sl.constraint_costs(_two_rounds())
    assert raw["start_to_away_hop_cost"]["costs"] == [21.0, 0.0]
    assert raw["away_to_end_hop_cost"]["costs"] == [0.0, 31.0]
    assert raw["workday_classic_matches_cost"]["costs"] == [0.0, 0.0]
    assert raw["home_to_away_hop_cost"]["costs"] == [13.0]
    assert score(raw) == _engine(None) == (0, -(21 + 31 + 13))


def test_a_repeated_pick_by_hand():
    """home-to-away km plus 2 x its square on the total: 13 + 2 x 169 = 351."""
    raw = sl.constraint_costs(_two_rounds(), REPEAT)
    assert list(raw) == ["home_to_away_hop_cost", "home_to_away_hop_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [13.0]
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 2.0, 2.0)
    assert first["name"] == second["name"] == "home_to_away_hop_cost"
    assert second["default_weight"] == 1.0
    expected = (0, -(13 + 2 * 13**2))
    assert expected == (0, -351)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(sl.constraint_costs(_two_rounds()), REPEAT) == expected
    assert _engine(REPEAT) == expected


def test_a_repeated_unit_pick_by_hand():
    """start hop per round plus 0.5 x its square: -(21 + 0.5 x 441) on round 0."""
    picks = [StartToAwayHopCost(), StartToAwayHopCost(weight=0.5, exponent=2)]
    raw = sl.constraint_costs(_two_rounds(), picks)
    assert list(raw) == ["start_to_away_hop_cost", "start_to_away_hop_cost#2"]
    assert raw["start_to_away_hop_cost"]["costs"] == raw["start_to_away_hop_cost#2"]["costs"]
    assert score(raw, picks) == (0, -241.5)
    assert score(sl.constraint_costs(_two_rounds()), picks) == (0, -241.5)
    assert _engine(picks) == (0, -241.5)


def test_identical_picks_count_twice():
    picks = [HomeToAwayHopCost(), HomeToAwayHopCost()]
    assert score(sl.constraint_costs(_two_rounds()), picks) == (0, -26.0)
    assert _engine(picks) == (0, -26.0)
    assert list(sl.constraint_costs(_two_rounds(), picks)) == ["home_to_away_hop_cost", "home_to_away_hop_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        HomeToAwayHopCost(weight=1),
        StartToAwayHopCost(),
        HomeToAwayHopCost(weight=2),
        TeamConflictCost(),
        HomeToAwayHopCost(weight=3),
    ]
    raw = sl.constraint_costs(_two_rounds(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "start_to_away_hop_cost": 1.0,
        "team_conflict_cost": 1.0,
        "home_to_away_hop_cost": 1.0,
        "home_to_away_hop_cost#2": 2.0,
        "home_to_away_hop_cost#3": 3.0,
    }
    assert score(raw, picks) == (0, -(21 + 6 * 13))
    assert _engine(picks) == score(raw, picks)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # Unit constraints, shaped per round position; an empty round (raw 0) counts in
        # literal mode.
        (StartToAwayHopCost(offset=10), (0, -11.0)),
        (StartToAwayHopCost(offset=10, clip=False), (0, -(11 - 10))),  # round 1 is rewarded
        (StartToAwayHopCost(offset=30), (0, 0.0)),
        (StartToAwayHopCost(offset=30, clip=False), (0, -(-9 - 30))),
        (AwayToEndHopCost(offset=-5), (0, -36.0)),  # a negative offset adds to every non-zero round
        (AwayToEndHopCost(offset=-5, clip=False), (0, -(5 + 36))),  # ...or to every round
        (WorkdayClassicMatchesCost(offset=1, clip=False, weight=1), (0, 2.0)),  # two derby-free rounds
        # Hard rounds per round: 61.5 -> 62, 0 -> 0. The literal formula with an offset
        # is refused on hard, so the literal pick stays on soft: 61.5 - 1.5, not rounded.
        (StartToAwayHopCost(level="hard", offset=0.5, weight=3), (-62, 0.0)),
        (StartToAwayHopCost(offset=0.5, clip=False, weight=3), (0, -(61.5 - 1.5))),
        # Global constraints shape their one total.
        (HomeToAwayHopCost(offset=3), (0, -10.0)),
        (HomeToAwayHopCost(offset=20, clip=False), (0, 7.0)),  # 13 - 20: a reward below 20 km
        (HomeToAwayHopCost(offset=10, exponent=2), (0, -9.0)),
        (HomeToAwayHopCost(offset=15, exponent=3, clip=False), (0, 8.0)),
        (TeamConflictCost(offset=-1), (0, 0.0)),  # the threshold leaves a 0 at 0
        # (0 - 0.5) x 3 = -1.5: a reward, refused on hard (its default), allowed on soft.
        (TeamConflictCost(level="soft", offset=0.5, clip=False, weight=3), (0, 1.5)),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert score(sl.constraint_costs(_two_rounds()), [pick]) == expected
    raw = sl.constraint_costs(_two_rounds(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert "units" not in entry  # every round is evaluated: no scoped constraint here
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*get_constraints()[:9], AwayToHomeHopCost(exponent=2)]
    shifted = [type(spec)(offset=0.0, clip=bool(index % 2)) for index, spec in enumerate(get_constraints()[:9])]
    shifted.append(AwayToHomeHopCost(exponent=2, offset=0, clip=False))
    raw, schedule = _raw()
    assert score(raw, shifted) == score(raw, plain)
    engines = []
    for picks in (plain, shifted):
        copy = build_schedule()
        copy.set_assignment(schedule.assignment())
        engines.append(_zero_budget_solve(copy, picks))
    assert engines[0].assignment() == engines[1].assignment() == schedule.assignment()
    assert (engines[0].score.hard, engines[0].score.soft) == (engines[1].score.hard, engines[1].score.soft)
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


@pytest.mark.parametrize(
    "specs",
    [
        [*get_constraints(), *REPEAT, StartToAwayHopCost(weight=2, exponent=2)],
        [
            *HARD_PICKS,
            TeamConflictCost(level="soft", offset=1, clip=False, weight=2),
            StartToAwayHopCost(offset=500),
            AwayToEndHopCost(offset=200, clip=False, weight=0.5),
            WorkdayClassicMatchesCost(),
            WorkdayClassicMatchesCost(offset=-1, weight=10),
            HomeToAwayHopCost(offset=2000),
            AwayToAwayHopCost(offset=5000, clip=False, exponent=2, weight=0.001),
            AwayToHomeHopCost(),
        ],
    ],
    ids=["repeat", "repeat-and-offsets"],
)
def test_score_matches_the_engine_for_repeated_and_shifted_specs(specs):
    schedule = build_schedule()
    sl.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=specs)
    python = score(sl.constraint_costs(schedule, specs), specs)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(sl.constraint_costs(schedule), specs) == python  # the default costs score the repeats too


@pytest.mark.parametrize(
    "picks",
    [
        [*get_constraints(), *REPEAT, StartToAwayHopCost(weight=2, exponent=2)],
        [
            TeamConflictCost(),
            TeamConflictCost(level="soft", offset=1, clip=False, weight=2),
            ConsecutiveHomeMatchesCost(offset=-1),
            ConsecutiveAwayMatchesCost(offset=2),
            ConsecutiveAwayMatchesCost(level="soft", offset=2, clip=False),
            RepeatMatchOnNextDayCost(),
            StartToAwayHopCost(offset=500, clip=False, weight=0.25),
            AwayToEndHopCost(offset=-100),
            WorkdayClassicMatchesCost(offset=1, clip=False),
            HomeToAwayHopCost(offset=2000, exponent=2, weight=0.001),
            AwayToAwayHopCost(),
            AwayToAwayHopCost(offset=5000, clip=False),
            AwayToHomeHopCost(),
        ],
        [
            *get_constraints(),
            TeamConflictCost(level="soft", weight=1.5, offset=1, clip=False),
            StartToAwayHopCost(level=Level.HARD, offset=500),
            HomeToAwayHopCost(level="hard", weight=0.5, offset=2000),
            WorkdayClassicMatchesCost(level="hard", weight=2),
        ],
    ],
    ids=["repeat", "repeat-and-offsets", "repeats-on-other-levels"],
)
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """ScoreChecks::Full re-checks the incremental score against a full recompute after every move."""
    schedule = build_schedule()
    config = sl.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 3000
    config.spent_limit_seconds = 60
    sl.Solver(config).solve_with_constraints(schedule, picks)
    raw = sl.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(sl.constraint_costs(schedule), picks) == python


# --- Levels: hard and soft, each pick (a repeat included) on the level it names ------


@pytest.mark.parametrize(
    ("picks", "expected"),
    [
        ([HomeToAwayHopCost(level="hard")], (-13, 0.0)),
        ([StartToAwayHopCost(level=Level.HARD, weight=2)], (-42, 0.0)),
        ([TeamConflictCost(level="soft", weight=5, offset=1, clip=False)], (0, 5.0)),  # 5 x (0 - 1), soft
        ([AwayToEndHopCost(level="soft")], (0, -31.0)),  # the default level, named
        # A repeat carries its own level and shaping. Hard: the start hop beyond 10 per
        # round (11 + 0). Soft: the default start hop 21 and home-to-away 13, and
        # 2 x (13 - 20) = -14 on the home-to-away total, a reward (refused on hard):
        # -34 + 14 = -20.
        (
            [
                StartToAwayHopCost(),
                StartToAwayHopCost(level="hard", offset=10),
                HomeToAwayHopCost(),
                HomeToAwayHopCost(level=Level.SOFT, weight=2, offset=20, clip=False),
            ],
            (-11, -20.0),
        ),
    ],
)
def test_levels_by_hand_on_both_paths(picks, expected):
    """A pick's level moves its term, with the weight in units of that level: engine == mirror == hand."""
    assert score(sl.constraint_costs(_two_rounds()), picks) == expected
    raw = sl.constraint_costs(_two_rounds(), picks)
    for pick, key in zip(picks, instance_keys(picks)):
        assert raw[key]["level"] == (pick.level or raw[key]["default_level"]), key
    assert score(raw, picks) == expected
    assert _engine(picks) == expected


@pytest.mark.parametrize("level", ["medium", Level.MEDIUM])
def test_medium_is_refused_without_a_medium_level(level):
    """The domain has no medium level: every path refuses the pick before scoring, a repeat too."""
    message = "constraint 'home_to_away_hop_cost': this use case has no medium level"
    pick = HomeToAwayHopCost(level=level)  # the spec cannot know the domain
    for picks in ([pick], [HomeToAwayHopCost(), pick]):
        with pytest.raises(ValueError, match=message):
            _engine(picks)
        with pytest.raises(ValueError, match=message):
            sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=picks)
        with pytest.raises(ValueError, match=message):
            sl.constraint_costs(_two_rounds(), picks)
        with pytest.raises(ValueError, match=message):
            score(sl.constraint_costs(_two_rounds()), picks)


def test_a_level_must_be_a_level():
    with pytest.raises(TypeError, match="level must be a Level"):
        HomeToAwayHopCost(level=True)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        HomeToAwayHopCost(level="firm")
    pick = SimpleNamespace(name="home_to_away_hop_cost", weight=None, level="firm", exponent=1.0)
    with pytest.raises(ValueError, match="level must be 'hard', 'medium' or 'soft', not 'firm'"):
        _engine([HomeToAwayHopCost(), pick])


def test_the_knob_was_renamed():
    """0.3.0 replaced hard=True / hard=False by level="hard" / level="soft"; hard= is unknown."""
    with pytest.raises(TypeError):
        HomeToAwayHopCost(hard=True)
    with pytest.raises(TypeError):
        TeamConflictCost(weight=None, hard=False)


# --- 0.3.0: clip=False with an offset stays off the hard score -------------------------

HARD_REFUSAL = (
    "constraint {q}: clip=False with a non-zero offset cannot be hard, because hard 0 must mean no "
    "violation; use clip=True (a threshold) or another level"
)


def _refusal(call):
    with pytest.raises(ValueError) as refused:
        call()
    return str(refused.value)


@pytest.mark.parametrize("offset", [3.0, -3.0])
@pytest.mark.parametrize(
    "pick",
    [
        lambda offset: TeamConflictCost(offset=offset, clip=False),  # the default level, hard
        lambda offset: TeamConflictCost(level="hard", offset=offset, clip=False),
        lambda offset: ConsecutiveHomeMatchesCost(offset=offset, clip=False),
        lambda offset: StartToAwayHopCost(level=Level.HARD, offset=offset, clip=False),
    ],
    ids=["team-conflict-default-hard", "team-conflict-level-hard", "consecutive-home", "start-hop-moved-to-hard"],
)
def test_the_literal_offset_is_refused_on_hard_on_every_path(pick, offset):
    """A positive offset would reward a schedule without team conflicts (hard above 0) and
    cancel another hard violation with it; a negative one would charge a schedule that
    holds none. The engine (solve, Solver, constraint_costs) and the score() mirror
    refuse it with the same message, alone and as a repeat; so does the CP-SAT
    reference (its own tests)."""
    spec = pick(offset)
    message = HARD_REFUSAL.format(q=repr(spec.name))
    picks = [*get_constraints(), spec]  # a repeat next to the defaults
    assert _refusal(lambda: sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[spec])) == message
    assert _refusal(lambda: _zero_budget_solve(_two_rounds(), [spec])) == message
    assert _refusal(lambda: _zero_budget_solve(_two_rounds(), picks)) == message
    assert _refusal(lambda: sl.constraint_costs(_two_rounds(), [spec])) == message
    assert _refusal(lambda: score(sl.constraint_costs(_two_rounds()), [spec])) == message
    assert _refusal(lambda: score(sl.constraint_costs(_two_rounds()), picks)) == message


@pytest.mark.parametrize("offset", [3.0, -3.0])
def test_the_same_literal_pick_on_soft_is_accepted(offset):
    """(0 - offset) on the team-conflict total, on soft, on every path."""
    pick = TeamConflictCost(level="soft", offset=offset, clip=False)
    expected = (0, -(0 - offset))
    assert score(sl.constraint_costs(_two_rounds()), [pick]) == expected
    raw = sl.constraint_costs(_two_rounds(), [pick])
    assert (raw["team_conflict_cost"]["level"], raw["team_conflict_cost"]["clip"]) == ("soft", False)
    assert score(raw, [pick]) == expected
    assert _engine([pick]) == expected
    # Next to the hard default, which keeps "hard 0 = no violation".
    assert _engine([TeamConflictCost(), pick]) == expected


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        (TeamConflictCost(offset=3.0), (0, 0.0)),  # clip=True on hard: a threshold
        (TeamConflictCost(offset=-3.0), (0, 0.0)),  # ...which leaves a raw 0 at 0
        (TeamConflictCost(clip=False), (0, 0.0)),  # clip=False with offset 0: the 0.2.0 shaping
        (StartToAwayHopCost(level="hard", offset=-5.0), (-26, 0.0)),  # 21 + 5 on round 0 only
        (StartToAwayHopCost(level="hard", offset=0.0, clip=False), (-21, 0.0)),
    ],
)
def test_the_allowed_combinations_on_hard(pick, expected):
    assert score(sl.constraint_costs(_two_rounds()), [pick]) == expected
    assert _engine([pick]) == expected


def test_the_refusal_comes_after_every_knob_of_a_duck_typed_pick():
    """The engine checks once a pick's knobs are all re-stated: the level a pick moves to decides."""
    moved = SimpleNamespace(name="team_conflict_cost", weight=None, level="soft", exponent=1.0, offset=3.0, clip=False)
    assert _engine([moved]) == (0, 3.0)
    kept = SimpleNamespace(name="team_conflict_cost", weight=None, level=None, exponent=1.0, offset=3.0, clip=False)
    message = HARD_REFUSAL.format(q="'team_conflict_cost'")
    assert _refusal(lambda: _engine([kept])) == message
    assert _refusal(lambda: score(sl.constraint_costs(_two_rounds()), [kept])) == message
