"""tournament_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file covers the selection path that picks "which constraints are on", the
catalog (`constraint_info()`), the five knobs, repeated picks and the medium level.
"""

import pickle
import re
from types import SimpleNamespace

import pytest

import tournament_scheduling as ts
from tournament_scheduling.constraints import (
    AssignmentCountImbalanceCost,
    ConfrontationCountImbalanceCost,
    Constraint,
    TeamConflictCost,
    TeamUnavailabilityCost,
    assignment_count_imbalance_cost,
    confrontation_count_imbalance_cost,
    get_constraints,
    score,
    team_conflict_cost,
    team_unavailability_cost,
)
from tournament_scheduling.models import demo_data

MEDIUM = "assignment_count_imbalance_cost"


def build_schedule():
    return demo_data().to_core()


def solve(constraints=None):
    schedule = build_schedule()
    ts.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints (the two imbalance terms) after.
    available = ts.list_constraints()
    assert available == [
        "team_conflict_cost",
        "team_unavailability_cost",
        "assignment_count_imbalance_cost",
        "confrontation_count_imbalance_cost",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ts.list_constraints()


def test_default_constraints_reproduce_the_monolithic_cost_function():
    """A cost function assembled from all default specs must equal the monolithic scoring.

    Whether both paths reach the same **solution** cannot be checked: the teamCountMoves
    guided operator does not run on ts's plugin path (it is tied to the monolithic
    calculator's live aggregates — cpp/include/ts/solver.hpp).
    What can be checked is that both calculators agree on the same solution."""
    schedule = build_schedule()
    ts.solve(
        schedule,
        time_limit_seconds=0.6,
        random_seed=1,
        constraints=get_constraints(),
    )
    recomputed = ts.calculate_score(schedule)
    assert schedule.score.hard == recomputed.hard
    assert schedule.score.soft == pytest.approx(recomputed.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the imbalance terms disappear entirely."""
    baseline = solve()
    assert baseline.soft < 0

    hard_only = solve([team_conflict_cost])
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the head-to-head balance weight grows soft only; hard stays put."""
    picks = [team_conflict_cost]
    base = solve([*picks, confrontation_count_imbalance_cost])
    heavier = solve([*picks, ConfrontationCountImbalanceCost(weight=50)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level.

    Head-to-head unfairness can never be 0 on this instance, so raising the level always
    lands on hard."""
    soft_level = solve([team_conflict_cost, confrontation_count_imbalance_cost])
    assert soft_level.hard == 0 and soft_level.soft < 0

    hard_level = solve([team_conflict_cost, ConfrontationCountImbalanceCost(hard=True)])
    assert hard_level.hard < 0
    assert hard_level.soft == 0


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ts.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_module_level_instances_keep_every_default():
    """Module-level instances must keep the defaults (weight=None, hard=None, exponent=1)."""
    instances = (
        team_conflict_cost,
        team_unavailability_cost,
        assignment_count_imbalance_cost,
        confrontation_count_imbalance_cost,
    )
    for spec in instances:
        assert (spec.weight, spec.hard, spec.exponent) == (None, None, 1.0)
    assert [spec.name for spec in instances] == ts.list_constraints()


def test_every_spec_has_labels_and_descriptions_in_both_languages():
    for spec in get_constraints():
        cls = type(spec)
        for attr in ("label_en", "label_ko", "description_en", "description_ko"):
            assert getattr(cls, attr), f"{cls.__name__}.{attr} is empty"


def test_the_spec_no_longer_shadows_the_unavailability_fact():
    """The fact class keeps its name; the constraint spec is TeamUnavailabilityCost."""
    assert ts.UnavailabilityPenalty is not TeamUnavailabilityCost
    assert ts.UnavailabilityPenalty.__module__ == ts._core.__name__
    assert TeamUnavailabilityCost.name == "team_unavailability_cost"


# --- The catalog (constraint_info) -------------------------------------------


def test_constraint_info_reports_the_cpp_defaults():
    """The defaults live in C++ only; constraint_info() reads them off the default assembly."""
    rows = ts.constraint_info()
    assert [row.name for row in rows] == ts.list_constraints()
    assert [
        (row.default_level, row.default_weight, row.level_flippable, row.reward, row.default_enabled) for row in rows
    ] == [
        ("hard", 1.0, True, False, True),  # team_conflict_cost
        ("hard", 1.0, True, False, True),  # team_unavailability_cost
        ("medium", 1.0, True, False, True),  # assignment_count_imbalance_cost
        ("soft", 1.0, True, False, True),  # confrontation_count_imbalance_cost
    ]


def test_constraint_info_rows_are_plain_values():
    row = ts.constraint_info()[0]
    assert isinstance(row, ts.ConstraintInfo)
    assert row._asdict() == {
        "name": "team_conflict_cost",
        "default_level": "hard",
        "default_weight": 1.0,
        "level_flippable": True,
        "reward": False,
        "default_enabled": True,
    }
    assert repr(row).startswith("ConstraintInfo(name='team_conflict_cost'")
    assert pickle.loads(pickle.dumps(ts.constraint_info())) == ts.constraint_info()
    assert ts.ConstraintInfo.__module__ == ts._core.__name__


def test_constraint_info_matches_the_raw_cost_metadata():
    """constraint_costs() reports the same defaults (weight, level) for every constraint,
    and every entry carries the domain's medium fold (1e6 into soft)."""
    raw = ts.constraint_costs(build_schedule())
    unit_names = ts.list_constraints()[:2]
    for row in ts.constraint_info():
        entry = raw[row.name]
        assert (entry["weight"], entry["level"], entry["hard"]) == (
            row.default_weight,
            row.default_level,
            row.default_level == "hard",
        )
        assert (entry["level_flippable"], entry["reward"]) == (True, False)
        assert entry["global"] == (row.name not in unit_names)
        assert (entry["name"], entry["label"]) == (row.name, "")  # one instance per name, no label
        assert entry["medium_fold"] == {"kind": "into_soft", "unit": ts.MEDIUM_SCALE}
    assert ts.MEDIUM_SCALE == 1e6
    assert [row.name for row in ts.constraint_info() if row.default_level == "medium"] == [MEDIUM]


# --- The knobs ------------------------------------------------------------------


def test_weight_none_means_the_component_default():
    """weight=None scores exactly like the explicit default weights (medium at 1, in medium units)."""
    explicit = [type(spec)(weight=row.default_weight) for spec, row in zip(get_constraints(), ts.constraint_info())]
    assert [spec.weight for spec in explicit] == [1, 1, 1, 1]
    schedule = _solved()
    raw = ts.constraint_costs(schedule)
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
        ConfrontationCountImbalanceCost(**kwargs)


def test_core_refuses_invalid_knobs_from_duck_typed_picks():
    """The C++ side guards too: a duck-typed pick cannot smuggle a negative weight in."""

    class Negative:
        name = "confrontation_count_imbalance_cost"
        weight = -2.0
        hard = None
        exponent = 1.0

    class Flat:
        name = "confrontation_count_imbalance_cost"
        weight = None
        hard = None
        exponent = 0.0

    class Unbounded:
        name = "confrontation_count_imbalance_cost"
        weight = None
        hard = None
        exponent = 1.0
        offset = float("inf")

    for pick, message in (
        (Negative(), "weight must be finite and >= 0"),
        (Flat(), "exponent must be finite and > 0"),
        (Unbounded(), "offset must be finite, not inf"),
    ):
        with pytest.raises(ValueError, match=message):
            ts.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
        with pytest.raises(ValueError, match=message):
            ts.constraint_costs(build_schedule(), [pick])
    with pytest.raises(ValueError, match="offset must be finite, not nan"):
        ConfrontationCountImbalanceCost(offset=float("nan"))


@pytest.mark.parametrize(
    ("knobs", "message"),
    [
        ({"weight": True}, "weight must be a number or None, not True"),
        ({"weight": "2"}, "weight must be a number or None, not '2'"),
        ({"hard": 1}, "hard must be True, False or None, not 1"),
        ({"hard": "x"}, "hard must be True, False or None, not 'x'"),
        ({"exponent": True}, "exponent must be a number, not True"),
        ({"offset": "5"}, "offset must be a number, not '5'"),
        ({"offset": False}, "offset must be a number, not False"),
        ({"clip": 1}, "clip must be True or False, not 1"),
        ({"clip": "no"}, "clip must be True or False, not 'no'"),
    ],
)
def test_core_refuses_the_types_the_python_base_refuses(knobs, message):
    """The C++ path checks the knob types like _constraint.py: TypeError, the same words."""
    name = "confrontation_count_imbalance_cost"
    pick = SimpleNamespace(name=name, weight=None, hard=None, exponent=1.0)
    for key, value in knobs.items():
        setattr(pick, key, value)
    with pytest.raises(TypeError, match=f"constraint '{name}': {re.escape(message)}"):
        ts.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(TypeError, match=re.escape(message)):
        ConfrontationCountImbalanceCost(**knobs)


OLD_NAMES = (
    "one_assignment_per_date_per_team",
    "unavailability_penalty",
    "fair_assignment_count_per_team",
    "evenly_confrontation_count",
)


@pytest.mark.parametrize("old", OLD_NAMES)
def test_old_names_are_gone(old):
    """0.2.0 renamed the catalog; the pre-0.2.0 names are unknown now (no aliases)."""
    pick = SimpleNamespace(name=old, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        ts.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(ts.constraint_costs(build_schedule()), [pick])
    assert old not in ts.list_constraints()


NEAR_MISSES = (
    "team_conflict_costs",
    "team_unavailability_costs",
    "assignment_count_imbalance",
    "confrontation_count_imbalance_costs",
)


@pytest.mark.parametrize("typo", NEAR_MISSES)
def test_a_near_miss_of_a_name_is_unknown(typo):
    """A name one suffix off a real one is still unknown, never a silent match."""
    pick = SimpleNamespace(name=typo, weight=None, hard=None, exponent=1.0)
    with pytest.raises(ValueError, match="unknown constraint"):
        ts.solve(build_schedule(), time_limit_seconds=0.1, constraints=[pick])
    with pytest.raises(ValueError, match="unknown constraint"):
        score(ts.constraint_costs(build_schedule()), [pick])
    assert typo not in ts.list_constraints()


# --- The medium level -------------------------------------------------------------


def _uneven_loads():
    """A fully pinned schedule with no hard violation: D0 T0, T1, T2; D1 T1, T2; D2 T1;
    D3 T1. Loads {1, 4, 2} -> sqrt(14/3) = 2.16025 medium; head-to-heads (0,1)=1 (0,2)=1
    (1,2)=2 -> sqrt(2/3) = 0.816497 soft."""
    slots = [(0, 0), (0, 1), (0, 2), (1, 1), (1, 2), (2, 1), (3, 1)]
    return ts.TournamentSchedule(
        [ts.Team(f"T{t}") for t in range(3)],
        [ts.Day(d) for d in range(4)],
        [],
        [ts.TeamAssignment(i, day, team=team, pinned=True) for i, (day, team) in enumerate(slots)],
    )


def _engine_score(constraints):
    """The engine's score of the (fully pinned) uneven schedule under `constraints`."""
    schedule = _uneven_loads()
    ts.solve(schedule, time_limit_seconds=0.1, random_seed=1, constraints=constraints)
    return schedule.score.hard, schedule.score.soft


def test_the_medium_level_folds_into_soft_at_1e6():
    schedule = _uneven_loads()
    raw = ts.constraint_costs(schedule)
    assert raw[MEDIUM]["costs"] == [pytest.approx(2.16025, abs=1e-12)]
    assert raw["confrontation_count_imbalance_cost"]["costs"] == [pytest.approx(0.816497, abs=1e-12)]
    engine = ts.calculate_score(schedule)
    assert (engine.hard, engine.soft) == (0, pytest.approx(-(2.16025 * 1e6 + 0.816497)))
    assert score(raw) == (engine.hard, engine.soft)
    assert _engine_score(None) == (engine.hard, engine.soft)


@pytest.mark.parametrize(
    ("spec", "expected"),
    [
        (AssignmentCountImbalanceCost(), (0, -2.16025 * 1e6)),  # medium, folded
        (AssignmentCountImbalanceCost(hard=True), (-2, 0.0)),  # one hard unit per raw unit, the total rounded
        (AssignmentCountImbalanceCost(hard=False), (0, -2.16025)),  # one soft unit, outside the fold
        (AssignmentCountImbalanceCost(weight=3), (0, -3 * 2.16025 * 1e6)),  # the weight stays in medium units
        (AssignmentCountImbalanceCost(hard=False, weight=5), (0, -5 * 2.16025)),
    ],
)
def test_medium_knobs_score_the_same_in_python_and_in_the_engine(spec, expected):
    """hard=True / hard=False move the medium constraint to hard / soft at one unit per raw
    unit; an explicit weight stays in medium units. Both paths agree."""
    raw = ts.constraint_costs(_uneven_loads())
    python = score(raw, [spec])
    engine = _engine_score([spec])
    assert python.hard == engine[0] == expected[0]
    assert python.soft == pytest.approx(expected[1]) and engine[1] == pytest.approx(expected[1])


def test_demoting_the_medium_scores_like_the_engine_on_a_solve():
    """hard=False on the medium term: the solve's own score equals the pure-Python rescoring."""
    selection = [spec if spec.name != MEDIUM else type(spec)(hard=False) for spec in get_constraints()]
    schedule = _solved(selection)
    python = score(ts.constraint_costs(schedule), selection)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _solved(constraints=None):
    schedule = build_schedule()
    ts.solve(schedule, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return schedule


def _raw():
    """Raw per-constraint costs of one solved solution."""
    schedule = _solved()
    return ts.constraint_costs(schedule), schedule


def test_score_reproduces_the_engine_score_exactly():
    """Python scoring must match the engine's hard/soft exactly.

    The engine reports HardSoftScore with the medium level folded into soft; `score()`
    folds the same way (the `medium_fold` of every entry). This assertion pins down, in
    this package, that `score()` reproduces the engine's applyRaw arithmetic (weights,
    shaping, per-unit hard rounding)."""
    raw, solution = _raw()
    engine = ts.calculate_score(solution)
    python = score(raw)
    assert python.hard == engine.hard
    assert python.soft == pytest.approx(engine.soft)


@pytest.mark.parametrize(
    "specs",
    [
        None,
        [team_conflict_cost],
        [team_conflict_cost, team_unavailability_cost, ConfrontationCountImbalanceCost(weight=50)],
        [TeamUnavailabilityCost(hard=False), team_conflict_cost, confrontation_count_imbalance_cost],
        [team_conflict_cost, team_unavailability_cost, ConfrontationCountImbalanceCost(exponent=2.0)],
        [TeamConflictCost(exponent=2.0), team_unavailability_cost, AssignmentCountImbalanceCost(hard=True)],
    ],
)
def test_score_matches_the_engine_for_arbitrary_specs(specs):
    """Rescoring a solution solved with arbitrary specs under those specs must equal the engine score.

    exponent applies per team for the two hard constraints and on the total for the
    two imbalance constraints, so reproducing it needs the per-unit raw costs —
    totals alone are not enough."""
    schedule = _solved(specs)
    python = score(ts.constraint_costs(schedule), specs)
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
    base = score(raw, [confrontation_count_imbalance_cost])
    ten = score(raw, [ConfrontationCountImbalanceCost(weight=10)])
    default_weight = raw[confrontation_count_imbalance_cost.name]["weight"]
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
    picks = [TeamConflictCost(), ConfrontationCountImbalanceCost(weight=10), ConfrontationCountImbalanceCost()]
    engine = _zero_budget(_hand(), picks)
    assert engine.hard == 0
    assert engine.soft == pytest.approx(-11 * 0.816497)
    assert get_constraints([TeamConflictCost(), TeamConflictCost()]) == [TeamConflictCost(), TeamConflictCost()]
    schedule = _solved(picks)
    python = score(ts.constraint_costs(schedule, picks), picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)


# --- 0.3.0: repeated picks and the offset / clip knobs ------------------------------
#
# The hand instance is _hand(): the pinned _uneven_loads() schedule plus three
# unavailability facts, T0 twice on D0 (it plays D0) and T2 on D3 (it does not). Raw per
# team (T0, T1, T2): team_unavailability_cost [2, 0, 0], evaluated on T0 and T2 only (the
# teams with a fact: its "units"); team_conflict_cost [0, 0, 0] on every team; the
# globals assignment_count_imbalance_cost 2.16025 (medium) and
# confrontation_count_imbalance_cost 0.816497 (soft).

ASSIGNMENT_IMBALANCE = 2.16025
CONFRONTATION_IMBALANCE = 0.816497


def _hand():
    schedule = _uneven_loads()
    return ts.TournamentSchedule(
        schedule.teams,
        schedule.days,
        [ts.UnavailabilityPenalty(0, 0), ts.UnavailabilityPenalty(0, 0), ts.UnavailabilityPenalty(2, 3)],
        schedule.team_assignments,
    )


def _zero_budget(schedule, picks):
    """The engine's score of the fully pinned `schedule` under `picks` (a solve keeps it)."""
    before = schedule.assignment()
    config = ts.SolverConfig()
    config.spent_limit_seconds = 0
    ts.Solver(config).solve_with_constraints(schedule, picks)
    assert schedule.assignment() == before
    return schedule.score


def _agrees(actual, expected):
    return actual.hard == expected[0] and actual.soft == pytest.approx(expected[1], abs=1e-9)


REPEAT = [TeamUnavailabilityCost(), TeamUnavailabilityCost(weight=2, exponent=2)]


def test_a_repeated_pick_by_hand():
    """The facts hit plus twice their square per team: 2 + 2 x 2^2 = 10 hard."""
    raw = ts.constraint_costs(_hand(), REPEAT)
    assert list(raw) == ["team_unavailability_cost", "team_unavailability_cost#2"]
    first, second = raw.values()
    assert first["costs"] == second["costs"] == [2.0, 0.0, 0.0]
    assert first["units"] == second["units"] == [0, 2]  # the teams with a fact
    assert (first["weight"], first["exponent"], second["weight"], second["exponent"]) == (1.0, 1.0, 2.0, 2.0)
    assert first["name"] == second["name"] == "team_unavailability_cost"
    assert second["default_weight"] == 1.0
    assert second["medium_fold"] == {"kind": "into_soft", "unit": 1e6}
    expected = (-(2 + 2 * 2**2), 0.0)
    assert expected == (-10, 0.0)
    assert score(raw, REPEAT) == expected
    # The default costs hold the name once; the second pick reads the same raw costs.
    assert score(ts.constraint_costs(_hand()), REPEAT) == expected
    assert _agrees(_zero_budget(_hand(), REPEAT), expected)


def test_identical_picks_count_twice():
    picks = [TeamUnavailabilityCost(), TeamUnavailabilityCost()]
    assert score(ts.constraint_costs(_hand()), picks) == (-4, 0.0)
    assert _agrees(_zero_budget(_hand(), picks), (-4, 0.0))
    assert list(ts.constraint_costs(_hand(), picks)) == ["team_unavailability_cost", "team_unavailability_cost#2"]


def test_the_keys_follow_the_pick_order_of_each_name():
    picks = [
        TeamUnavailabilityCost(weight=1),
        ConfrontationCountImbalanceCost(),
        TeamUnavailabilityCost(weight=2),
        TeamConflictCost(),
        TeamUnavailabilityCost(weight=3),
    ]
    raw = ts.constraint_costs(_hand(), picks)
    assert {key: entry["weight"] for key, entry in raw.items()} == {
        "team_conflict_cost": 1.0,
        "team_unavailability_cost": 1.0,
        "team_unavailability_cost#2": 2.0,
        "team_unavailability_cost#3": 3.0,
        "confrontation_count_imbalance_cost": 1.0,
    }
    assert [key for key in raw if key.startswith("team_unavailability")] == [
        "team_unavailability_cost",
        "team_unavailability_cost#2",
        "team_unavailability_cost#3",
    ]
    expected = (-12, -CONFRONTATION_IMBALANCE)
    assert _agrees(score(raw, picks), expected)
    assert _agrees(_zero_budget(_hand(), picks), expected)


def test_a_repeated_medium_pick_adds_up():
    """Both instances stay on the medium level (folded at 10^6); a promoted repeat goes hard
    (the total rounded per unit: 2.16025 -> 2)."""
    picks = [AssignmentCountImbalanceCost(), AssignmentCountImbalanceCost(weight=2)]
    expected = (0, -3 * ASSIGNMENT_IMBALANCE * 1e6)
    assert _agrees(score(ts.constraint_costs(_hand()), picks), expected)
    assert _agrees(_zero_budget(_hand(), picks), expected)
    mixed = [
        AssignmentCountImbalanceCost(),
        AssignmentCountImbalanceCost(hard=True),
        AssignmentCountImbalanceCost(hard=False, offset=1),
    ]
    expected = (-2, -ASSIGNMENT_IMBALANCE * 1e6 - (ASSIGNMENT_IMBALANCE - 1))
    assert _agrees(score(ts.constraint_costs(_hand(), mixed), mixed), expected)
    assert _agrees(_zero_budget(_hand(), mixed), expected)


@pytest.mark.parametrize(
    ("pick", "expected"),
    [
        # The scoped constraint evaluates T0 (2 facts hit) and T2 (0), never T1.
        (TeamUnavailabilityCost(offset=1), (-1, 0.0)),
        (TeamUnavailabilityCost(offset=3, clip=False), (4, 0.0)),  # (2 - 3) + (0 - 3), not 3 teams
        (TeamUnavailabilityCost(offset=-1), (-3, 0.0)),  # a negative offset adds to every non-zero unit
        (TeamUnavailabilityCost(offset=-1, clip=False), (-4, 0.0)),  # ...or to every evaluated unit
        # Every team is a unit of team_conflict_cost (all three at 0 here).
        (TeamConflictCost(offset=1, clip=False), (3, 0.0)),
        (TeamConflictCost(weight=3, offset=0.5, clip=False), (6, 0.0)),  # -1.5 -> -2 per team
        (TeamConflictCost(offset=1), (0, 0.0)),
        # The globals' totals are shaped once.
        (ConfrontationCountImbalanceCost(offset=1, clip=False), (0, -(CONFRONTATION_IMBALANCE - 1))),
        (ConfrontationCountImbalanceCost(offset=0.5, exponent=2), (0, -((CONFRONTATION_IMBALANCE - 0.5) ** 2))),
        (AssignmentCountImbalanceCost(offset=2), (0, -(ASSIGNMENT_IMBALANCE - 2) * 1e6)),
        (AssignmentCountImbalanceCost(offset=3, clip=False, hard=True), (1, 0.0)),  # -0.83975 -> -1
        (AssignmentCountImbalanceCost(offset=-1, hard=False), (0, -(ASSIGNMENT_IMBALANCE + 1))),
    ],
)
def test_offsets_by_hand_on_both_paths(pick, expected):
    """threshold sign(x) max(0, |x| - offset)^e and literal (x - offset)^e: Python mirror == engine == hand."""
    assert _agrees(score(ts.constraint_costs(_hand()), [pick]), expected)
    raw = ts.constraint_costs(_hand(), [pick])
    entry = next(iter(raw.values()))
    assert (entry["offset"], entry["clip"]) == (float(pick.offset), pick.clip)
    assert _agrees(score(raw, [pick]), expected)
    assert _agrees(_zero_budget(_hand(), [pick]), expected)


def test_only_the_scoped_constraint_reports_units():
    """team_unavailability_cost evaluates the teams with a fact, and only it carries "units";
    on a schedule without facts its literal offset charges nothing at all."""
    raw = ts.constraint_costs(_hand())
    assert raw["team_unavailability_cost"]["units"] == [0, 2]
    assert all("units" not in entry for key, entry in raw.items() if key != "team_unavailability_cost")
    pick = TeamUnavailabilityCost(offset=5, clip=False)
    plain = ts.constraint_costs(_uneven_loads())
    assert plain["team_unavailability_cost"]["units"] == []
    assert score(plain, [pick]) == (0, 0.0)
    assert _agrees(_zero_budget(_uneven_loads(), [pick]), (0, 0.0))
    # Facts for T0 on D3 and T2 on D2, days they do not play: every raw cost is 0, and
    # the literal offset charges the two scoped teams, 2 x 2 hard, not all three.
    loads = _uneven_loads()
    clear = ts.TournamentSchedule(
        loads.teams,
        loads.days,
        [ts.UnavailabilityPenalty(0, 3), ts.UnavailabilityPenalty(2, 2)],
        loads.team_assignments,
    )
    raw = ts.constraint_costs(clear)
    assert raw["team_unavailability_cost"]["costs"] == [0.0, 0.0, 0.0]
    assert raw["team_unavailability_cost"]["units"] == [0, 2]
    pick = TeamUnavailabilityCost(offset=2, clip=False)
    assert score(raw, [pick]) == (4, 0.0)
    assert _agrees(_zero_budget(clear, [pick]), (4, 0.0))


def test_offset_zero_reproduces_the_0_2_0_scores():
    """offset 0 is the plain shaping in both modes, so every 0.2.0 selection scores the same."""
    plain = [*get_constraints()[:-1], ConfrontationCountImbalanceCost(exponent=2)]
    shifted = [type(spec)(exponent=spec.exponent, offset=0.0, clip=False) for spec in plain]
    raw, solved = _raw()
    assert score(raw, shifted) == score(raw, plain)
    copies = []
    for picks in (plain, shifted):
        schedule = build_schedule()
        schedule.set_assignment(solved.assignment())
        config = ts.SolverConfig()
        config.spent_limit_seconds = 0
        ts.Solver(config).solve_with_constraints(schedule, picks)
        copies.append((schedule.score.hard, schedule.score.soft))
    assert copies[0] == copies[1]
    for entry in raw.values():
        assert (entry["offset"], entry["clip"]) == (0.0, True)


SELECTIONS = {
    "repeat": [*get_constraints(), *REPEAT, AssignmentCountImbalanceCost(weight=2, exponent=2)],
    "repeat-and-offsets": [
        TeamConflictCost(),
        TeamConflictCost(weight=2, offset=1, clip=False),
        TeamUnavailabilityCost(),
        TeamUnavailabilityCost(offset=-1, clip=False),
        AssignmentCountImbalanceCost(),
        AssignmentCountImbalanceCost(offset=1, exponent=2),
        AssignmentCountImbalanceCost(offset=-0.5, clip=False, hard=False, weight=3),
        ConfrontationCountImbalanceCost(offset=0.5),
        ConfrontationCountImbalanceCost(offset=2, clip=False, weight=0.25),
    ],
}


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_score_matches_the_engine_for_repeats_and_offsets(picks):
    schedule = _solved(picks)
    python = score(ts.constraint_costs(schedule, picks), picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(ts.constraint_costs(schedule), picks) == python  # the default costs score the repeats too


@pytest.mark.parametrize("picks", list(SELECTIONS.values()), ids=list(SELECTIONS))
def test_full_assert_solves_with_repeats_and_offsets(picks):
    """FULL_ASSERT re-checks the incremental score against a full recompute after every move."""
    schedule = build_schedule()
    config = ts.SolverConfig()
    config.full_assert = True
    config.random_seed = 3
    config.parallel_solver_count = 1
    config.step_count_limit = 2000
    config.spent_limit_seconds = 30
    ts.Solver(config).solve_with_constraints(schedule, picks)
    raw = ts.constraint_costs(schedule, picks)
    assert len(raw) == len(picks)
    python = score(raw, picks)
    assert python.hard == schedule.score.hard
    assert python.soft == pytest.approx(schedule.score.soft)
    assert score(ts.constraint_costs(schedule), picks) == python
