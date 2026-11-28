#ifndef BS_SCORE_CALCULATOR_HPP
#define BS_SCORE_CALCULATOR_HPP

#include "bs/model/domain.hpp"

namespace bs {

// The box-sizing score as one monolithic function.
//
// score does a full recompute: the reference the incremental calculator is
// checked against (in the tests and under ScoreChecks::Full).
//
// Constraints (Score), the default assembly of bs/constraints.hpp at the
// components' kDefaultWeight:
//   hard 1. parcelFitCost      -(length by which a parcel exceeds its box,
//                              summed over the three sorted sides)
//   hard 2. boxWeightCost      -(parcel weight above its box's maxWeight)
//   hard 3. boxTypeLimitCost   -(distinct boxes in use above maxBoxTypes)
//   soft 4. voidVolumeCost     -(quantity * (box volume - parcel volume))
//   soft 5. boxCost            -(quantity * the box's cost)
//   soft 6. boxTypeCost        weight 0: scores nothing until a caller picks
//                              it (the plugin path)
// Unassigned parcels (box == -1) contribute nothing: the construction scores
// partially initialized solutions.
class ScoreCalculator {
public:
    static Score score(BoxSizingProblem& problem);

    // Validates the facts and precomputes the sorted sides and the volumes,
    // so scoring reads are array lookups. Idempotent (guarded by
    // BoxSizingProblem::prepared); the parcels' boxes are range-checked on
    // every call, since they stay writable. Must run before scoring;
    // score/solve call it.
    //
    // Throws std::invalid_argument naming the box or the parcel for: no
    // candidate box, a repeated or empty id, a side outside 1..kMaxSide, a
    // weight or a maxWeight outside 0..kMaxWeight, a quantity below 1, a cost
    // that is negative or not finite, maxBoxTypes below 1, shipped volume
    // that could pass kMaxVolume, and a box index outside -1..boxCount-1.
    static void prepare(BoxSizingProblem& problem);

    // What a solve refuses on top of prepare(), because no assignment could
    // ever be feasible: a parcel that fits none of the candidates (by size and
    // weight), and more required boxes than maxBoxTypes allows. The message
    // describes the first such parcel and names up to five more by id, with
    // their count. Needs prepare().
    static void requireSolvable(const BoxSizingProblem& problem);

    // The largest side and weight a fact may have, and the bound on the
    // shipped volume (the largest box volume times the summed quantities):
    // below it every volume sum is an exact double.
    static constexpr std::int64_t kMaxSide = 2000000;
    static constexpr std::int64_t kMaxWeight = 1000000000000;     // 1e12
    static constexpr std::int64_t kMaxVolume = 9007199254740992;  // 2^53
};

}  // namespace bs

#endif  // BS_SCORE_CALCULATOR_HPP
