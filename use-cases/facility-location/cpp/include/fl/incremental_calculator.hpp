#ifndef FL_INCREMENTAL_CALCULATOR_HPP
#define FL_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "fl/domain.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace fl
{

// The facility-location implementation of the engine's
// optimaru::IncrementalScoreCalculator — the C++ analog of implementing
// Java's IncrementalScoreCalculator instead of a ConstraintProvider.
//
// Every constraint is a function of per-facility aggregates, so a single
// variable change is O(1):
//   - hard 1 (facilityCapacity): contribution(f) = max(0, usedDemand_f -
//     capacity_f) while facility f serves >= 1 consumer, else 0 (Java's
//     groupBy only sees used facilities). Moving one consumer in/out changes
//     only that facility's term.
//   - soft 2 (facilitySetupCost): setupCost_f enters when consumerCnt_f goes
//     0 -> 1 and leaves when it drops back to 0.
//   - soft 3 (distanceFromFacility): one precomputed matrix lookup per
//     change.
//
// beforeVariableChanged retracts the consumer's contribution while
// `facility` still holds the OLD value; afterVariableChanged inserts the NEW
// value. Unassigned consumers (facility == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::calculateScore (checked in
// the tests and under EnvironmentMode::FULL_ASSERT).
class FacilityLocationIncrementalScoreCalculator final
    : public optimaru::IncrementalScoreCalculator<FacilityLocationProblem>
{
public:
    void
    resetWorkingSolution(FacilityLocationProblem &workingSolution) override;
    void beforeVariableChanged(int consumerIndex) override;
    void afterVariableChanged(int consumerIndex) override;
    Score calculateScore() override;

private:
    FacilityLocationProblem *problem_ = nullptr;
    std::vector<int> assign_;  // facility per consumer (-1 == unassigned)

    // Per-facility aggregates (same shapes as the full recompute's locals).
    std::vector<std::int64_t> usedDemand_;
    std::vector<int> consumerCnt_;

    // Running totals (all counted positive; signs/weights applied in
    // calculateScore).
    std::int64_t overCapacity_ = 0;   // hard 1
    std::int64_t usedSetupCost_ = 0;  // soft 2 (unweighted)
    std::int64_t distance_ = 0;       // soft 3 (unweighted meters)

    void insert(int consumerIndex, int facilityIndex);
    void retract(int consumerIndex, int facilityIndex);

    // hard 1 term of one facility given its current aggregates.
    std::int64_t capacityPenalty(int facilityIndex) const
    {
        if (consumerCnt_[facilityIndex] == 0)
            return 0;
        const std::int64_t over
            = usedDemand_[facilityIndex]
              - problem_->facilities[facilityIndex].capacity;
        return over > 0 ? over : 0;
    }
};

}  // namespace fl

#endif  // FL_INCREMENTAL_CALCULATOR_HPP
