"""Constraint spot checks: the C++ full recompute (calculate_score) against hand-computed values.

The expected values are the 6-night (penalizesBy 6) cases of Java
BedAllocationConstraintProviderTest times the constraint weights. Full per-constraint
verification lives in the C++ tests (cpp/test/ba_test.cpp).
"""

import bed_allocation as ba


def make_plan(rooms, beds, stays, departments=None):
    if departments is None:
        departments = [ba.Department("d0", "Dept", {"default": 1})]
    return ba.BedPlan(departments, rooms, beds, stays)


def make_stay(index, bed, *, gender=ba.Gender.MALE, specialty="default", **kwargs):
    return ba.Stay(
        f"s{index}",
        f"patient-{index}",
        gender,
        30,
        0,
        5,  # 6 nights: ZERO_NIGHT..FIVE_NIGHT of the Java test
        specialty,
        bed=bed,
        **kwargs,
    )


def test_female_in_male_room():
    plan = make_plan(
        [ba.Room("r0", "r0", gender_limitation=ba.GenderLimitation.MALE_ONLY)],
        [ba.Bed("b0", 0)],
        [make_stay(0, 0, gender=ba.Gender.FEMALE)],
    )
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (-300, 0, 0)


def test_same_bed_in_same_night():
    plan = make_plan(
        [ba.Room("r0", "r0")],
        [ba.Bed("b0", 0)],
        [make_stay(0, 0), make_stay(1, 0)],
    )
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (-6000, 0, 0)


def test_no_bed_penalized_on_medium_level():
    plan = make_plan(
        [ba.Room("r0", "r0")],
        [ba.Bed("b0", 0)],
        [make_stay(0, 1)],  # no_bed_value() == 1
    )
    assert plan.no_bed_value() == 1
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (0, -6, 0)
    assert score.is_feasible()


def test_uninitialized_contributes_nothing():
    plan = make_plan([ba.Room("r0", "r0")], [ba.Bed("b0", 0)], [make_stay(0, -1)])
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (0, 0, 0)


def test_required_equipment_counts_present_ones():
    """Semantics pinned by the Java original's unit test: weighted by the required
    equipment the room HAS — partial coverage is penalized, none at all is 0."""
    room_partial = ba.Room("r0", "r0", equipment=["telemetry"])
    room_none = ba.Room("r1", "r1")
    plan = make_plan(
        [room_partial, room_none],
        [ba.Bed("b0", 0), ba.Bed("b1", 1)],
        [
            make_stay(0, 0, patient_required_equipment=["television", "telemetry"]),
            make_stay(1, 1, patient_required_equipment=["television"]),
        ],
    )
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (-300, 0, 0)


def test_preferred_equipment_counts_missing_ones():
    plan = make_plan(
        [ba.Room("r0", "r0", equipment=["telemetry"])],
        [ba.Bed("b0", 0)],
        [make_stay(0, 0, patient_preferred_equipment=["television", "telemetry"])],
    )
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (0, 0, -300)


def test_department_specialty_and_priority():
    departments = [ba.Department("d0", "Dept", {"spec1": 2, "spec2": 1})]
    plan = make_plan(
        [ba.Room("r0", "r0", capacity=3)],
        [ba.Bed("b0", 0), ba.Bed("b1", 0), ba.Bed("b2", 0)],
        [
            make_stay(0, 0, specialty="spec1"),  # priority 2 -> -60 soft
            make_stay(1, 1, specialty="spec2"),  # priority 1 -> 0
            make_stay(2, 2, specialty="spec3"),  # not offered -> -60 soft
        ],
        departments=departments,
    )
    # Three stays on different beds of the same room (capacity 3, ANY_GENDER): hard 0.
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (0, 0, -120)
