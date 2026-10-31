"""sports_league.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import sports_league as sl
from sports_league.constraints import (
    AwayToAwayHopCost,
    AwayToEndHopCost,
    AwayToHomeHopCost,
    ConsecutiveAwayMatchesCost,
    ConsecutiveHomeMatchesCost,
    Constraint,
    HomeToAwayHopCost,
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

    hard_level = solve([*HARD_PICKS, *(type(hop)(hard=True) for hop in HOPS)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "workday_classic_match_cost"  # missing the es of matches
        weight = None
        hard = None
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
    """Module-level instances must keep the defaults (weight=None, hard=None, exponent 1)."""
    for spec in get_constraints():
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)


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
    """hard=False moves the raw cost to the soft level, in the engine and in score()."""
    schedule = build_schedule()
    schedule.set_assignment([0] * schedule.match_count())  # every match in round 0
    demoted = [TeamConflictCost(hard=False)]
    raw = sl.constraint_costs(schedule)
    pairs = sum(raw["team_conflict_cost"]["costs"])
    assert pairs > 0
    assert score(raw, demoted) == (0, -pairs)
    assert score(raw, [team_conflict_cost]) == (-pairs, 0.0)
    # The engine path agrees. One round and two matches sharing team 0 make the
    # conflict unavoidable, so the solve reports a non-zero cost on whichever level
    # the selection puts it (a binding that ignored hard=False would report -1hard).
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
        hard = None
        exponent = 1.0

    class Flat:
        name = "workday_classic_matches_cost"
        weight = None
        hard = None
        exponent = 0.0

    for pick, message in ((Negative(), "weight must be finite and >= 0"), (Flat(), "exponent must be finite and > 0")):
        with pytest.raises(ValueError, match=message):
            sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])


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
    pick = SimpleNamespace(name="workday_classic_matches_cost", weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint 'workday_classic_matches_cost': {re.escape(message)}"):
        sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        WorkdayClassicMatchesCost(**knobs)


def test_a_constraint_named_twice_is_rejected():
    with pytest.raises(ValueError, match="picked more than once"):
        get_constraints([team_conflict_cost, TeamConflictCost(weight=2)])


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
        pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
        with pytest.raises(ValueError, match="unknown constraint"):
            sl.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match="unknown constraint"):
            score(raw, [pick])
    for gone in ("Breakdown", "compute_breakdown"):
        assert not hasattr(sl, gone)
