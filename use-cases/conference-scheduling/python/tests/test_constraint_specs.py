"""conference_scheduling.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pytest

import conference_scheduling as cs
from conference_scheduling.constraints import (
    Constraint,
    ContentConflict,
    RoomConflict,
    content_conflict,
    get_constraints,
    room_conflict,
    room_unavailable_timeslot,
    score,
    speaker_conflict,
    talk_type_compatibility,
    theme_track_conflict,
)

HARD_PICKS = [
    talk_type_compatibility,
    room_unavailable_timeslot,
    room_conflict,
    speaker_conflict,
]


def build_schedule():
    return cs.demo_conference()


def solve(constraints=None):
    schedule = build_schedule()
    cs.solve(schedule, time_limit_seconds=0.6, random_seed=1, constraints=constraints)
    return schedule.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    available = cs.list_constraints()
    assert len(available) == 37
    assert available[:5] == [
        "talk_type_compatibility",
        "room_unavailable_timeslot",
        "room_conflict",
        "speaker_unavailable_timeslot",
        "speaker_conflict",
    ]
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
    base = solve([*HARD_PICKS, content_conflict])
    heavier = solve([*HARD_PICKS, ContentConflict(weight=100_000)])
    assert heavier.hard == base.hard
    assert heavier.soft <= base.soft  # penalty, so equal or more negative


def test_level_can_be_flipped():
    """Promoting a soft constraint to hard moves its penalty to the hard level."""
    picks = [talk_type_compatibility, room_unavailable_timeslot]

    soft_level = solve([*picks, theme_track_conflict, content_conflict])
    assert soft_level.hard == 0

    hard_level = solve(
        [
            *picks,
            type(theme_track_conflict)(hard=True),
            type(content_conflict)(hard=True),
        ]
    )
    assert hard_level.soft == 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "room_conflicts"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        cs.solve(build_schedule(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert content_conflict.weight is None
    assert content_conflict.hard is None
    assert content_conflict.exponent == 1.0


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
    base = score(raw, [room_conflict])
    ten = score(raw, [RoomConflict(weight=10)])
    default_weight = raw[room_conflict.name]["weight"]
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
