#ifndef BA_SCORE_CALCULATOR_HPP
#define BA_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "ba/domain.hpp"

namespace ba
{

// Port of the reference bed-allocation BedAllocationConstraintProvider.
//
// calculateScore does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under FULL_ASSERT).
//
// Constraints (HardMediumSoftScore; every one penalizes stay-nights):
//   hard 1. sameBedInSameNight             -1000 * sameNightCount per pair of
//                                          stays in the same bed with
//                                          overlapping nights
//   hard 2. femaleInMaleRoom               -50 * nightCount per FEMALE stay
//                                          in a MALE_ONLY room
//   hard 3. maleInFemaleRoom               -50 * nightCount per MALE stay in
//                                          a FEMALE_ONLY room
//   hard 4. differentGenderInSameGenderRoomInSameNight
//                                          -1000 * sameNightCount per mixed-
//                                          gender pair in one SAME_GENDER
//                                          room with overlapping nights
//   hard 5. departmentMinimumAge           -100 * nightCount per stay younger
//                                          than its department's minimumAge
//   hard 6. departmentMaximumAge           -100 * nightCount per stay older
//                                          than its department's maximumAge
//   hard 7. requiredPatientEquipment       when the room misses at least one
//                                          required equipment:
//                                          -50 * nightCount * (# of required
//                                          equipment the room DOES have) —
//                                          the quickstart counts the present
//                                          ones, verbatim from the Java
//                                          ConstraintProvider (its unit test
//                                          pins this down), so a room with
//                                          none of the required equipment
//                                          costs 0
//   medium 8. assignEveryPatientToABed     -1 * nightCount per stay left in
//                                          the "no bed" sentinel (the
//                                          quickstart's null bed)
//   soft 9. preferredMaximumRoomCapacity   -8 * nightCount when the room is
//                                          larger than the patient prefers
//   soft 10. departmentSpecialty           -10 * nightCount when the
//                                          department does not offer the
//                                          stay's specialty
//   soft 11. departmentSpecialtyNotFirstPriority
//                                          -10 * (priority - 1) * nightCount
//                                          when the department offers it at
//                                          priority > 1 (skipped when not
//                                          offered — Java would throw there)
//   soft 12. preferredPatientEquipment     -50 * nightCount * (# of preferred
//                                          equipment the room lacks)
//
// Uninitialized stays (bed == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    // Full recompute; also stores plan.score.
    static Score calculateScore(BedPlan &plan);

    // The same recompute in the engine's score type (see toEngineScore).
    static optimaru::HardSoftScore calculateEngineScore(BedPlan &plan)
    {
        return toEngineScore(calculateScore(plan));
    }

    // Interns the equipment names into dense ids, flattens the room
    // equipment table and resolves each stay's specialty priority per
    // department. Idempotent (guarded by BedPlan::prepared). Must run before
    // scoring; calculateScore/solve call it.
    static void prepare(BedPlan &plan);

    // --- shared building blocks (positive penalty units) ---

    // Per-stay penalties of assigning stay `stayIndex` to planning value
    // `value` (a real bed or the "no bed" sentinel; -1 contributes nothing).
    struct SingleImpact
    {
        std::int64_t hard = 0;
        std::int64_t medium = 0;
        std::int64_t soft = 0;
    };
    static SingleImpact
    singleStayImpact(const BedPlan &plan, int stayIndex, int value);

    // hard 1: 1000 * sameNightCount for two stays in one bed.
    static std::int64_t sameBedPairPenalty(const Stay &a, const Stay &b)
    {
        return 1000LL * a.sameNightCount(b);
    }

    // hard 4: 1000 * sameNightCount for a mixed-gender pair; both stays must
    // already share one SAME_GENDER room (the caller checks the room).
    static std::int64_t mixedGenderPairPenalty(const Stay &a, const Stay &b)
    {
        if (a.patientGender == b.patientGender)
            return 0;
        return 1000LL * a.sameNightCount(b);
    }
};

}  // namespace ba

#endif  // BA_SCORE_CALCULATOR_HPP
