#ifndef BA_SCORE_CALCULATOR_HPP
#define BA_SCORE_CALCULATOR_HPP

#include <cstdint>

#include "ba/domain.hpp"

namespace ba
{

// The bed-allocation constraints as one monolithic calculator.
//
// score does a full recompute — the reference the incremental
// calculator is checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (hard / medium / soft; every one penalizes stay-nights), the
// default assembly of ba/constraints.hpp at the components' kDefaultWeight
// (the numbers below; ba/constraints/*.hpp is where they live):
//   hard 1. bedConflictCost                -1000 * sameNightCount per pair of
//                                          stays in the same bed with
//                                          overlapping nights
//   hard 2. femaleInMaleRoomCost           -50 * nightCount per FEMALE stay
//                                          in a MALE_ONLY room
//   hard 3. maleInFemaleRoomCost           -50 * nightCount per MALE stay in
//                                          a FEMALE_ONLY room
//   hard 4. mixedGenderRoomCost            -1000 * sameNightCount per mixed-
//                                          gender pair in one SAME_GENDER
//                                          room with overlapping nights
//   hard 5. departmentMinAgeCost           -100 * nightCount per stay younger
//                                          than its department's minimumAge
//   hard 6. departmentMaxAgeCost           -100 * nightCount per stay older
//                                          than its department's maximumAge
//   hard 7. requiredPatientEquipmentCost   when the room misses at least one
//                                          required equipment:
//                                          -50 * nightCount * (# of required
//                                          equipment the room DOES have) —
//                                          the present ones are counted (the
//                                          unit tests pin this down), so a
//                                          room with none of the required
//                                          equipment costs 0
//   medium 8. unassignedStaysCost          -1 * nightCount per stay left in
//                                          the "no bed" sentinel (left
//                                          unassigned); folded into the
//                                          engine soft level by
//                                          toEngineScore (kMediumFold)
//   soft 9. preferredMaxRoomCapacityCost   -8 * nightCount when the room is
//                                          larger than the patient prefers
//   soft 10. departmentSpecialtyCost       -10 * nightCount when the
//                                          department does not offer the
//                                          stay's specialty
//   soft 11. departmentSpecialtyPriorityCost
//                                          -10 * (priority - 1) * nightCount
//                                          when the department offers it at
//                                          priority > 1 (skipped when not
//                                          offered)
//   soft 12. preferredPatientEquipmentCost -50 * nightCount * (# of preferred
//                                          equipment the room lacks)
//
// Uninitialized stays (bed == -1) contribute nothing: the construction
// heuristic scores partially initialized solutions.
class ScoreCalculator
{
public:
    // Full recompute; also stores plan.score.
    static Score score(BedPlan &plan);

    // The same recompute in the engine's score type (see toEngineScore).
    static optimaru::Score calculateEngineScore(BedPlan &plan)
    {
        return toEngineScore(score(plan));
    }

    // Interns the equipment names into dense ids, flattens the room
    // equipment table and resolves each stay's specialty priority per
    // department. Idempotent (guarded by BedPlan::prepared). Must run before
    // scoring; score/solve call it.
    static void prepare(BedPlan &plan);

    // --- shared building blocks (positive penalty units) ---

    // Per-stay penalties of assigning stay `stayIndex` to value
    // `value` (a real bed or the "no bed" sentinel; -1 contributes nothing).
    struct SingleImpact
    {
        std::int64_t hard = 0;
        std::int64_t medium = 0;
        std::int64_t soft = 0;
    };
    static SingleImpact
    singleStayImpact(const BedPlan &plan, int stayIndex, int value);

    // hard 1 (bedConflictCost): BedConflictCost::kDefaultWeight (1000) *
    // sameNightCount for two stays in one bed. Defined in
    // score_calculator.cpp, which reads the component constant (the
    // components include this header, so it cannot include theirs).
    static std::int64_t bedConflictPairPenalty(const Stay &a, const Stay &b);

    // hard 4 (mixedGenderRoomCost): MixedGenderRoomCost::kDefaultWeight
    // (1000) * sameNightCount for a mixed-gender pair; both stays must
    // already share one SAME_GENDER room (the caller checks the room).
    static std::int64_t mixedGenderPairPenalty(const Stay &a, const Stay &b);
};

}  // namespace ba

#endif  // BA_SCORE_CALCULATOR_HPP
