"""제약 스팟체크: C++ full recompute(calculate_score)를 손계산 값과 대조.

Java BedAllocationConstraintProviderTest 의 6박(penalizesBy 6) 케이스에
제약 가중치를 곱한 기대값이다. 전체 제약별 검증은 C++ 테스트
(cpp/test/ba_test.cpp)가 담당한다.
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
        5,  # 6박: Java 테스트의 ZERO_NIGHT..FIVE_NIGHT
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
    """Java 원본의 유닛테스트로 고정된 의미론: 병실에 '있는' 필수 장비 수로
    가중한다 — 부분 보유는 벌점, 전무하면 0."""
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
            make_stay(0, 0, specialty="spec1"),  # 우선순위 2 -> -60 soft
            make_stay(1, 1, specialty="spec2"),  # 우선순위 1 -> 0
            make_stay(2, 2, specialty="spec3"),  # 미제공 -> -60 soft
        ],
        departments=departments,
    )
    # 재원 3건이 같은 병실(정원 3, ANY_GENDER)의 서로 다른 병상: hard 0.
    score = ba.calculate_score(plan)
    assert (score.hard, score.medium, score.soft) == (0, 0, -120)
