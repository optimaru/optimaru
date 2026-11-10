#ifndef FL_INCREMENTAL_CALCULATOR_HPP
#define FL_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "fl/domain.hpp"
#include "optimaru/score/objective.hpp"

namespace fl
{

// The facility-location implementation of the engine's
// optimaru::Objective: one hand-written incremental calculator for all
// three constraints.
//
// Every constraint is a function of per-facility aggregates, so a single
// variable change is O(1):
//   - hard 1 (facilityCapacityCost): contribution(f) = max(0, usedDemand_f -
//     capacity_f) while facility f serves >= 1 consumer, else 0 (an
//     unused facility is never penalized). Moving one consumer in/out changes
//     only that facility's term.
//   - soft 2 (facilitySetupCost): setupCost_f enters when consumerCnt_f goes
//     0 -> 1 and leaves when it drops back to 0.
//   - soft 3 (distanceFromFacilityCost): one precomputed matrix lookup per
//     change.
//
// before_assign retracts the consumer's contribution while
// `facility` still holds the OLD value; after_assign inserts the NEW
// value. Unassigned consumers (facility == -1) contribute nothing, so the
// construction heuristic can score partially initialized solutions. The
// full-recompute reference is ScoreCalculator::score (checked in
// the tests and under ScoreChecks::ScoreChecks::Full).
class FacilityLocationIncrementalScoreCalculator final
    : public optimaru::Objective<FacilityLocationProblem>
{
public:
    void
    rebuild(FacilityLocationProblem &solution) override;
    void before_assign(int consumerIndex) override;
    void after_assign(int consumerIndex) override;
    Score score() override;

private:
    FacilityLocationProblem *problem_ = nullptr;
    std::vector<int> assign_;  // facility per consumer (-1 == unassigned)

    // Per-facility aggregates (same shapes as the full recompute's locals).
    std::vector<std::int64_t> usedDemand_;
    std::vector<int> consumerCnt_;

    // Running totals (all counted positive; signs/weights applied in
    // score).
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
