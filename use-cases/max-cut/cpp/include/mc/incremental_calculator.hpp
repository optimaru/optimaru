#ifndef MC_INCREMENTAL_CALCULATOR_HPP
#define MC_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "mc/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace mc
{

// The max-cut implementation of the engine's
// optimaru::IncrementalScoreCalculator: the monolithic fast path of the
// default constraints (mc/score_calculator.hpp lists them).
//
// It keeps a copy of the sides and the raw totals of every constraint — the
// fixed vertices off their side, the split same-side pairs, the joined
// different-side pairs, the cut weight and the size of each side — so
// moving one vertex costs O(its degree + its pair degree); the size and
// imbalance terms are O(1) at calculateScore.
//
// beforeVariableChanged is a no-op (the copy still holds the old side);
// afterVariableChanged compares the vertex's new side with the copy and, if
// it changed, retracts the vertex's terms at the old side and inserts them at
// the new one. -1 (unassigned) follows the rule of mc/domain.hpp. The
// full-recompute reference is ScoreCalculator::calculateScore (checked in the
// tests and under EnvironmentMode::FULL_ASSERT). Requires
// ScoreCalculator::prepare(problem).
class MaxCutIncrementalScoreCalculator final : public optimaru::IncrementalScoreCalculator<MaxCutProblem>
{
public:
    // The raw cost of every default constraint, read from the running
    // totals: fixed / same / different summed over both sides (the
    // RawCosts::*Total() of mc/score_calculator.hpp), the size bounds per
    // side, the imbalance and the cut weight (+cut; cutWeightCost is -cut).
    // calculateScore() weighs exactly these; the tests compare them with
    // ScoreCalculator::rawCosts after every move.
    struct Totals
    {
        std::int64_t fixed = 0;
        std::int64_t same = 0;
        std::int64_t different = 0;
        std::int64_t size[kSideCount] = {0, 0};
        std::int64_t imbalance = 0;
        std::int64_t cut = 0;
    };

    void resetWorkingSolution(MaxCutProblem &workingSolution) override;
    void beforeVariableChanged(int vertex) override;
    void afterVariableChanged(int vertex) override;
    Score calculateScore() override;
    Totals totals() const;

private:
    MaxCutProblem *problem_ = nullptr;
    std::vector<int> side_;  // the sides as last seen

    // Running raw totals, all >= 0 (signs and weights applied in
    // calculateScore).
    std::int64_t count_[kSideCount] = {0, 0};  // partitionSizeBoundsCost / partitionImbalanceCost
    std::int64_t fixed_ = 0;                   // fixedPartitionCost
    std::int64_t sameSplit_ = 0;               // samePartitionPairsCost
    std::int64_t diffJoined_ = 0;              // differentPartitionPairsCost
    std::int64_t cut_ = 0;                     // cutWeightCost (as +cut)

    // Adds (sign = +1) or removes (sign = -1) every term that involves
    // `vertex`, evaluated at side_[vertex] (which may be -1: a same-side pair
    // whose second endpoint is unassigned is split); the other vertices are
    // read from side_. A move is apply(-1), the side change, apply(+1).
    void apply(int vertex, int sign);
};

}  // namespace mc

#endif  // MC_INCREMENTAL_CALCULATOR_HPP
