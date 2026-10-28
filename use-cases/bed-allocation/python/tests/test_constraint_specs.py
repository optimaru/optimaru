"""bed_allocation.constraints — an API for picking the cost function declaratively.

Spot checks of individual constraint values live in `test_constraints.py`. This
file only covers the selection path that picks "which constraints are on".
"""

import pytest

import bed_allocation as ba
from bed_allocation.constraints import (
    Constraint,
    PreferredMaximumRoomCapacity,
    SameBedInSameNight,
    assign_every_patient_to_a_bed,
    get_constraints,
    preferred_maximum_room_capacity,
    same_bed_in_same_night,
    score,
)


def make_stay(index, bed=-1):
    return ba.Stay(
        f"s{index}",
        f"patient-{index}",
        ba.Gender.MALE,
        30,
        0,
        5,  # 6 nights
        "default",
        patient_preferred_maximum_room_capacity=1,
        bed=bed,
    )


def build_plan():
    """Only capacity-2 rooms, so a preferred-capacity (1) violation always remains, and
    more stays (4) than beds (3), so a "no bed" medium cost always remains."""
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=2), ba.Room("r1", "r1", capacity=2)],
        [ba.Bed("b0", 0), ba.Bed("b1", 0), ba.Bed("b2", 1)],
        [make_stay(i) for i in range(4)],
    )


def build_crowded_plan():
    """Two overlapping stays for one bed — forces a choice between sharing (hard) and
    unassigned (medium). Preferred capacity is left empty to remove soft noise."""
    return ba.BedPlan(
        [ba.Department("d0", "Dept", {"default": 1})],
        [ba.Room("r0", "r0", capacity=1)],
        [ba.Bed("b0", 0)],
        [ba.Stay(f"s{i}", f"patient-{i}", ba.Gender.MALE, 30, 0, 5, "default") for i in range(2)],
    )


def solve(constraints=None):
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return plan.score


def solve_crowded(constraints=None):
    plan = build_crowded_plan()
    ba.solve(plan, time_limit_seconds=0.4, random_seed=1, constraints=constraints)
    return plan.score


def test_list_constraints_matches_the_spec_names():
    """Names carried by specs must be names the core actually knows."""
    # Unit constraints first, global constraints after — the global
    # different_gender_... goes last.
    available = ba.list_constraints()
    assert available == [
        "same_bed_in_same_night",
        "female_in_male_room",
        "male_in_female_room",
        "department_minimum_age",
        "department_maximum_age",
        "required_patient_equipment",
        "assign_every_patient_to_a_bed",
        "preferred_maximum_room_capacity",
        "department_specialty",
        "department_specialty_not_first_priority",
        "preferred_patient_equipment",
        "different_gender_in_same_gender_room_in_same_night",
    ]
    for spec in get_constraints():
        assert spec.name in available


def test_get_constraints_defaults_to_the_full_set():
    assert [spec.name for spec in get_constraints()] == ba.list_constraints()


def test_default_solve_equals_explicit_default_constraints():
    """`constraints=get_constraints()` must score the same as the default path."""
    plain = solve()
    explicit = solve(get_constraints())
    assert plain.hard == explicit.hard
    assert plain.medium == explicit.medium
    assert plain.soft == pytest.approx(explicit.soft)


def test_picking_a_subset_drops_the_other_terms():
    """Unlisted constraints are not scored — the medium/soft terms disappear entirely."""
    baseline = solve()
    assert baseline.medium < 0 and baseline.soft < 0

    hard_only = solve([same_bed_in_same_night])
    assert hard_only.medium == 0
    assert hard_only.soft == 0


def test_reweighting_one_constraint_scales_only_that_term():
    """Raising only the preferred-capacity weight grows soft only; hard stays put.

    `assign_every_patient_to_a_bed` must be on too — otherwise unassigned is free, the
    solver leaves everyone out of a bed and the soft term itself vanishes."""
    picks = [same_bed_in_same_night, assign_every_patient_to_a_bed]
    base = solve([*picks, preferred_maximum_room_capacity])
    heavier = solve([*picks, PreferredMaximumRoomCapacity(weight=80)])
    assert heavier.hard == base.hard
    assert heavier.soft < base.soft  # penalty, so more negative


def test_level_can_be_flipped():
    """Demoting a hard constraint to soft flips the solver's choice.

    Two overlapping stays for one bed: sharing the bed is same_bed_in_same_night,
    leaving one out is assign_every_patient_to_a_bed. With the former hard, unassigned
    is cheaper (levels take priority); demoted to soft, sharing becomes cheaper."""
    plan_picks = [assign_every_patient_to_a_bed]

    hard_default = solve_crowded([same_bed_in_same_night, *plan_picks])
    assert hard_default.medium < 0  # leaves one out instead of sharing
    assert hard_default.soft == 0

    flipped = solve_crowded([SameBedInSameNight(hard=False), *plan_picks])
    assert flipped.medium == 0  # sharing is cheaper now
    assert flipped.soft < 0


def test_unknown_constraint_name_raises():
    """A typo must not silently leave the cost function unchanged."""

    class Typo:
        name = "same_bed_in_same_nights"  # trailing s
        weight = None
        hard = None
        exponent = 1.0

    with pytest.raises(ValueError, match="unknown constraint"):
        solve([Typo()])


def test_empty_constraint_list_is_rejected():
    """An empty list means 'score nothing', which is most likely a mistake."""
    with pytest.raises(ValueError, match="must not be empty"):
        ba.solve(build_plan(), time_limit_seconds=0.2, constraints=[])


def test_spec_subclass_must_name_a_constraint():
    with pytest.raises(TypeError, match="registry `name`"):

        class Nameless(Constraint):
            pass


def test_default_instance_carries_no_overrides():
    """Module-level instances must keep the defaults (weight=None)."""
    assert preferred_maximum_room_capacity.weight is None
    assert preferred_maximum_room_capacity.hard is None
    assert preferred_maximum_room_capacity.exponent == 1.0


# --- Pure-Python scoring (constraint_costs -> score) ------------------------


def _raw():
    """Raw per-constraint costs of one solved solution."""
    plan = build_plan()
    ba.solve(plan, time_limit_seconds=0.3, random_seed=1)
    return ba.constraint_costs(plan), plan


def test_score_reproduces_the_engine_hard_level():
    """The hard level of the Python score at default specs must match the engine."""
    raw, solution = _raw()
    assert score(raw).hard == ba.calculate_score(solution).hard


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
    base = score(raw, [same_bed_in_same_night])
    ten = score(raw, [SameBedInSameNight(weight=10)])
    default_weight = raw[same_bed_in_same_night.name]["weight"]
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
